/*
 * QEMU OpenTitan Alert handler device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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
 * Note: for now, only a minimalist subset of Alert Handler device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#define PARAM_ESC_CNT_DW  32u
#define PARAM_ACCU_CNT_DW 16u
#define PARAM_N_ESC_SEV   4u
#define PARAM_PING_CNT_DW 16u
#define PARAM_PHASE_DW    2u
#define PARAM_CLASS_DW    2u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_STATE_CLASSA, 0u, 1u)
    SHARED_FIELD(INTR_STATE_CLASSB, 1u, 1u)
    SHARED_FIELD(INTR_STATE_CLASSC, 2u, 1u)
    SHARED_FIELD(INTR_STATE_CLASSD, 3u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(PING_TIMER_REGWEN, 0xcu)
    FIELD(PING_TIMER_REGWEN, EN, 0u, 1u)
REG32(PING_TIMEOUT_CYC_SHADOWED, 0x10u)
    FIELD(PING_TIMEOUT_CYC_SHADOWED, VAL, 0u, 16u)
REG32(PING_TIMER_EN_SHADOWED, 0x14u)
    FIELD(PING_TIMER_EN_SHADOWED, EN, 0u, 1u)
SHARED_FIELD(ALERT_REGWEN_EN, 0u, 1u)
SHARED_FIELD(ALERT_EN_SHADOWED_EN, 0u, 1u)
SHARED_FIELD(ALERT_CLASS_SHADOWED_EN, 0u, 2u)
SHARED_FIELD(ALERT_CAUSE_EN, 0u, 1u)
SHARED_FIELD(LOC_ALERT_REGWEN_EN, 0u, 1u)
SHARED_FIELD(LOC_ALERT_EN_SHADOWED_EN, 0u, 1u)
SHARED_FIELD(LOC_ALERT_CLASS_SHADOWED_EN, 0u, 2u)
SHARED_FIELD(LOC_ALERT_CAUSE_EN, 0u, 1u)
SHARED_FIELD(CLASS_REGWEN_EN, 0u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_EN, 0u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_LOCK, 1u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_EN_E0, 2u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_EN_E1, 3u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_EN_E2, 4u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_EN_E3, 5u, 1u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_MAP_E0, 6u, 2u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_MAP_E1, 8u, 2u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_MAP_E2, 10u, 2u)
SHARED_FIELD(CLASS_CTRL_SHADOWED_MAP_E3, 12u, 2u)
SHARED_FIELD(CLASS_CLR_REGWEN_EN, 0u, 1u)
SHARED_FIELD(CLASS_CLR_SHADOWED_EN, 0u, 1u)
SHARED_FIELD(CLASS_ACCUM_CNT, 0u, 16u)
SHARED_FIELD(CLASS_ACCUM_THRESH_SHADOWED, 0u, 16u)
SHARED_FIELD(CLASS_CRASHDUMP_TRIGGER_SHADOWED, 0u, 2u)
SHARED_FIELD(CLASS_STATE, 0u, 3u)
/* clang-format on */

#define INTR_MASK ((1u << PARAM_N_CLASSES) - 1u)
#define CLASS_CTRL_SHADOWED_MASK \
    (CLASS_CTRL_SHADOWED_EN_MASK | CLASS_CTRL_SHADOWED_LOCK_MASK | \
     CLASS_CTRL_SHADOWED_EN_E0_MASK | CLASS_CTRL_SHADOWED_EN_E1_MASK | \
     CLASS_CTRL_SHADOWED_EN_E2_MASK | CLASS_CTRL_SHADOWED_EN_E3_MASK | \
     CLASS_CTRL_SHADOWED_MAP_E0_MASK | CLASS_CTRL_SHADOWED_MAP_E1_MASK | \
     CLASS_CTRL_SHADOWED_MAP_E2_MASK | CLASS_CTRL_SHADOWED_MAP_E3_MASK)

#define R32_OFF(_r_)   ((_r_) / sizeof(uint32_t))
#define REG_COUNT(_s_) (sizeof(_s_) / sizeof(OtShadowReg))

/*
 * as many registers are shadowed, it is easier to use shadow registers
 * for all registers, and only use the shadow 'committed' attribute for
 * the rest of them (the non-shadow registers)
 */

/* direct value of a 'fake' shadow register */
#define DVAL(_shadow_) ((_shadow_).committed)

#define ACLASS(_cls_) ((char)('A' + (_cls_)))

typedef struct {
    OtShadowReg state;
    OtShadowReg enable;
    OtShadowReg test;
} OtAlertIntr;

typedef struct {
    OtShadowReg timer_regwen;
    OtShadowReg timeout_cyc_shadowed;
    OtShadowReg timer_en_shadowed;
} OtAlertPing;

/* not a real structure, only used to compute register spacing */
typedef struct {
    OtShadowReg regwen;
    OtShadowReg en_shadowed;
    OtShadowReg class_shadowed;
    OtShadowReg cause;
} OtAlertTemplate;

typedef struct {
    OtShadowReg *regwen;
    OtShadowReg *en_shadowed;
    OtShadowReg *class_shadowed;
    OtShadowReg *cause;
} OtAlertArrays;

typedef struct {
    OtShadowReg regwen;
    OtShadowReg ctrl_shadowed;
    OtShadowReg clr_regwen;
    OtShadowReg clr_shadowed;
    OtShadowReg accum_cnt;
    OtShadowReg accum_thresh_shadowed;
    OtShadowReg timeout_cyc_shadowed;
    OtShadowReg crashdump_trigger_shadowed;
    OtShadowReg phase_cyc_shadowed[4u];
    OtShadowReg esc_cnt;
    OtShadowReg state;
} OtAlertAClass;

typedef struct OtAlertRegs {
    OtShadowReg *shadow;
    /* shortcuts to the shadow entries */
    OtAlertIntr *intr;
    OtAlertPing *ping;
    OtAlertArrays alerts;
    OtAlertArrays loc_alerts;
    OtAlertAClass *classes;
} OtAlertRegs;

typedef uint32_t (*ot_alert_reg_read_fn)(OtAlertState *s, unsigned reg);
typedef void (*ot_alert_reg_write_fn)(OtAlertState *s, unsigned reg,
                                      uint32_t value);

typedef enum {
    PWA_UPDATE_IRQ,
    PWA_CLEAR_ALERT,
} OtAlertPostWriteAction;

typedef struct {
    ot_alert_reg_read_fn read;
    ot_alert_reg_write_fn write;
    uint32_t mask; /* the mask to apply to the written value */
    uint16_t protect; /* not 0 if write protected by another register */
    uint8_t wpost; /* whether to perform post-write action */
} OtAlertAccess;

typedef struct {
    /* count cycles: either timeout cycles or phase escalation cycles */
    QEMUTimer timer;
    QEMUBH *esc_releaser;
    OtAlertState *parent;
    IbexIRQ *esc_tx_release; /* Escalate signal to release */
    unsigned nclass;
} OtAlertScheduler;

enum {
    LOCAL_ALERT_ALERT_PINGFAIL,
    LOCAL_ALERT_ESC_PINGFAIL,
    LOCAL_ALERT_ALERT_INTEGFAIL,
    LOCAL_ALERT_ESC_INTEGFAIL,
    LOCAL_ALERT_BUS_INTEGFAIL,
    LOCAL_ALERT_SHADOW_REG_UPDATE_ERROR,
    LOCAL_ALERT_SHADOW_REG_STORAGE_ERROR,
    LOCAL_ALERT_COUNT,
};

typedef enum {
    STATE_IDLE,
    STATE_TIMEOUT,
    STATE_FSMERROR,
    STATE_TERMINAL,
    STATE_PHASE0,
    STATE_PHASE1,
    STATE_PHASE2,
    STATE_PHASE3,
    STATE_COUNT,
} OtAlertAClassState;

struct OtAlertState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ *irqs;
    IbexIRQ *esc_txs;
    OtAlertScheduler *schedulers;

    OtAlertRegs regs; /* not ordered by register index */
    OtAlertAccess *access_table; /* ordered by register index */
    char **reg_names; /* ordered by register index */
    unsigned reg_count; /* total count of registers */
    unsigned reg_aclass_pos; /* index of the first register of OtAlertAClass */

    char *ot_id;
    OtEDNState *edn;
    uint32_t pclk;
    uint16_t n_alerts;
    uint8_t edn_ep;
    uint8_t n_low_power_groups;
    uint8_t n_classes;
};

/* clang-format off */
#define ST_NAME_ENTRY(_name_) [STATE_##_name_] = stringify(_name_)
#define ST_NAME(_st_) ((_st_) < ARRAY_SIZE(ST_NAMES) ? ST_NAMES[(_st_)] : "?")

static const char *ST_NAMES[] = {
    ST_NAME_ENTRY(IDLE),
    ST_NAME_ENTRY(TIMEOUT),
    ST_NAME_ENTRY(FSMERROR),
    ST_NAME_ENTRY(TERMINAL),
    ST_NAME_ENTRY(PHASE0),
    ST_NAME_ENTRY(PHASE1),
    ST_NAME_ENTRY(PHASE2),
    ST_NAME_ENTRY(PHASE3),
};
#undef ST_NAME_ENTRY

#define R_ACC_MPA(_r_, _w_, _m_, _p_, _u_) (OtAlertAccess) { \
    .read = &ot_alert_reg_ ## _r_, \
    .write = &ot_alert_reg_ ## _w_, \
    .mask = (_m_), \
    .protect = (_p_), \
    .wpost = (_u_) \
}
/* clang-format on */
#define R_ACC_MP(_r_, _w_, _m_, _p_) R_ACC_MPA(_r_, _w_, _m_, _p_, 0)
#define R_ACC_M_IRQ(_r_, _w_, _m_) \
    R_ACC_MPA(_r_, _w_, _m_, 0, BIT(PWA_UPDATE_IRQ))
#define R_ACC_P(_r_, _w_, _p_) R_ACC_MP(_r_, _w_, UINT32_MAX, _p_)
#define R_ACC_M(_r_, _w_, _m_) R_ACC_MP(_r_, _w_, _m_, 0)
#define R_ACC_IRQ(_r_, _w_)    R_ACC_M_IRQ(_r_, _w_, UINT32_MAX)
#define R_ACC(_r_, _w_)        R_ACC_M(_r_, _w_, UINT32_MAX)

#define REG_NAME_LENGTH 36u /* > "CLASS_X_CRASHDUMP_TRIGGER_SHADOWED" */

#define REG_NAME(_s_, _reg_) \
    ((_reg_) < (_s_)->reg_count ? (_s_)->reg_names[(_reg_)] : "?")
#define CREATE_NAME_REGISTER(_s_, _reg_) \
    strncpy((_s_)->reg_names[R_##_reg_], stringify(_reg_), REG_NAME_LENGTH - 1)
#define CREATE_NAME_REG_IX_AT(_s_, _off_, _ix_, _reg_) \
    do { \
        int l = snprintf((_s_)->reg_names[(_off_)], REG_NAME_LENGTH, \
                         stringify(_reg_) "_%02u", (_ix_)); \
        g_assert((unsigned)l < REG_NAME_LENGTH); \
    } while (0)
#define CREATE_NAME_REG_CLS_AT(_s_, _off_, _ix_, _reg_) \
    do { \
        int l = snprintf((_s_)->reg_names[(_off_)], REG_NAME_LENGTH, \
                         "CLASS_%c_" stringify(_reg_), 'A' + (_ix_)); \
        g_assert((unsigned)l < REG_NAME_LENGTH); \
    } while (0)
#undef ALERT_SHOW_OT_ID_REG_NAME /* define as ot_id string here */

static unsigned
ot_alert_get_nclass_from_reg(const OtAlertState *s, unsigned reg)
{
    g_assert(reg >= s->reg_aclass_pos && reg < s->reg_count);
    unsigned nclass = (reg - s->reg_aclass_pos) / REG_COUNT(OtAlertAClass);
    g_assert(nclass < s->n_classes);
    return nclass;
}

static OtAlertAClassState
ot_alert_get_class_state(const OtAlertState *s, unsigned nclass)
{
    const OtAlertAClass *aclass = &s->regs.classes[nclass];

    return DVAL(aclass->state);
}

static void ot_alert_set_class_state(OtAlertState *s, unsigned nclass,
                                     OtAlertAClassState state)
{
    trace_ot_alert_set_class_state(s->ot_id, ACLASS(nclass),
                                   ST_NAME(DVAL(s->regs.classes[nclass].state)),
                                   ST_NAME(state));

    g_assert(state >= 0 && state < STATE_COUNT);

    DVAL(s->regs.classes[nclass].state) = state;
}

static void ot_alert_update_irqs(OtAlertState *s)
{
    uint32_t level = DVAL(s->regs.intr->state) & DVAL(s->regs.intr->enable);

    trace_ot_alert_irqs(s->ot_id, DVAL(s->regs.intr->state),
                        DVAL(s->regs.intr->enable), level);
    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1));
    }
}

static uint32_t ot_alert_reg_write_only(OtAlertState *s, unsigned reg)
{
    (void)s;
    qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%03x\n", __func__,
                  (unsigned)(reg * sizeof(uint32_t)));
    return 0;
}

static void ot_alert_reg_read_only(OtAlertState *s, unsigned reg,
                                   uint32_t value)
{
    (void)s;
    (void)value;
    qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%03x\n", __func__,
                  (unsigned)(reg * sizeof(uint32_t)));
}

static uint32_t ot_alert_reg_direct_read(OtAlertState *s, unsigned reg)
{
    return DVAL(s->regs.shadow[reg]);
}

static uint32_t ot_alert_reg_shadow_read(OtAlertState *s, unsigned reg)
{
    return ot_shadow_reg_read(&s->regs.shadow[reg]);
}

static uint32_t ot_alert_reg_esc_count_read(OtAlertState *s, unsigned reg)
{
    unsigned nclass = ot_alert_get_nclass_from_reg(s, reg);
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    unsigned state = DVAL(aclass->state);

    QEMUTimer *timer = &s->schedulers[nclass].timer;

    uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    uint64_t expire = timer_expire_time_ns(timer);
    if (expire == UINT64_MAX) {
        trace_ot_alert_esc_count(s->ot_id, ACLASS(nclass), ST_NAME(state), 0);
        return 0;
    }

    uint32_t cycles;

    switch (state) {
    case STATE_TIMEOUT:
        cycles = ot_shadow_reg_peek(&aclass->timeout_cyc_shadowed);
        break;
    case STATE_PHASE0:
    case STATE_PHASE1:
    case STATE_PHASE2:
    case STATE_PHASE3:
        cycles = ot_shadow_reg_peek(
            &aclass->phase_cyc_shadowed[state - STATE_PHASE0]);
        break;
    default:
        trace_ot_alert_esc_count(s->ot_id, ACLASS(nclass), ST_NAME(state), 0);
        return 0;
    }

    uint32_t cnt;
    if (expire >= now) {
        uint64_t rem64 =
            muldiv64(expire - now, s->pclk, NANOSECONDS_PER_SECOND);
        uint32_t rem32 = (uint32_t)MIN(rem64, (uint64_t)UINT32_MAX);
        cnt = (rem32 < cycles) ? cycles - rem32 : 0;
    } else {
        cnt = cycles;
    }

    trace_ot_alert_esc_count(s->ot_id, ACLASS(nclass), ST_NAME(state), cnt);

    return cnt;
}

static void ot_alert_reg_direct_write(OtAlertState *s, unsigned reg,
                                      uint32_t value)
{
    if ((!s->access_table[reg].protect) ||
        DVAL(s->regs.shadow[s->access_table[reg].protect]) & 0x1) {
        value &= s->access_table[reg].mask;
        DVAL(s->regs.shadow[reg]) = value;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Register 0x%03x write protected by 0x%03x\n",
                      __func__, (unsigned)(reg * sizeof(uint32_t)),
                      (unsigned)(s->access_table[reg].protect *
                                 sizeof(uint32_t)));
    }
}

static void ot_alert_reg_shadow_write(OtAlertState *s, unsigned reg,
                                      uint32_t value)
{
    value &= s->access_table[reg].mask;
    ot_shadow_reg_write(&s->regs.shadow[reg], value);
}

static void
ot_alert_reg_direct_rw0c_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.shadow[reg]) &= value;
}

static void
ot_alert_reg_direct_rw1c_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.shadow[reg]) &= ~value;
}

static void
ot_alert_reg_intr_state_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.shadow[reg]) &= ~value;

    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        /*
         * "Software should clear the corresponding interrupt state bit
         *  INTR_STATE.CLASSn before the timeout expires to avoid escalation."
         */
        if (value & (1u << ix)) {
            OtAlertAClassState state = ot_alert_get_class_state(s, ix);
            if (state == STATE_TIMEOUT) {
                if (timer_pending(&s->schedulers[ix].timer)) {
                    trace_ot_alert_cancel_timeout(s->ot_id, ACLASS(ix));
                    timer_del(&s->schedulers[ix].timer);
                }
                ot_alert_set_class_state(s, ix, STATE_IDLE);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: %s: clearing IRQ for class %c in state %s "
                              "did not stop escalation",
                              __func__, s->ot_id, ACLASS(ix), ST_NAME(state));
            }
        }
    }
}

static void ot_alert_set_class_timer(OtAlertState *s, unsigned nclass,
                                     uint32_t timeout)
{
    OtAlertScheduler *atimer = &s->schedulers[nclass];
    /* TODO: update running schedulers if timeout_cyc_shadowed is updated */
    int64_t ns = (int64_t)muldiv64(timeout, NANOSECONDS_PER_SECOND, s->pclk);

    OtAlertAClassState state = ot_alert_get_class_state(s, nclass);
    trace_ot_alert_set_class_timer(s->ot_id, ACLASS(nclass), ST_NAME(state),
                                   ns / 1000, timeout);

    ns += qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    if (ns < timer_expire_time_ns(&atimer->timer)) {
        timer_mod_anticipate_ns(&atimer->timer, ns);
    }
}

static bool
ot_alert_is_escalation_enabled(OtAlertState *s, unsigned nclass, unsigned esc)
{
    const OtAlertAClass *aclass = &s->regs.classes[nclass];
    uint32_t ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);

    bool enable;
    switch (esc) {
    case 0:
        enable = (bool)SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E0);
        break;
    case 1:
        enable = (bool)SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E1);
        break;
    case 2:
        enable = (bool)SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E2);
        break;
    case 3:
        enable = (bool)SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E3);
        break;
    default:
        g_assert_not_reached();
        break;
    }

    return enable;
}

static IbexIRQ *
ot_alert_get_escalation_output(OtAlertState *s, unsigned nclass, unsigned esc)
{
    const OtAlertAClass *aclass = &s->regs.classes[nclass];
    uint32_t ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);

    uint32_t out;
    switch (esc) {
    case 0:
        out = SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E0);
        break;
    case 1:
        out = SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E1);
        break;
    case 2:
        out = SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E2);
        break;
    case 3:
        out = SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E3);
        break;
    default:
        g_assert_not_reached();
        break;
    }

    return &s->esc_txs[out];
}

static void ot_alert_clear_alert(OtAlertState *s, unsigned nclass)
{
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    OtAlertAClassState state = DVAL(aclass->state);

    if (state == STATE_FSMERROR) {
        trace_ot_alert_error(s->ot_id, ACLASS(nclass),
                             "cannot exit FSMERROR state");
        return;
    }

    uint32_t ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);

    if (ctrl & CLASS_CTRL_SHADOWED_LOCK_MASK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: class %c cannot clear escalation: locked\n",
                      __func__, s->ot_id, ACLASS(nclass));
        return;
    }

    OtAlertScheduler *atimer = &s->schedulers[nclass];
    timer_del(&atimer->timer);
    for (unsigned ix = 0; ix < PARAM_N_ESC_SEV; ix++) {
        IbexIRQ *esc_tx = ot_alert_get_escalation_output(s, nclass, ix);
        if (ibex_irq_get_level(esc_tx)) {
            trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), ix, "release");
        }
        ibex_irq_set(esc_tx, 0);
    }
    /*
     * "Software can clear CLASSn_ACCUM_CNT with a write to CLASSA_CLR_SHADOWED"
     */
    DVAL(aclass->accum_cnt) = 0;
    ot_alert_set_class_state(s, nclass, STATE_IDLE);
}

static void ot_alert_configure_phase_cycles(OtAlertState *s, unsigned nclass)
{
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    OtAlertAClassState state = DVAL(aclass->state);

    unsigned phase;

    switch (state) {
    case STATE_PHASE0:
    case STATE_PHASE1:
    case STATE_PHASE2:
    case STATE_PHASE3:
        phase = state - STATE_PHASE0;
        break;
    default:
        g_assert_not_reached();
        return;
    }

    uint32_t cycles =
        ot_shadow_reg_peek(&s->regs.classes[nclass].phase_cyc_shadowed[phase]);
    ot_alert_set_class_timer(s, nclass, cycles);
}

static void ot_alert_fsm_update(OtAlertState *s, unsigned nclass,
                                bool from_timer)
{
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    OtAlertAClassState state = DVAL(aclass->state);

    bool accu_trig = DVAL(aclass->accum_cnt) >
                     ot_shadow_reg_peek(&aclass->accum_thresh_shadowed);

    trace_ot_alert_fsm_update(s->ot_id, ACLASS(nclass), ST_NAME(state),
                              from_timer, accu_trig);

    IbexIRQ *esc_tx;
    switch (state) {
    case STATE_IDLE:
        if (accu_trig) {
            unsigned esc = 0;
            ot_alert_set_class_state(s, nclass, STATE_PHASE0);
            if (ot_alert_is_escalation_enabled(s, nclass, esc)) {
                esc_tx = ot_alert_get_escalation_output(s, nclass, esc);
                ibex_irq_set(esc_tx, true);
                trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), esc,
                                          "activate");
            } else {
                trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), esc,
                                          "disabled");
            }
            ot_alert_configure_phase_cycles(s, nclass);
        } else {
            uint32_t timeout = DVAL(aclass->timeout_cyc_shadowed);
            if (timeout) {
                ot_alert_set_class_state(s, nclass, STATE_TIMEOUT);
                ot_alert_set_class_timer(s, nclass, timeout);
            }
        }
        break;
    case STATE_TIMEOUT:
        if (from_timer || accu_trig) {
            /* cancel timer, even if only useful on accu_trigg */
            OtAlertScheduler *atimer = &s->schedulers[nclass];
            timer_del(&atimer->timer);
            ot_alert_set_class_state(s, nclass, STATE_PHASE0);
            unsigned esc = 0;
            if (ot_alert_is_escalation_enabled(s, nclass, esc)) {
                esc_tx = ot_alert_get_escalation_output(s, nclass, esc);
                ibex_irq_set(esc_tx, true);
                trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), esc,
                                          "activate");
            } else {
                trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), esc,
                                          "disabled");
            }
            ot_alert_configure_phase_cycles(s, nclass);
        }
        break;
    case STATE_PHASE0:
    case STATE_PHASE1:
    case STATE_PHASE2:
    case STATE_PHASE3:
        /* cycle count has reached threshold */
        if (from_timer) {
            if (state <= STATE_PHASE2) {
                /*
                 * Store escalation output to release before updating state.
                 * HW raise next escalation output before releasing current one.
                 * Use a BH to release with on "next" cycle.
                 */
                unsigned esc = state - STATE_PHASE0;
                esc_tx = ot_alert_get_escalation_output(s, nclass, esc);
                s->schedulers[nclass].esc_tx_release = esc_tx;
                state += 1;
                esc = state - STATE_PHASE0;
                ot_alert_set_class_state(s, nclass, state);
                if (ot_alert_is_escalation_enabled(s, nclass, esc)) {
                    esc_tx = ot_alert_get_escalation_output(s, nclass, esc);
                    ibex_irq_set(esc_tx, true);
                    trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), esc,
                                              "activate");
                } else {
                    trace_ot_alert_escalation(s->ot_id, ACLASS(nclass), esc,
                                              "disabled");
                }
                ot_alert_configure_phase_cycles(s, nclass);
                qemu_bh_schedule(s->schedulers[nclass].esc_releaser);
            } else /* PHASE3 */ {
                ot_alert_set_class_state(s, nclass, STATE_TERMINAL);
            }
        }
        break;
    case STATE_TERMINAL:
        break;
    case STATE_FSMERROR:
    default:
        g_assert_not_reached();
        break;
    }
}

static void ot_alert_timer_expire(void *opaque)
{
    OtAlertScheduler *scheduler = opaque;
    OtAlertState *s = scheduler->parent;

    trace_ot_alert_timer_expire(s->ot_id, ACLASS(scheduler->nclass));

    ot_alert_fsm_update(s, scheduler->nclass, true);
}

static void ot_alert_release_esc_fn(void *opaque)
{
    OtAlertScheduler *scheduler = opaque;

    IbexIRQ *esc_tx = scheduler->esc_tx_release;
    g_assert(esc_tx);

    if (ibex_irq_get_level(esc_tx)) {
        OtAlertState *s = scheduler->parent;
        unsigned ix = (unsigned)(uintptr_t)(esc_tx - &s->esc_txs[0]);
        trace_ot_alert_escalation(s->ot_id, ACLASS(scheduler->nclass), ix,
                                  "release");
    }
    ibex_irq_set(esc_tx, false);
    scheduler->esc_tx_release = NULL;
}

static void ot_alert_signal_tx(void *opaque, int n, int level)
{
    OtAlertState *s = opaque;

    unsigned alert = (unsigned)n;

    g_assert(alert < s->n_alerts);

    OtAlertArrays *alerts = &s->regs.alerts;
    bool alert_en = ot_shadow_reg_peek(&alerts->en_shadowed[alert]);

    trace_ot_alert_signal_tx(s->ot_id, alert, (bool)level, alert_en);

    if (!alert_en || !level) {
        /* releasing the alert does not clear it */
        return;
    }

    DVAL(alerts->cause[alert]) |= ALERT_CAUSE_EN_MASK;

    unsigned nclass = ot_shadow_reg_peek(&alerts->class_shadowed[alert]);

    OtAlertAClass *aclass = &s->regs.classes[nclass];

    uint32_t ac_ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);
    bool class_en = (bool)(ac_ctrl & CLASS_CTRL_SHADOWED_EN_MASK);

    trace_ot_alert_signal_class(s->ot_id, alert, ACLASS(nclass), class_en);

    DVAL(s->regs.intr->state) |= 1u << nclass;

    if (class_en) {
        /* saturate (no roll over) */
        if (DVAL(aclass->accum_cnt) < CLASS_ACCUM_CNT_MASK) {
            DVAL(aclass->accum_cnt) += 1u;
        }

        ot_alert_fsm_update(s, nclass, false);
    }

    ot_alert_update_irqs(s);
}

static uint64_t ot_alert_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtAlertState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    if (reg >= s->reg_count) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s:Invalid register 0x%03" HWADDR_PRIx "\n", __func__,
                      addr);
        return 0;
    }

    val32 = (*s->access_table[reg].read)(s, reg);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_alert_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(s, reg),
                               val32, pc);

    return (uint64_t)val32;
};

static void ot_alert_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtAlertState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_alert_io_write(s->ot_id, (uint32_t)addr, REG_NAME(s, reg), val32,
                            pc);

    if (reg >= s->reg_count) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s:Invalid register 0x%03" HWADDR_PRIx "\n", __func__,
                      addr);
        return;
    }

    (*s->access_table[reg].write)(s, reg, val32);

    if (s->access_table[reg].wpost & BIT(PWA_UPDATE_IRQ)) {
        ot_alert_update_irqs(s);
    }

    if (s->access_table[reg].wpost & BIT(PWA_CLEAR_ALERT)) {
        unsigned nclass = ot_alert_get_nclass_from_reg(s, reg);
        ot_alert_clear_alert(s, nclass);
    }
};

static void ot_alert_fill_access_table(OtAlertState *s)
{
    OtAlertAccess *table = s->access_table;

    table[R_INTR_STATE] = R_ACC_IRQ(direct_read, intr_state_write);
    table[R_INTR_ENABLE] = R_ACC_IRQ(direct_read, direct_write);
    table[R_INTR_TEST] = R_ACC(write_only, direct_write);
    table[R_PING_TIMER_REGWEN] = R_ACC(direct_read, direct_write);
    table[R_PING_TIMEOUT_CYC_SHADOWED] = R_ACC(shadow_read, shadow_write);
    table[R_PING_TIMER_EN_SHADOWED] = R_ACC(shadow_read, shadow_write);
    OtShadowReg *first_var_reg = s->regs.alerts.regwen;

    unsigned offset = (unsigned)(first_var_reg - &s->regs.shadow[0]);

    /* ALERT_REGWEN */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_M(direct_read, direct_rw0c_write, ALERT_REGWEN_EN_MASK);
    }
    offset += s->n_alerts;

    /* ALERT_EN_SHADOWED */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_M(shadow_read, shadow_write, ALERT_EN_SHADOWED_EN_MASK);
    }
    offset += s->n_alerts;

    /* ALERT_CLASS_SHADOWED */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_M(shadow_read, shadow_write, s->n_classes - 1u);
    }
    offset += s->n_alerts;

    /* ALERT_CAUSE */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_M(direct_read, direct_rw1c_write, ALERT_CAUSE_EN_MASK);
    }
    offset += s->n_alerts;

    /* LOC_ALERT_REGWEN */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_M(direct_read, direct_rw0c_write, LOC_ALERT_REGWEN_EN_MASK);
    }
    offset += LOCAL_ALERT_COUNT;

    /* LOC_ALERT_EN_SHADOWED */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_M(shadow_read, shadow_write, LOC_ALERT_EN_SHADOWED_EN_MASK);
    }
    offset += LOCAL_ALERT_COUNT;

    /* LOC_ALERT_CLASS_SHADOWED */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_M(shadow_read, shadow_write, s->n_classes - 1u);
    }
    offset += LOCAL_ALERT_COUNT;

    /* LOC_ALERT_CAUSE */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_M(direct_read, direct_rw1c_write, LOC_ALERT_CAUSE_EN_MASK);
    }
    offset += LOCAL_ALERT_COUNT;

    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        /* CLASS_REGWEN */
        unsigned regwen = offset;
        table[offset++] =
            R_ACC_M(direct_read, direct_rw0c_write, CLASS_REGWEN_EN_MASK);

        /* CLASS_CTRL_SHADOWED */
        table[offset++] = R_ACC_MP(shadow_read, shadow_write,
                                   CLASS_CTRL_SHADOWED_MASK, regwen);

        /* CLASS_CLR_REGWEN */
        unsigned clr_regwen = offset;
        table[offset++] =
            R_ACC_M(direct_read, direct_rw0c_write, CLASS_CLR_REGWEN_EN_MASK);

        /* CLASS_CLR_SHADOWED */
        table[offset++] =
            R_ACC_MPA(shadow_read, shadow_write, CLASS_CLR_SHADOWED_EN_MASK,
                      clr_regwen, BIT(PWA_CLEAR_ALERT));

        /* CLASS_ACCUM_CNT */
        table[offset++] = R_ACC(direct_read, read_only);

        /* CLASS_ACCUM_THRESH_SHADOWED */
        table[offset++] =
            R_ACC_MP(shadow_read, shadow_write, UINT16_MAX, regwen);

        /* CLASS_TIMEOUT_CYC_SHADOWED */
        table[offset++] = R_ACC_P(shadow_read, shadow_write, regwen);

        /* CLASS_CRASHDUMP_TRIGGER_SHADOWED */
        table[offset++] =
            R_ACC_MP(shadow_read, shadow_write,
                     CLASS_CRASHDUMP_TRIGGER_SHADOWED_MASK, regwen);

        /* CLASS_PHASE0_CYC_SHADOWED */
        table[offset++] = R_ACC_P(shadow_read, shadow_write, regwen);

        /* CLASS_PHASE1_CYC_SHADOWED */
        table[offset++] = R_ACC_P(shadow_read, shadow_write, regwen);

        /* CLASS_PHASE2_CYC_SHADOWED */
        table[offset++] = R_ACC_P(shadow_read, shadow_write, regwen);

        /* CLASS_PHASE3_CYC_SHADOWED */
        table[offset++] = R_ACC_P(shadow_read, shadow_write, regwen);

        /* CLASS_ESC_CNT */
        table[offset++] = R_ACC(esc_count_read, read_only);

        /* CLASS_STATE */
        table[offset++] = R_ACC(direct_read, read_only);
    }

    g_assert(offset == s->reg_count);
}

static Property ot_alert_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtAlertState, ot_id),
    DEFINE_PROP_UINT16("n_alerts", OtAlertState, n_alerts, 0),
    DEFINE_PROP_UINT8("n_lpg", OtAlertState, n_low_power_groups, 1u),
    DEFINE_PROP_UINT8("n_classes", OtAlertState, n_classes, 4u),
    DEFINE_PROP_UINT32("pclk", OtAlertState, pclk, 0u),
    DEFINE_PROP_LINK("edn", OtAlertState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtAlertState, edn_ep, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_alert_regs_ops = {
    .read = &ot_alert_regs_read,
    .write = &ot_alert_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_alert_reset(DeviceState *dev)
{
    OtAlertState *s = OT_ALERT(dev);

    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        timer_del(&s->schedulers[ix].timer);
    }

    memset(s->regs.shadow, 0, sizeof(OtShadowReg) * s->reg_count);

    DVAL(s->regs.ping->timer_regwen) = R_PING_TIMER_REGWEN_EN_MASK;
    ot_shadow_reg_init(&s->regs.ping->timeout_cyc_shadowed, 256u);
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        /* direct register */
        DVAL(s->regs.alerts.regwen[ix]) = ALERT_REGWEN_EN_MASK;
    }
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        /* direct register */
        DVAL(s->regs.loc_alerts.regwen[ix]) = LOC_ALERT_REGWEN_EN_MASK;
    }
    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        DVAL(s->regs.classes[ix].regwen) = CLASS_REGWEN_EN_MASK;
        ot_shadow_reg_init(&s->regs.classes[ix].ctrl_shadowed, 0x393cu);
        DVAL(s->regs.classes[ix].clr_regwen) = CLASS_CLR_REGWEN_EN_MASK;
    }

    ot_alert_update_irqs(s);
}

static void ot_alert_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    OtAlertState *s = OT_ALERT(dev);

    g_assert(s->n_alerts != 0);
    g_assert(s->pclk != 0);
    g_assert(s->n_classes > 0 && s->n_classes <= 32);

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }

    size_t size = sizeof(OtAlertIntr) + sizeof(OtAlertPing) +
                  sizeof(OtAlertTemplate) * s->n_alerts +
                  sizeof(OtAlertTemplate) * LOCAL_ALERT_COUNT +
                  sizeof(OtAlertAClass) * s->n_classes;

    memory_region_init_io(&s->mmio, OBJECT(dev), &ot_alert_regs_ops, s,
                          TYPE_OT_ALERT, size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->irqs = g_new0(IbexIRQ, s->n_classes);
    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        ibex_sysbus_init_irq(OBJECT(dev), &s->irqs[ix]);
    }

    s->esc_txs = g_new0(IbexIRQ, PARAM_N_ESC_SEV);
    ibex_qdev_init_irqs(OBJECT(dev), s->esc_txs, OT_ALERT_ESCALATE,
                        PARAM_N_ESC_SEV);

    qdev_init_gpio_in_named(dev, &ot_alert_signal_tx, OT_DEVICE_ALERT,
                            s->n_alerts);

    s->schedulers = g_new0(OtAlertScheduler, s->n_classes);
    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        s->schedulers[ix].parent = s;
        s->schedulers[ix].nclass = ix;
        timer_init_full(&s->schedulers[ix].timer, NULL, OT_VIRTUAL_CLOCK,
                        SCALE_NS, 0, &ot_alert_timer_expire,
                        &s->schedulers[ix]);
        s->schedulers[ix].esc_releaser =
            qemu_bh_new(&ot_alert_release_esc_fn, &s->schedulers[ix]);
        s->schedulers[ix].esc_tx_release = NULL;
    }

    s->reg_count = size / sizeof(OtShadowReg);
    s->regs.shadow = g_new0(OtShadowReg, s->reg_count);
    s->access_table = g_new0(OtAlertAccess, s->reg_count);
    s->reg_names = g_new0(char *, s->reg_count);

    OtShadowReg *reg = s->regs.shadow;
    s->regs.intr = (OtAlertIntr *)reg;
    reg += REG_COUNT(OtAlertIntr);
    s->regs.ping = (OtAlertPing *)reg;
    reg += REG_COUNT(OtAlertPing);
    s->regs.alerts.regwen = reg;
    reg += s->n_alerts;
    s->regs.alerts.en_shadowed = reg;
    reg += s->n_alerts;
    s->regs.alerts.class_shadowed = reg;
    reg += s->n_alerts;
    s->regs.alerts.cause = reg;
    reg += s->n_alerts;
    s->regs.loc_alerts.regwen = reg;
    reg += LOCAL_ALERT_COUNT;
    s->regs.loc_alerts.en_shadowed = reg;
    reg += LOCAL_ALERT_COUNT;
    s->regs.loc_alerts.class_shadowed = reg;
    reg += LOCAL_ALERT_COUNT;
    s->regs.loc_alerts.cause = reg;
    reg += LOCAL_ALERT_COUNT;
    s->regs.classes = (OtAlertAClass *)reg;
    s->reg_aclass_pos = (unsigned)(uintptr_t)(reg - s->regs.shadow);
    reg += REG_COUNT(OtAlertAClass) * s->n_classes;

    g_assert(reg - s->regs.shadow == s->reg_count);

    char *name_buf = g_new0(char, (size_t)(REG_NAME_LENGTH * s->reg_count));
    for (unsigned ix = 0; ix < s->reg_count;
         ix++, name_buf += REG_NAME_LENGTH) {
        s->reg_names[ix] = name_buf;
    }
    unsigned nreg = 0;
    g_assert(s->reg_count > REG_COUNT(OtAlertIntr) + REG_COUNT(OtAlertPing));
    CREATE_NAME_REGISTER(s, INTR_STATE);
    CREATE_NAME_REGISTER(s, INTR_ENABLE);
    CREATE_NAME_REGISTER(s, INTR_TEST);
    nreg += REG_COUNT(OtAlertIntr);
    CREATE_NAME_REGISTER(s, PING_TIMER_REGWEN);
    CREATE_NAME_REGISTER(s, PING_TIMEOUT_CYC_SHADOWED);
    CREATE_NAME_REGISTER(s, PING_TIMER_EN_SHADOWED);
    nreg += REG_COUNT(OtAlertPing);
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        CREATE_NAME_REG_IX_AT(s, nreg + s->n_alerts * 0u + ix, ix,
                              ALERT_REGWEN);
        CREATE_NAME_REG_IX_AT(s, nreg + s->n_alerts * 1u + ix, ix,
                              ALERT_EN_SHADOWED);
        CREATE_NAME_REG_IX_AT(s, nreg + s->n_alerts * 2u + ix, ix,
                              ALERT_CLASS_SHADOWED);
        CREATE_NAME_REG_IX_AT(s, nreg + s->n_alerts * 3u + ix, ix, ALERT_CAUSE);
    }
    nreg += REG_COUNT(OtAlertTemplate) * s->n_alerts;
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        CREATE_NAME_REG_IX_AT(s, nreg + LOCAL_ALERT_COUNT * 0u + ix, ix,
                              LOC_ALERT_REGWEN);
        CREATE_NAME_REG_IX_AT(s, nreg + LOCAL_ALERT_COUNT * 1u + ix, ix,
                              LOC_ALERT_EN_SHADOWED);
        CREATE_NAME_REG_IX_AT(s, nreg + LOCAL_ALERT_COUNT * 2u + ix, ix,
                              LOC_ALERT_CLASS_SHADOWED);
        CREATE_NAME_REG_IX_AT(s, nreg + LOCAL_ALERT_COUNT * 3u + ix, ix,
                              LOC_ALERT_CAUSE);
    }
    nreg += REG_COUNT(OtAlertTemplate) * LOCAL_ALERT_COUNT;
    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        CREATE_NAME_REG_CLS_AT(s, nreg + 0u, ix, REGWEN);
        CREATE_NAME_REG_CLS_AT(s, nreg + 1u, ix, CTRL_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 2u, ix, CLR_REGWEN);
        CREATE_NAME_REG_CLS_AT(s, nreg + 3u, ix, CLR_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 4u, ix, ACCUM_CNT);
        CREATE_NAME_REG_CLS_AT(s, nreg + 5u, ix, ACCUM_THRESH_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 6u, ix, TIMEOUT_CYC_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 7u, ix, CRASHDUMP_TRIGGER_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 8u, ix, PHASE0_CYC_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 9u, ix, PHASE1_CYC_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 10u, ix, PHASE2_CYC_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 11u, ix, PHASE3_CYC_SHADOWED);
        CREATE_NAME_REG_CLS_AT(s, nreg + 12u, ix, ESC_CNT);
        CREATE_NAME_REG_CLS_AT(s, nreg + 13u, ix, STATE);
        nreg += 14u;
    }
    g_assert(nreg == s->reg_count);

#ifdef ALERT_SHOW_OT_ID_REG_NAME
    if (!strcmp(s->ot_id, ALERT_SHOW_OT_ID_REG_NAME)) {
        fprintf(stderr, "nreg %u regcount %u\n", nreg, s->reg_count);
        for (unsigned ix = 0; ix < nreg; ix++) {
            fprintf(stderr, "reg[%03x]: %s\n", ix, s->reg_names[ix]);
        }
    }
#endif

    ot_alert_fill_access_table(s);
}

static void ot_alert_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_alert_reset;
    dc->realize = &ot_alert_realize;
    device_class_set_props(dc, ot_alert_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_alert_info = {
    .name = TYPE_OT_ALERT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtAlertState),
    .class_init = &ot_alert_class_init,
};

static void ot_alert_register_types(void)
{
    type_register_static(&ot_alert_info);
}

type_init(ot_alert_register_types);
