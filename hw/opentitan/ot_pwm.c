/*
 * QEMU OpenTitan PWM device
 *
 * Copyright lowRISC contributors.
 *
 * Licensed under the Apache License, Version 2.0, see LICENSE for details.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_pwm.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"

#define PARAM_NUM_OUTPUTS 6u

/* clang-format off */
REG32(ALERT_TEST, 0x00u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(REGWEN, 0x04u)
    FIELD(REGWEN, EN, 0u, 1u)
REG32(CFG, 0x08u)
    FIELD(CFG, CLK_DIV, 0u, 27u)
    FIELD(CFG, DC_RESN, 27u, 4u)
    FIELD(CFG, CNTR_EN, 31u, 1u)
REG32(PWM_EN, 0x0cu)
REG32(INVERT, 0x10u)
REG32(PWM_PARAM_0, 0x14u)
REG32(DUTY_CYCLE_0, 0x2cu)
REG32(BLINK_PARAM_0, 0x44u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))
#define R_LAST_REG   (R_BLINK_PARAM_0 + PARAM_NUM_OUTPUTS - 1u)
#define REGS_COUNT   (R_LAST_REG + 1u)
#define REGS_SIZE    (REGS_COUNT * sizeof(uint32_t))

struct OtPwmState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ alert;

    uint32_t regs[REGS_COUNT];
    int64_t cntr_start_ns;
    uint32_t beat_ctr_q;
    uint16_t phase_ctr_q;
    uint16_t dc_htbt_q[PARAM_NUM_OUTPUTS];

    char *ot_id;
};

static uint64_t ot_pwm_calc_phase(const OtPwmState *s, int64_t now_ns,
                                  uint32_t *beat_out)
{
    uint32_t beat = s->beat_ctr_q;
    uint64_t total_phase = s->phase_ctr_q;
    if (FIELD_EX32(s->regs[R_CFG], CFG, CNTR_EN) && now_ns > s->cntr_start_ns) {
        uint64_t ticks = (uint64_t)(now_ns - s->cntr_start_ns) / 4000ULL;
        uint32_t clk_div = FIELD_EX32(s->regs[R_CFG], CFG, CLK_DIV);
        uint32_t dc_resn = FIELD_EX32(s->regs[R_CFG], CFG, DC_RESN);
        uint64_t period = (uint64_t)clk_div + 1ULL;
        uint64_t rem = (beat <= clk_div) ? (clk_div - beat) : 0ULL;
        if (ticks <= rem) {
            beat += (uint32_t)ticks;
        } else {
            uint64_t after = ticks - (rem + 1ULL);
            beat = (uint32_t)(after % period);
            total_phase += (1ULL + after / period) << (15u - dc_resn);
        }
    }
    if (beat_out) {
        *beat_out = beat;
    }
    return total_phase;
}

void ot_pwm_shift_start_ns(OtPwmState *s, int64_t delta_ns)
{
    if (s) {
        s->cntr_start_ns += delta_ns;
    }
}

bool ot_pwm_is_active(OtPwmState *s)
{
    if (!s) {
        return false;
    }
    bool cntr_en = (bool)FIELD_EX32(s->regs[R_CFG], CFG, CNTR_EN);
    bool chan_en = (s->regs[R_PWM_EN] != 0);
    return cntr_en && chan_en;
}

bool ot_pwm_get_channel_level(OtPwmState *s, unsigned ch, int64_t now_ns)
{
    if (!s || ch >= PARAM_NUM_OUTPUTS) {
        return false;
    }

    bool chan_en = (bool)((s->regs[R_PWM_EN] >> ch) & 1u);
    bool invert = (bool)((s->regs[R_INVERT] >> ch) & 1u);

    if (!chan_en) {
        return invert;
    }

    uint32_t dc_resn = FIELD_EX32(s->regs[R_CFG], CFG, DC_RESN);
    uint64_t total_phase = ot_pwm_calc_phase(s, now_ns, NULL);
    uint16_t phase_cntr = (uint16_t)total_phase;
    uint64_t cycle_idx = total_phase >> 16u;

    uint32_t duty_reg = s->regs[R_DUTY_CYCLE_0 + ch];
    uint16_t duty_a = (uint16_t)(duty_reg & 0xffffu);
    uint16_t duty_b = (uint16_t)((duty_reg >> 16u) & 0xffffu);

    uint32_t param_reg = s->regs[R_PWM_PARAM_0 + ch];
    uint16_t phase_delay = (uint16_t)(param_reg & 0xffffu);
    bool htbt_en = (bool)((param_reg >> 30u) & 1u);
    bool blink_en = (bool)((param_reg >> 31u) & 1u);

    uint32_t blink_reg = s->regs[R_BLINK_PARAM_0 + ch];
    uint16_t blink_x = (uint16_t)(blink_reg & 0xffffu);
    uint16_t blink_y = (uint16_t)((blink_reg >> 16u) & 0xffffu);

    uint16_t duty_cycle_actual =
        (blink_en && htbt_en) ? s->dc_htbt_q[ch] : duty_a;
    if (blink_en && !htbt_en) {
        uint16_t blink_sum = (uint16_t)(blink_x + blink_y + 1u);
        uint16_t blink_ctr =
            (uint16_t)(cycle_idx % ((uint64_t)blink_sum + 1ULL));
        duty_cycle_actual = (blink_ctr > blink_x) ? duty_b : duty_a;
    } else if (blink_en && htbt_en && duty_a != duty_b) {
        uint64_t htbt_steps = cycle_idx / ((uint64_t)blink_x + 1ULL);
        bool pos_htbt = (duty_a < duty_b);
        bool htbt_dir = !pos_htbt;
        uint16_t dc_htbt = s->dc_htbt_q[ch];
        bool dc_wrap_q = false;
        uint64_t max_steps = MIN(htbt_steps, 4096ULL);
        for (uint64_t i = 0; i < max_steps; i++) {
            uint32_t ext =
                htbt_dir ? ((uint32_t)dc_htbt - (uint32_t)blink_y - 1u) :
                           ((uint32_t)dc_htbt + (uint32_t)blink_y + 1u);
            bool dc_wrap = (ext >> 16u) & 1u;
            uint16_t dc_htbt_d = (uint16_t)ext;
            dc_wrap_q =
                pos_htbt ? (dc_wrap && !htbt_dir) : (dc_wrap && htbt_dir);
            dc_htbt = dc_htbt_d;
            if (pos_htbt && (dc_htbt >= duty_b || dc_wrap)) {
                htbt_dir = true;
            } else if (pos_htbt && dc_htbt == duty_a) {
                htbt_dir = false;
            } else if (!pos_htbt && (dc_htbt <= duty_b || dc_wrap)) {
                htbt_dir = false;
            } else if (!pos_htbt && dc_htbt == duty_a) {
                htbt_dir = true;
            }
        }
        duty_cycle_actual = !dc_wrap_q ? dc_htbt : (pos_htbt ? 0xffffu : 0u);
    }

    uint16_t dc_mask = (uint16_t)(0xffffu >> (dc_resn + 1u));
    uint16_t phase_delay_masked = phase_delay & ~dc_mask;
    uint16_t duty_cycle_masked = duty_cycle_actual & ~dc_mask;

    uint32_t off_full =
        (uint32_t)phase_delay_masked + (uint32_t)duty_cycle_masked;
    bool phase_wrap = (off_full > 0xffffu);
    uint16_t off_phase = (uint16_t)off_full;

    bool on_phase_exceeded = (phase_cntr >= phase_delay_masked);
    bool off_phase_exceeded = (phase_cntr >= off_phase);

    bool active = phase_wrap ? (on_phase_exceeded || !off_phase_exceeded) :
                               (on_phase_exceeded && !off_phase_exceeded);

    return invert ? !active : active;
}

static bool ot_pwm_accepts(void *opaque, hwaddr addr, unsigned size,
                           bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    uint8_t reg_be = (uint8_t)(((1u << size) - 1u) << (addr & 0x3u));
    uint8_t permit = (reg == R_CFG || reg >= R_PWM_PARAM_0) ? 0xfu : 0x1u;
    return reg < REGS_COUNT && (!is_write || (permit & ~reg_be) == 0u);
}

static uint64_t ot_pwm_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPwmState *s = opaque;
    (void)size;
    hwaddr reg = R32_OFF(addr);
    return (reg == R_ALERT_TEST) ? 0u : s->regs[reg];
}

static void ot_pwm_write(void *opaque, hwaddr addr, uint64_t val64,
                         unsigned size)
{
    OtPwmState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    hwaddr reg = R32_OFF(addr);

    if (reg == R_ALERT_TEST) {
        if (val32 & R_ALERT_TEST_FATAL_FAULT_MASK) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        return;
    }

    if (reg == R_REGWEN) {
        s->regs[R_REGWEN] &= (val32 & R_REGWEN_EN_MASK);
        return;
    }

    if (!s->regs[R_REGWEN]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Write to locked register 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        return;
    }

    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks = (now_ns > s->cntr_start_ns) ?
                         (uint64_t)(now_ns - s->cntr_start_ns) / 4000ULL :
                         0ULL;
    if (FIELD_EX32(s->regs[R_CFG], CFG, CNTR_EN) && ticks > 0ULL) {
        s->phase_ctr_q = (uint16_t)ot_pwm_calc_phase(s, now_ns, &s->beat_ctr_q);
        s->cntr_start_ns += (int64_t)(ticks * 4000ULL);
    }

    switch (reg) {
    case R_CFG:
        /*
         * In hw/ip/pwm/rtl/pwm_core.sv:85-103,
         * phase_ctr_en = beat_end & (clr_phase_cntr | cntr_en), where
         * beat_end = (beat_ctr_q == clk_div). Because clr_phase_cntr is gated
         * by beat_end, phase_ctr_q is preserved across CFG updates unless
         * beat_ctr_q equals the newly written CLK_DIV.
         */
        if (s->beat_ctr_q == FIELD_EX32(val32, CFG, CLK_DIV)) {
            s->phase_ctr_q = 0u;
        }
        s->beat_ctr_q = 0u;
        s->regs[R_CFG] = val32;
        s->cntr_start_ns = now_ns;
        break;
    case R_PWM_EN: {
        uint32_t prev_en = s->regs[R_PWM_EN];
        s->regs[R_PWM_EN] = val32 & ((1u << PARAM_NUM_OUTPUTS) - 1u);
        if (!prev_en && s->regs[R_PWM_EN] &&
            FIELD_EX32(s->regs[R_CFG], CFG, CNTR_EN)) {
            s->beat_ctr_q = 0u;
            s->phase_ctr_q = 0u;
            s->cntr_start_ns = now_ns;
        }
        break;
    }
    case R_INVERT:
        s->regs[R_INVERT] = val32 & ((1u << PARAM_NUM_OUTPUTS) - 1u);
        break;
    case R_PWM_PARAM_0 ...(R_PWM_PARAM_0 + PARAM_NUM_OUTPUTS - 1u):
        s->regs[reg] = val32 & 0xc000ffffu;
        break;
    default:
        s->regs[reg] = val32;
        break;
    }

    /*
     * In hw/ip/pwm/rtl/pwm_chan.sv:140-154, dc_htbt_q only loads
     * duty_cycle_a_i while htbt_en_i == 0 (ignoring blink_en_i == 0), so
     * writing DUTY_CYCLE while htbt_en == 1 does not re-seed dc_htbt_q.
     */
    for (unsigned ch = 0; ch < PARAM_NUM_OUTPUTS; ch++) {
        if (!(s->regs[R_PWM_PARAM_0 + ch] & (1u << 30u))) {
            s->dc_htbt_q[ch] =
                (uint16_t)(s->regs[R_DUTY_CYCLE_0 + ch] & 0xffffu);
        }
    }

}

static const MemoryRegionOps ot_pwm_regs_ops = {
    .read = &ot_pwm_read,
    .write = &ot_pwm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
    .valid.min_access_size = 1u,
    .valid.max_access_size = 4u,
    .valid.accepts = &ot_pwm_accepts,
};

static const Property ot_pwm_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtPwmState, ot_id),
};

static void ot_pwm_reset_enter(Object *obj, ResetType type)
{
    OtPwmState *s = OT_PWM(obj);

    if (type != RESET_TYPE_COLD && ot_rstmgr_is_low_power_exit()) {
        ibex_irq_set(&s->alert, 0);
        return;
    }

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_REGWEN] = 1u;
    s->regs[R_CFG] = (7u << R_CFG_DC_RESN_SHIFT) | 0x8000u;
    for (unsigned i = 0; i < PARAM_NUM_OUTPUTS; i++) {
        s->regs[R_DUTY_CYCLE_0 + i] = 0x7fff7fffull;
        s->dc_htbt_q[i] = 0x7fffu;
    }
    s->cntr_start_ns = 0;
    s->beat_ctr_q = 0u;
    s->phase_ctr_q = 0u;
    ibex_irq_set(&s->alert, 0);
}

static void ot_pwm_init(Object *obj)
{
    OtPwmState *s = OT_PWM(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pwm_regs_ops, s, TYPE_OT_PWM,
                          0x80u);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
}

static void ot_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    RESETTABLE_CLASS(klass)->phases.enter = &ot_pwm_reset_enter;
    device_class_set_props(dc, ot_pwm_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_pwm_info = {
    .name = TYPE_OT_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPwmState),
    .instance_init = &ot_pwm_init,
    .class_init = &ot_pwm_class_init,
};

static void ot_pwm_register_types(void)
{
    type_register_static(&ot_pwm_info);
}

type_init(ot_pwm_register_types);
