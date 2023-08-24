/*
 * QEMU OpenTitan SRAM controller
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Note: scrambling features are not supported
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/memalign.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/opentitan/ot_sram_ctrl.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/block-backend.h"
#include "trace.h"

#define PARAM_NUM_ALERTS 1

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_ERROR, 0u, 1u)
REG32(STATUS, 0x4u)
    FIELD(STATUS, BUS_INTEG_ERROR, 0u, 1u)
    FIELD(STATUS, INIT_ERROR, 1u, 1u)
    FIELD(STATUS, ESCALATED, 2u, 1u)
    FIELD(STATUS, SCR_KEY_VALID, 3u, 1u)
    FIELD(STATUS, SCR_KEY_SEED_VALID, 4u, 1u)
    FIELD(STATUS, INIT_DONE, 5u, 1u)
REG32(EXEC_REGWEN, 0x8u)
    FIELD(EXEC_REGWEN, EN, 0u, 1u)
REG32(EXEC, 0xcu)
    FIELD(EXEC, EN, 0u, 4u)
REG32(CTRL_REGWEN, 0x10u)
    FIELD(CTRL_REGWEN_CTRL, REGWEN, 0u, 1u)
REG32(CTRL, 0x14u)
    FIELD(CTRL, RENEW_SCR_KEY, 0u, 1u)
    FIELD(CTRL, INIT, 1u, 1u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_CTRL)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),  REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(EXEC_REGWEN), REG_NAME_ENTRY(EXEC),
    REG_NAME_ENTRY(CTRL_REGWEN), REG_NAME_ENTRY(CTRL),
};
#undef REG_NAME_ENTRY

struct OtSramCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    MemoryRegion mmio;
    IbexIRQ alert;

    uint32_t regs[REGS_COUNT];
    bool otp_ifetch;
    bool cfg_ifetch;

    OtOTPState *otp_ctrl;
    uint32_t size;
};

static uint64_t ot_sram_ctrl_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSramCtrlState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_STATUS:
    case R_EXEC_REGWEN:
    case R_EXEC:
    case R_CTRL_REGWEN:
    case R_CTRL:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_sram_ctrl_io_read_out((unsigned)addr, REG_NAME(reg),
                                   (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_sram_ctrl_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtSramCtrlState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_sram_ctrl_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_ERROR_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case R_EXEC_REGWEN:
        val32 &= R_EXEC_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_EXEC:
        if (s->regs[R_EXEC_REGWEN]) {
            val32 &= R_EXEC_EN_MASK;
            s->regs[val32] = val32;
            if ((s->regs[val32] == OT_MULTIBITBOOL4_TRUE) && s->otp_ifetch) {
                s->cfg_ifetch = true;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: R_EXEC protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_CTRL_REGWEN:
        val32 &= R_CTRL_REGWEN_CTRL_REGWEN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CTRL:
        if (s->regs[R_CTRL_REGWEN]) {
            val32 &= R_CTRL_INIT_MASK | R_CTRL_RENEW_SCR_KEY_MASK;
            s->regs[val32] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: R_CTRL protected w/ REGWEN\n",
                          __func__);
        }
        break;
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x02%" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};


static Property ot_sram_ctrl_properties[] = {
    DEFINE_PROP_LINK("otp_ctrl", OtSramCtrlState, otp_ctrl, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_UINT32("size", OtSramCtrlState, size, 0u),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_sram_ctrl_regs_ops = {
    .read = &ot_sram_ctrl_regs_read,
    .write = &ot_sram_ctrl_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_sram_ctrl_reset(DeviceState *dev)
{
    OtSramCtrlState *s = OT_SRAM_CTRL(dev);

    memset(s->regs, 0, REGS_SIZE);

    /* note: SRAM storage is -not- reset */

    s->regs[R_EXEC_REGWEN] = 0x1u;
    s->regs[R_EXEC] = 0x9u;
    s->regs[R_CTRL_REGWEN] = 0x1u;

    OtOTPStateClass *oc =
        OBJECT_GET_CLASS(OtOTPStateClass, s->otp_ctrl, TYPE_OT_OTP);
    s->otp_ifetch = oc->get_hw_cfg(s->otp_ctrl)->en_sram_ifetch;
    s->cfg_ifetch = 0u; /* not used for now */
}

static void ot_sram_ctrl_realize(DeviceState *dev, Error **errp)
{
    OtSramCtrlState *s = OT_SRAM_CTRL(dev);

    assert(s->otp_ctrl);
    assert(s->size);

    MemoryRegion *mr = &s->mem;
    memory_region_init_ram_nomigrate(mr, OBJECT(dev), TYPE_OT_SRAM_CTRL "-mem",
                                     s->size, errp);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), mr);
}

static void ot_sram_ctrl_init(Object *obj)
{
    OtSramCtrlState *s = OT_SRAM_CTRL(obj);

    memory_region_init_io(&s->mmio, obj, &ot_sram_ctrl_regs_ops, s,
                          TYPE_OT_SRAM_CTRL "-regs", REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
}

static void ot_sram_ctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_sram_ctrl_reset;
    dc->realize = &ot_sram_ctrl_realize;
    device_class_set_props(dc, ot_sram_ctrl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_sram_ctrl_info = {
    .name = TYPE_OT_SRAM_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSramCtrlState),
    .instance_init = &ot_sram_ctrl_init,
    .class_init = &ot_sram_ctrl_class_init,
};

static void ot_sram_ctrl_register_types(void)
{
    type_register_static(&ot_sram_ctrl_info);
}

type_init(ot_sram_ctrl_register_types)
