/*
 * QEMU OpenTitan Sensor controller device for EarlGrey
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
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
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_sensor_eg.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#define NUM_ALERTS   2u
#define NUM_IO_RAILS 2u

#define NUM_ALERT_SENSOR_COUNT 11u

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
REG32(ALERT_EN_0, 0x18u)
    SHARED_FIELD(ALERT_EN_VAL, 0u, 4u)
REG32(ALERT_EN_1, 0x1cu)
REG32(ALERT_EN_2, 0x20u)
REG32(ALERT_EN_3, 0x24u)
REG32(ALERT_EN_4, 0x28u)
REG32(ALERT_EN_5, 0x2cu)
REG32(ALERT_EN_6, 0x30u)
REG32(ALERT_EN_7, 0x34u)
REG32(ALERT_EN_8, 0x38u)
REG32(ALERT_EN_9, 0x3cu)
REG32(ALERT_EN_10, 0x40u)
REG32(FATAL_ALERT_EN, 0x44u)
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
REG32(RECOV_ALERT, 0x48u)
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
REG32(FATAL_ALERT, 0x4cu)
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
REG32(STATUS, 0x50u)
    FIELD(STATUS, AST_INIT_DONE, 0u, 1u)
    FIELD(STATUS, IO_POK, 1u, 2u)
REG32(MANUAL_PAD_ATTR_REGWEN_0, 0x54u)
    SHARED_FIELD(MANUAL_PAD_ATTR_REGWEN_EN, 0u, 1u)
REG32(MANUAL_PAD_ATTR_REGWEN_1, 0x58u)
REG32(MANUAL_PAD_ATTR_REGWEN_2, 0x5cu)
REG32(MANUAL_PAD_ATTR_REGWEN_3, 0x60u)
REG32(MANUAL_PAD_ATTR_0, 0x64u)
    SHARED_FIELD(MANUAL_PAD_ATTR_PULL_EN, 2u, 1u)
    SHARED_FIELD(MANUAL_PAD_ATTR_PULL_SELECT, 3u, 1u)
    SHARED_FIELD(MANUAL_PAD_ATTR_INPUT_DISABLE, 7u, 1u)
REG32(MANUAL_PAD_ATTR_1, 0x68u)
REG32(MANUAL_PAD_ATTR_2, 0x6cu)
REG32(MANUAL_PAD_ATTR_3, 0x70u)
/* clang-format on */

#define INTR_MASK (INTR_IO_STATUS_CHANGE_MASK | INTR_INIT_STATUS_CHANGE_MASK)
#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_ALERT_MASK | R_ALERT_TEST_FATAL_ALERT_MASK)
#define ALERT_SENSOR_MASK ((1u << NUM_ALERT_SENSOR_COUNT) - 1u)
#define MANUAL_PAD_ATTR_MASK \
    (MANUAL_PAD_ATTR_PULL_EN_MASK | MANUAL_PAD_ATTR_PULL_SELECT_MASK | \
     MANUAL_PAD_ATTR_INPUT_DISABLE_MASK)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MANUAL_PAD_ATTR_3)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
/* clang-format off */
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CFG_REGWEN),
    REG_NAME_ENTRY(ALERT_TRIG),
    REG_NAME_ENTRY(ALERT_EN_0),
    REG_NAME_ENTRY(ALERT_EN_1),
    REG_NAME_ENTRY(ALERT_EN_2),
    REG_NAME_ENTRY(ALERT_EN_3),
    REG_NAME_ENTRY(ALERT_EN_4),
    REG_NAME_ENTRY(ALERT_EN_5),
    REG_NAME_ENTRY(ALERT_EN_6),
    REG_NAME_ENTRY(ALERT_EN_7),
    REG_NAME_ENTRY(ALERT_EN_8),
    REG_NAME_ENTRY(ALERT_EN_9),
    REG_NAME_ENTRY(ALERT_EN_10),
    REG_NAME_ENTRY(FATAL_ALERT_EN),
    REG_NAME_ENTRY(RECOV_ALERT),
    REG_NAME_ENTRY(FATAL_ALERT),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_REGWEN_0),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_REGWEN_1),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_REGWEN_2),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_REGWEN_3),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_0),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_1),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_2),
    REG_NAME_ENTRY(MANUAL_PAD_ATTR_3),
};
/* clang-format on */
#undef REG_NAME_ENTRY

struct OtSensorEgState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[2u];
    IbexIRQ alerts[NUM_ALERTS];
    IbexIRQ wkup;
    uint32_t ast_alert_latch;

    uint32_t *regs;
};

struct OtSensorEgClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static void ot_sensor_eg_update_irqs(OtSensorEgState *s)
{
    uint32_t levels = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        int level = (int)(bool)(levels & (1u << ix));
        ibex_irq_set(&s->irqs[ix], level);
    }
}

static uint32_t ot_sensor_eg_get_alert_en_mask(const OtSensorEgState *s)
{
    uint32_t mask = 0;
    for (unsigned ix = 0; ix <= (R_ALERT_EN_10 - R_ALERT_EN_0); ix++) {
        if ((s->regs[R_ALERT_EN_0 + ix] & ALERT_EN_VAL_MASK) !=
            OT_MULTIBITBOOL4_FALSE) {
            mask |= 1u << ix;
        }
    }
    return mask;
}

static void ot_sensor_eg_update_alerts(OtSensorEgState *s)
{
    s->ast_alert_latch |= s->regs[R_ALERT_TRIG];

    uint32_t alert_en_mask = ot_sensor_eg_get_alert_en_mask(s);
    uint32_t active_events = s->ast_alert_latch & alert_en_mask;
    uint32_t recov_events = active_events & ~s->regs[R_FATAL_ALERT_EN];
    uint32_t fatal_events = active_events & s->regs[R_FATAL_ALERT_EN];

    s->regs[R_RECOV_ALERT] |= recov_events;
    s->regs[R_FATAL_ALERT] |= fatal_events;

    uint32_t level = s->regs[R_ALERT_TEST];
    if (recov_events) {
        level |= (1u << 0u);
    }
    if (s->regs[R_FATAL_ALERT]) {
        level |= (1u << 1u);
    }

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }

    /*
     * In ast_alert.sv & sensor_ctrl.sv:
     * event_clr[i] = recov_event[i] & reg2hw.recov_alert[i].q
     * clr_p_alert = !set_p_alert && p_alert_ack
     */
    s->ast_alert_latch &= ~(recov_events & ~s->regs[R_ALERT_TRIG]);
    uint32_t rem_recov =
        s->ast_alert_latch & alert_en_mask & ~s->regs[R_FATAL_ALERT_EN];
    if (!rem_recov &&
        !(s->regs[R_ALERT_TEST] & R_ALERT_TEST_RECOV_ALERT_MASK)) {
        ibex_irq_set(&s->alerts[0], 0);
    }

    ibex_irq_set(&s->wkup, (int)(bool)((s->ast_alert_latch & alert_en_mask) ||
                                       s->regs[R_RECOV_ALERT]));
}

void ot_sensor_eg_trigger_recov_event(OtSensorEgState *s, unsigned event_idx)
{
    if (!s || event_idx > (R_ALERT_EN_10 - R_ALERT_EN_0)) {
        return;
    }
    s->ast_alert_latch |= (1u << event_idx);
    ot_sensor_eg_update_alerts(s);
}

static uint8_t ot_sensor_eg_reg_permit(hwaddr reg)
{
    switch (reg) {
    case R_ALERT_TRIG:
    case R_FATAL_ALERT_EN:
    case R_RECOV_ALERT:
    case R_FATAL_ALERT:
        return 0x3u;
    default:
        return 0x1u;
    }
}

static bool ot_sensor_eg_regs_accepts(void *opaque, hwaddr addr, unsigned size,
                                      bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    if (reg >= REGS_COUNT) {
        return false;
    }
    if (!is_write) {
        return true;
    }
    uint32_t reg_be = ((1u << size) - 1u) << (addr & 3u);
    return (ot_sensor_eg_reg_permit(reg) & ~reg_be) == 0u;
}

static uint64_t ot_sensor_eg_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSensorEgState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CFG_REGWEN:
    case R_ALERT_TRIG:
    case R_ALERT_EN_0 ... R_ALERT_EN_10:
    case R_FATAL_ALERT_EN:
    case R_RECOV_ALERT:
    case R_FATAL_ALERT:
    case R_MANUAL_PAD_ATTR_REGWEN_0 ... R_MANUAL_PAD_ATTR_REGWEN_3:
    case R_MANUAL_PAD_ATTR_0 ... R_MANUAL_PAD_ATTR_3:
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
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_sensor_io_read_out((uint32_t)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
}

static void ot_sensor_eg_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtSensorEgState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_sensor_io_write((uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_sensor_eg_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_sensor_eg_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_sensor_eg_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        s->regs[reg] = val32;
        ot_sensor_eg_update_alerts(s);
        s->regs[reg] = 0u;
        ot_sensor_eg_update_alerts(s);
        break;
    case R_CFG_REGWEN:
        val32 &= R_CFG_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_ALERT_TRIG:
        val32 &= ALERT_SENSOR_MASK;
        s->regs[reg] = val32;
        ot_sensor_eg_update_alerts(s);
        break;
    case R_ALERT_EN_0 ... R_ALERT_EN_10:
        if (!s->regs[R_CFG_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Cannot change %s, CFG_REGWEN disabled",
                          REG_NAME(reg));
            break;
        }
        val32 &= ALERT_EN_VAL_MASK;
        s->regs[reg] = val32;
        ot_sensor_eg_update_alerts(s);
        break;
    case R_FATAL_ALERT_EN:
        if (!s->regs[R_CFG_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Cannot change %s, CFG_REGWEN disabled",
                          REG_NAME(reg));
            break;
        }
        val32 &= ALERT_SENSOR_MASK;
        s->regs[reg] = val32;
        ot_sensor_eg_update_alerts(s);
        break;
    case R_RECOV_ALERT:
        val32 &= ALERT_SENSOR_MASK;
        s->regs[reg] &= ~val32;
        ot_sensor_eg_update_alerts(s);
        break;
    case R_FATAL_ALERT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        break;
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        break;
    case R_MANUAL_PAD_ATTR_REGWEN_0 ... R_MANUAL_PAD_ATTR_REGWEN_3:
        val32 &= MANUAL_PAD_ATTR_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_MANUAL_PAD_ATTR_0 ... R_MANUAL_PAD_ATTR_3:
        if (!s->regs[reg - R_MANUAL_PAD_ATTR_0 + R_MANUAL_PAD_ATTR_REGWEN_0]) {
            qemu_log_mask(LOG_GUEST_ERROR, "Cannot change %s, %s disabled\n",
                          REG_NAME(reg),
                          REG_NAME(reg - R_MANUAL_PAD_ATTR_0 +
                                   R_MANUAL_PAD_ATTR_REGWEN_0));
            break;
        }
        val32 &= MANUAL_PAD_ATTR_MASK;
        s->regs[reg] = val32;
        qemu_log_mask(LOG_UNIMP, "%s: %s is not supported\n", __func__,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        break;
    }
};

static const MemoryRegionOps ot_sensor_eg_regs_ops = {
    .read = &ot_sensor_eg_regs_read,
    .write = &ot_sensor_eg_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
    .valid.accepts = &ot_sensor_eg_regs_accepts,
};

static void ot_sensor_eg_reset_enter(Object *obj, ResetType type)
{
    OtSensorEgClass *c = OT_SENSOR_EG_GET_CLASS(obj);
    OtSensorEgState *s = OT_SENSOR_EG(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    s->ast_alert_latch = 0u;

    uint32_t alert_trig = s->regs[R_ALERT_TRIG];
    uint32_t recov_alert = s->regs[R_RECOV_ALERT];
    memset(s->regs, 0, REGS_SIZE);
    s->regs[R_ALERT_TRIG] = alert_trig;
    s->regs[R_RECOV_ALERT] = recov_alert;

    s->regs[R_CFG_REGWEN] = 0x1u;
    s->regs[R_STATUS] = R_STATUS_AST_INIT_DONE_MASK | R_STATUS_IO_POK_MASK;
    for (unsigned rix = R_ALERT_EN_0; rix <= R_ALERT_EN_10; rix++) {
        s->regs[rix] = 0x6u;
    }
    for (unsigned rix = R_MANUAL_PAD_ATTR_REGWEN_0;
         rix <= R_MANUAL_PAD_ATTR_REGWEN_3; rix++) {
        s->regs[rix] = 0x1u;
    }

    ot_sensor_eg_update_irqs(s);
    ot_sensor_eg_update_alerts(s);
}

static void ot_sensor_eg_init(Object *obj)
{
    OtSensorEgState *s = OT_SENSOR_EG(obj);

    memory_region_init_io(&s->mmio, obj, &ot_sensor_eg_regs_ops, s,
                          TYPE_OT_SENSOR_EG, 0x80u);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }
    ibex_qdev_init_irq(obj, &s->wkup, OT_SENSOR_WKUP);
}

static void ot_sensor_eg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtSensorEgClass *sc = OT_SENSOR_EG_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_sensor_eg_reset_enter, NULL,
                                       NULL, &sc->parent_phases);
}

static const TypeInfo ot_sensor_eg_info = {
    .name = TYPE_OT_SENSOR_EG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSensorEgState),
    .instance_init = &ot_sensor_eg_init,
    .class_size = sizeof(OtSensorEgClass),
    .class_init = &ot_sensor_eg_class_init,
};

static void ot_sensor_eg_register_types(void)
{
    type_register_static(&ot_sensor_eg_info);
}

type_init(ot_sensor_eg_register_types);
