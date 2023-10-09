/*
 * QEMU OpenTitan EarlGrey Analog Sensor Top device
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
 * Note: for now, only a minimalist subset of Analog Sensor Top device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_ast_earlgrey.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(REGA0, 0x0u)
REG32(REGA1, 0x4u)
REG32(REGA2, 0x8u)
REG32(REGA3, 0xcu)
REG32(REGA4, 0x10u)
REG32(REGA5, 0x14u)
REG32(REGA6, 0x18u)
REG32(REGA7, 0x1cu)
REG32(REGA8, 0x20u)
REG32(REGA9, 0x24u)
REG32(REGA10, 0x28u)
REG32(REGA11, 0x2cu)
REG32(REGA12, 0x30u)
REG32(REGA13, 0x34u)
REG32(REGA14, 0x38u)
REG32(REGA15, 0x3cu)
REG32(REGA16, 0x40u)
REG32(REGA17, 0x44u)
REG32(REGA18, 0x48u)
REG32(REGA19, 0x4cu)
REG32(REGA20, 0x50u)
REG32(REGA21, 0x54u)
REG32(REGA22, 0x58u)
REG32(REGA23, 0x5cu)
REG32(REGA24, 0x60u)
REG32(REGA25, 0x64u)
REG32(REGA26, 0x68u)
REG32(REGA27, 0x6cu)
REG32(REGA28, 0x70u)
REG32(REGA29, 0x74u)
REG32(REGA30, 0x78u)
REG32(REGA31, 0x7cu)
REG32(REGA32, 0x80u)
REG32(REGA33, 0x84u)
REG32(REGA34, 0x88u)
REG32(REGA35, 0x8cu)
REG32(REGA36, 0x90u)
REG32(REGA37, 0x94u)
REG32(REGAL, 0x98u)
REG32(REGB0, 0x200u)
REG32(REGB1, 0x204u)
REG32(REGB2, 0x208u)
REG32(REGB3, 0x20cu)
REG32(REGB4, 0x210u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define RA_LAST_REG (R_REGAL)
#define REGSA_COUNT (RA_LAST_REG + 1u)
#define REGSA_SIZE  (REGSA_COUNT * sizeof(uint32_t))
#define REGA_NAME(_reg_) \
    ((((_reg_) <= REGSA_COUNT) && REGA_NAMES[_reg_]) ? REGA_NAMES[_reg_] : "?")

#define RB_LAST_REG (R_REGB4)
#define REGSB_COUNT (RB_LAST_REG + 1u)
#define REGSB_SIZE  (REGSB_COUNT * sizeof(uint32_t))
#define REGB_NAME(_reg_) \
    ((((_reg_) <= REGSB_COUNT) && REGB_NAMES[_reg_]) ? REGB_NAMES[_reg_] : "?")

#define REGS_SIZE (A_REGB4 + sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((_reg_) >= R_REGB0 ? REGB_NAME((_reg_) - (R_REGB0)) : REGA_NAME(_reg_))

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char REGA_NAMES[REGSA_COUNT][8U] = {
    REG_NAME_ENTRY(REGA0),  REG_NAME_ENTRY(REGA1),  REG_NAME_ENTRY(REGA2),
    REG_NAME_ENTRY(REGA3),  REG_NAME_ENTRY(REGA4),  REG_NAME_ENTRY(REGA5),
    REG_NAME_ENTRY(REGA6),  REG_NAME_ENTRY(REGA7),  REG_NAME_ENTRY(REGA8),
    REG_NAME_ENTRY(REGA9),  REG_NAME_ENTRY(REGA10), REG_NAME_ENTRY(REGA11),
    REG_NAME_ENTRY(REGA12), REG_NAME_ENTRY(REGA13), REG_NAME_ENTRY(REGA14),
    REG_NAME_ENTRY(REGA15), REG_NAME_ENTRY(REGA16), REG_NAME_ENTRY(REGA17),
    REG_NAME_ENTRY(REGA18), REG_NAME_ENTRY(REGA19), REG_NAME_ENTRY(REGA20),
    REG_NAME_ENTRY(REGA21), REG_NAME_ENTRY(REGA22), REG_NAME_ENTRY(REGA23),
    REG_NAME_ENTRY(REGA24), REG_NAME_ENTRY(REGA25), REG_NAME_ENTRY(REGA26),
    REG_NAME_ENTRY(REGA27), REG_NAME_ENTRY(REGA28), REG_NAME_ENTRY(REGA29),
    REG_NAME_ENTRY(REGA30), REG_NAME_ENTRY(REGA31), REG_NAME_ENTRY(REGA32),
    REG_NAME_ENTRY(REGA33), REG_NAME_ENTRY(REGA34), REG_NAME_ENTRY(REGA35),
    REG_NAME_ENTRY(REGA36), REG_NAME_ENTRY(REGA37), REG_NAME_ENTRY(REGAL),
};

static const char REGB_NAMES[REGSB_COUNT][6U] = {
    REG_NAME_ENTRY(REGB0), REG_NAME_ENTRY(REGB1), REG_NAME_ENTRY(REGB2),
    REG_NAME_ENTRY(REGB3), REG_NAME_ENTRY(REGB4),
};
#undef REG_NAME_ENTRY

struct OtASTEarlGreyState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t *regsa;
    uint32_t *regsb;
};

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

void ot_ast_eg_getrandom(void *buf, size_t len)
{
    qemu_guest_getrandom_nofail(buf, len);
}

/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static uint64_t ot_ast_eg_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtASTEarlGreyState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_REGA0:
    case R_REGA1:
    case R_REGA2:
    case R_REGA3:
    case R_REGA4:
    case R_REGA5:
    case R_REGA6:
    case R_REGA7:
    case R_REGA8:
    case R_REGA9:
    case R_REGA10:
    case R_REGA11:
    case R_REGA12:
    case R_REGA13:
    case R_REGA14:
    case R_REGA15:
    case R_REGA16:
    case R_REGA17:
    case R_REGA18:
    case R_REGA19:
    case R_REGA20:
    case R_REGA21:
    case R_REGA22:
    case R_REGA23:
    case R_REGA24:
    case R_REGA25:
    case R_REGA26:
    case R_REGA27:
    case R_REGA28:
    case R_REGA29:
    case R_REGA30:
    case R_REGA31:
    case R_REGA32:
    case R_REGA33:
    case R_REGA34:
    case R_REGA35:
    case R_REGA36:
    case R_REGA37:
    case R_REGAL:
        val32 = s->regsa[reg];
        break;
    case R_REGB0:
    case R_REGB1:
    case R_REGB2:
    case R_REGB3:
    case R_REGB4:
        val32 = s->regsb[reg - R_REGB0];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_ast_eg_io_read_out((unsigned)addr, REG_NAME(reg), (uint64_t)val32,
                                pc);

    return (uint64_t)val32;
};

static void ot_ast_eg_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtASTEarlGreyState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_ast_eg_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_REGA0:
    case R_REGA1:
    case R_REGA2:
    case R_REGA3:
    case R_REGA4:
    case R_REGA5:
    case R_REGA6:
    case R_REGA7:
    case R_REGA8:
    case R_REGA9:
    case R_REGA10:
    case R_REGA11:
    case R_REGA12:
    case R_REGA13:
    case R_REGA14:
    case R_REGA15:
    case R_REGA16:
    case R_REGA17:
    case R_REGA18:
    case R_REGA19:
    case R_REGA20:
    case R_REGA21:
    case R_REGA22:
    case R_REGA23:
    case R_REGA24:
    case R_REGA25:
    case R_REGA26:
    case R_REGA27:
    case R_REGA28:
    case R_REGA29:
    case R_REGA30:
    case R_REGA31:
    case R_REGA32:
    case R_REGA33:
    case R_REGA34:
    case R_REGA35:
    case R_REGA36:
    case R_REGA37:
    case R_REGAL:
        s->regsa[reg] = val32 & 0xffu;
        break;
    case R_REGB0:
    case R_REGB1:
    case R_REGB2:
    case R_REGB3:
    case R_REGB4:
        s->regsb[reg - R_REGB0] = val32;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_ast_eg_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_ast_eg_regs_ops = {
    .read = &ot_ast_eg_regs_read,
    .write = &ot_ast_eg_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_ast_eg_reset(DeviceState *dev)
{
    OtASTEarlGreyState *s = OT_AST_EARLGREY(dev);

    memset(s->regsa, 0, REGSA_SIZE);
    memset(s->regsb, 0, REGSB_SIZE);

    s->regsa[R_REGA1] = 0x1u;
    s->regsa[R_REGA2] = 0x2u;
    s->regsa[R_REGA3] = 0x3u;
    s->regsa[R_REGA4] = 0x4u;
    s->regsa[R_REGA5] = 0x5u;
    s->regsa[R_REGA6] = 0x6u;
    s->regsa[R_REGA7] = 0x7u;
    s->regsa[R_REGA8] = 0x8u;
    s->regsa[R_REGA9] = 0x9u;
    s->regsa[R_REGA10] = 0xau;
    s->regsa[R_REGA11] = 0xbu;
    s->regsa[R_REGA12] = 0xcu;
    s->regsa[R_REGA13] = 0xdu;
    s->regsa[R_REGA14] = 0xeu;
    s->regsa[R_REGA15] = 0xfu;
    s->regsa[R_REGA16] = 0x10u;
    s->regsa[R_REGA17] = 0x11u;
    s->regsa[R_REGA18] = 0x12u;
    s->regsa[R_REGA19] = 0x13u;
    s->regsa[R_REGA20] = 0x14u;
    s->regsa[R_REGA21] = 0x15u;
    s->regsa[R_REGA22] = 0x16u;
    s->regsa[R_REGA23] = 0x17u;
    s->regsa[R_REGA24] = 0x18u;
    s->regsa[R_REGA25] = 0x19u;
    s->regsa[R_REGA26] = 0x1au;
    s->regsa[R_REGA27] = 0x1bu;
    s->regsa[R_REGA28] = 0x1cu;
    s->regsa[R_REGA29] = 0x1du;
    s->regsa[R_REGA30] = 0x1eu;
    s->regsa[R_REGA31] = 0x1fu;
    s->regsa[R_REGA32] = 0x20u;
    s->regsa[R_REGA33] = 0x21u;
    s->regsa[R_REGA34] = 0x22u;
    s->regsa[R_REGA35] = 0x23u;
    s->regsa[R_REGA36] = 0x24u;
    s->regsa[R_REGA37] = 0x25u;
    s->regsa[R_REGAL] = 0x26u;
}

static void ot_ast_eg_init(Object *obj)
{
    OtASTEarlGreyState *s = OT_AST_EARLGREY(obj);

    memory_region_init_io(&s->mmio, obj, &ot_ast_eg_regs_ops, s,
                          TYPE_OT_AST_EARLGREY, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regsa = g_new0(uint32_t, REGSA_COUNT);
    s->regsb = g_new0(uint32_t, REGSB_COUNT);
}

static void ot_ast_eg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_ast_eg_reset;
    device_class_set_props(dc, ot_ast_eg_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_ast_eg_info = {
    .name = TYPE_OT_AST_EARLGREY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtASTEarlGreyState),
    .instance_init = &ot_ast_eg_init,
    .class_init = &ot_ast_eg_class_init,
};

static void ot_ast_eg_register_types(void)
{
    type_register_static(&ot_ast_eg_info);
}

type_init(ot_ast_eg_register_types)
