/*
 * QEMU OpenTitan Sensor controller device
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
 * Note: for now, only a minimalist subset of Power Manager device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_sensor.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_IO_STATUS_CHANGE, 0u, 1u)
    SHARED_FIELD(INTR_INIT_STATUS_CHANGE, 1u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, RECOV_ALERT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_ALERT, 1u, 1u)
REG32(CFG_REGWEN, 0x10u)
    FIELD(CFG_REGWEN, EN, 0u, 1u)
REG32(ALERT_TRIG, 0x14u)
    FIELD(ALERT_TRIG, VAL_0, 0u, 1u)
    FIELD(ALERT_TRIG, VAL_1, 1u, 1u)
    FIELD(ALERT_TRIG, VAL_2, 2u, 1u)
    FIELD(ALERT_TRIG, VAL_3, 3u, 1u)
    FIELD(ALERT_TRIG, VAL_4, 4u, 1u)
    FIELD(ALERT_TRIG, VAL_5, 5u, 1u)
    FIELD(ALERT_TRIG, VAL_6, 6u, 1u)
    FIELD(ALERT_TRIG, VAL_7, 7u, 1u)
    FIELD(ALERT_TRIG, VAL_8, 8u, 1u)
    FIELD(ALERT_TRIG, VAL_9, 9u, 1u)
    FIELD(ALERT_TRIG, VAL_10, 10u, 1u)
REG32(FATAL_ALERT_EN, 0x18u)
    FIELD(FATAL_ALERT_EN, VAL_0, 0u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_1, 1u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_2, 2u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_3, 3u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_4, 4u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_5, 5u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_6, 6u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_7, 7u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_8, 8u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_9, 9u, 1u)
    FIELD(FATAL_ALERT_EN, VAL_10, 10u, 1u)
REG32(RECOV_ALERT, 0x1cu)
    FIELD(RECOV_ALERT, VAL_0, 0u, 1u)
    FIELD(RECOV_ALERT, VAL_1, 1u, 1u)
    FIELD(RECOV_ALERT, VAL_2, 2u, 1u)
    FIELD(RECOV_ALERT, VAL_3, 3u, 1u)
    FIELD(RECOV_ALERT, VAL_4, 4u, 1u)
    FIELD(RECOV_ALERT, VAL_5, 5u, 1u)
    FIELD(RECOV_ALERT, VAL_6, 6u, 1u)
    FIELD(RECOV_ALERT, VAL_7, 7u, 1u)
    FIELD(RECOV_ALERT, VAL_8, 8u, 1u)
    FIELD(RECOV_ALERT, VAL_9, 9u, 1u)
    FIELD(RECOV_ALERT, VAL_10, 10u, 1u)
REG32(FATAL_ALERT, 0x20u)
    FIELD(FATAL_ALERT, VAL_0, 0u, 1u)
    FIELD(FATAL_ALERT, VAL_1, 1u, 1u)
    FIELD(FATAL_ALERT, VAL_2, 2u, 1u)
    FIELD(FATAL_ALERT, VAL_3, 3u, 1u)
    FIELD(FATAL_ALERT, VAL_4, 4u, 1u)
    FIELD(FATAL_ALERT, VAL_5, 5u, 1u)
    FIELD(FATAL_ALERT, VAL_6, 6u, 1u)
    FIELD(FATAL_ALERT, VAL_7, 7u, 1u)
    FIELD(FATAL_ALERT, VAL_8, 8u, 1u)
    FIELD(FATAL_ALERT, VAL_9, 9u, 1u)
    FIELD(FATAL_ALERT, VAL_10, 10u, 1u)
    FIELD(FATAL_ALERT, VAL_11, 11u, 1u)
REG32(STATUS, 0x24u)
    FIELD(STATUS, AST_INIT_DONE, 0u, 1u)
    FIELD(STATUS, IO_POK, 1u, 2u)
/* clang-format on */

#define PARAM_NUM_IO_RAILS 2

#define INTR_MASK (INTR_IO_STATUS_CHANGE_MASK | INTR_INIT_STATUS_CHANGE_MASK)
#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_ALERT_MASK | R_ALERT_TEST_FATAL_ALERT_MASK)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_STATUS)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),     REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),      REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CFG_REGWEN),     REG_NAME_ENTRY(ALERT_TRIG),
    REG_NAME_ENTRY(FATAL_ALERT_EN), REG_NAME_ENTRY(RECOV_ALERT),
    REG_NAME_ENTRY(FATAL_ALERT),    REG_NAME_ENTRY(STATUS),
};
#undef REG_NAME_ENTRY

struct OtSensorState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[2u];
    IbexIRQ alert;

    uint32_t *regs;
};

static void ot_sensor_update_irqs(OtSensorState *s)
{
    uint32_t levels = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        int level = (int)(bool)(levels & (1u << ix));
        ibex_irq_set(&s->irqs[ix], level);
    }
}

static uint64_t ot_sensor_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSensorState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CFG_REGWEN:
    case R_ALERT_TRIG:
    case R_FATAL_ALERT_EN:
    case R_RECOV_ALERT:
    case R_FATAL_ALERT:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        if (!s->regs[reg]) {
            /* fake init: reports initialized */
            s->regs[reg] |= R_STATUS_AST_INIT_DONE_MASK;
        }
        val32 = s->regs[reg];
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
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
    trace_ot_sensor_io_read_out((unsigned)addr, REG_NAME(reg), (uint64_t)val32,
                                pc);

    return (uint64_t)val32;
};

static void ot_sensor_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtSensorState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_sensor_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_sensor_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_sensor_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_sensor_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case R_CFG_REGWEN:
    case R_ALERT_TRIG:
    case R_FATAL_ALERT_EN:
    case R_RECOV_ALERT:
    case R_FATAL_ALERT:
        qemu_log_mask(LOG_UNIMP,
                      "Unimplemented register 0x%02" HWADDR_PRIx " (%s)\n",
                      addr, REG_NAME(reg));
        break;
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_sensor_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_sensor_regs_ops = {
    .read = &ot_sensor_regs_read,
    .write = &ot_sensor_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_sensor_reset(DeviceState *dev)
{
    OtSensorState *s = OT_SENSOR(dev);

    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_CFG_REGWEN] = 0x1u;

    ot_sensor_update_irqs(s);
    ibex_irq_set(&s->alert, 0);
}

static void ot_sensor_init(Object *obj)
{
    OtSensorState *s = OT_SENSOR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_sensor_regs_ops, s, TYPE_OT_SENSOR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
}

static void ot_sensor_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_sensor_reset;
    device_class_set_props(dc, ot_sensor_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_sensor_info = {
    .name = TYPE_OT_SENSOR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSensorState),
    .instance_init = &ot_sensor_init,
    .class_init = &ot_sensor_class_init,
};

static void ot_sensor_register_types(void)
{
    type_register_static(&ot_sensor_info);
}

type_init(ot_sensor_register_types)
