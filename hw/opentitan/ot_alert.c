/*
 * QEMU OpenTitan Alert handler device
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
 * Note: for now, only a minimalist subset of Alert Handler device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 *       secondary clock source is not supported (secure.io_div4)
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
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
    uint8_t permit; /* ALERT_HANDLER_PERMIT byte-enable mask (0x1, 0x3, 0xf) */
} OtAlertAccess;

typedef struct {
    /* count cycles: either timeout cycles or phase escalation cycles */
    QEMUTimer timer;
    QEMUBH *esc_releaser;
    OtAlertState *parent;
    uint32_t
        esc_tx_release_mask; /* Escalate signals to release on next cycle */
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

typedef enum {
    OT_ALERT_CLOCK_SRC_IO,
    OT_ALERT_CLOCK_SRC_EDN,
    OT_ALERT_CLOCK_SRC_COUNT
} OtAonTimerClockSrc;

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
    uint32_t pclks[OT_ALERT_CLOCK_SRC_COUNT];
    const char *clock_src_names[OT_ALERT_CLOCK_SRC_COUNT];

    char *ot_id;
    OtEDNState *edn;
    char *clock_names[OT_ALERT_CLOCK_SRC_COUNT];
    DeviceState *clock_src;
    uint16_t n_alerts;
    uint8_t edn_ep;
    uint8_t n_low_power_groups;
    uint8_t n_classes;
    uint32_t crashdump_latched;
    uint32_t latched_dump[9];
    QEMUTimer reseed_timer;
    bool edn_connected;
    bool entropy_requested;
    bool *alert_levels;
};

struct OtAlertClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
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
    .wpost = (_u_), \
    .permit = ((uint32_t)(_m_) > 0xffffu ? 0xfu : \
               ((uint32_t)(_m_) > 0xffu ? 0x3u : 0x1u)) \
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

static bool ot_alert_is_class_enabled(const OtAlertState *s, unsigned nclass)
{
    const OtAlertAClass *aclass = &s->regs.classes[nclass];
    uint32_t ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);
    uint32_t esc_en_mask =
        CLASS_CTRL_SHADOWED_EN_E0_MASK | CLASS_CTRL_SHADOWED_EN_E1_MASK |
        CLASS_CTRL_SHADOWED_EN_E2_MASK | CLASS_CTRL_SHADOWED_EN_E3_MASK;
    return (ctrl & CLASS_CTRL_SHADOWED_EN_MASK) && (ctrl & esc_en_mask);
}

static void ot_alert_build_crashdump(OtAlertState *s, uint32_t dump[9]);
static void ot_alert_fsm_update(OtAlertState *s, unsigned nclass,
                                bool from_timer, bool class_trig);
static void ot_alert_enter_phase0(OtAlertState *s, unsigned nclass);

static void ot_alert_set_class_state(OtAlertState *s, unsigned nclass,
                                     OtAlertAClassState state)
{
    trace_ot_alert_set_class_state(s->ot_id, ACLASS(nclass),
                                   ST_NAME(DVAL(s->regs.classes[nclass].state)),
                                   ST_NAME(state));

    g_assert(state >= 0 && state < STATE_COUNT);

    DVAL(s->regs.classes[nclass].state) = state;

    if (state >= STATE_PHASE0 && state <= STATE_PHASE3) {
        unsigned phase = state - STATE_PHASE0;
        uint32_t trig_phase =
            ot_shadow_reg_peek(
                &s->regs.classes[nclass].crashdump_trigger_shadowed) &
            CLASS_CRASHDUMP_TRIGGER_SHADOWED_MASK;
        if (phase == trig_phase) {
            if (s->crashdump_latched == 0) {
                ot_alert_build_crashdump(s, s->latched_dump);
            }
            s->crashdump_latched |= (1u << nclass);
        }
    }
}

static void ot_alert_update_irqs(OtAlertState *s)
{
    uint32_t level = DVAL(s->regs.intr->state) & DVAL(s->regs.intr->enable);

    trace_ot_alert_irqs(s->ot_id, DVAL(s->regs.intr->state),
                        DVAL(s->regs.intr->enable), level);
    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        bool irq_active = ((level >> ix) & 0x1u) != 0;
        ibex_irq_set(&s->irqs[ix], (int)irq_active);

        OtAlertAClassState state = ot_alert_get_class_state(s, ix);
        uint32_t timeout =
            ot_shadow_reg_peek(&s->regs.classes[ix].timeout_cyc_shadowed);
        if (state == STATE_IDLE && irq_active && timeout != 0 &&
            ot_alert_is_class_enabled(s, ix)) {
            ot_alert_fsm_update(s, ix, false, false);
        } else if (state == STATE_TIMEOUT) {
            if (!irq_active) {
                if (timer_pending(&s->schedulers[ix].timer)) {
                    trace_ot_alert_cancel_timeout(s->ot_id, ACLASS(ix));
                    timer_del(&s->schedulers[ix].timer);
                }
                ot_alert_set_class_state(s, ix, STATE_IDLE);
            } else if (timeout == 0) {
                if (timer_pending(&s->schedulers[ix].timer)) {
                    timer_del(&s->schedulers[ix].timer);
                }
                ot_alert_enter_phase0(s, ix);
            }
        }
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
    if ((expire >= now) && (s->pclks[OT_ALERT_CLOCK_SRC_IO] != 0)) {
        uint64_t rem64 = muldiv64(expire - now, s->pclks[OT_ALERT_CLOCK_SRC_IO],
                                  NANOSECONDS_PER_SECOND);
        uint32_t rem32 = (uint32_t)MIN(rem64, (uint64_t)UINT32_MAX);
        cnt = (rem32 < cycles) ? cycles - rem32 : 0;
    } else {
        cnt = cycles;
    }

    trace_ot_alert_esc_count(s->ot_id, ACLASS(nclass), ST_NAME(state), cnt);

    return cnt;
}

static void ot_alert_set_dump_bits(uint32_t dump[9], unsigned start_bit,
                                   unsigned width, uint64_t val)
{
    for (unsigned i = 0; i < width; i++) {
        unsigned bit_pos = start_bit + i;
        unsigned word_idx = bit_pos / 32u;
        unsigned bit_idx = bit_pos % 32u;
        if (val & (1ULL << i)) {
            dump[word_idx] |= (1u << bit_idx);
        }
    }
}

static void ot_alert_build_crashdump(OtAlertState *s, uint32_t dump[9])
{
    memset(dump, 0, sizeof(uint32_t) * 9u);

    /* class_esc_state[0..3]: 4 x 3 bits at bit 0..11 */
    for (unsigned c = 0; c < s->n_classes && c < 4u; c++) {
        uint32_t state = DVAL(s->regs.classes[c].state) & 0x7u;
        ot_alert_set_dump_bits(dump, c * 3u, 3u, state);
    }

    /* class_esc_cnt[0..3]: 4 x 32 bits at bit 12..139 */
    for (unsigned c = 0; c < s->n_classes && c < 4u; c++) {
        unsigned reg = s->reg_aclass_pos + c * REG_COUNT(OtAlertAClass) +
                       offsetof(OtAlertAClass, esc_cnt) / sizeof(OtShadowReg);
        uint32_t esc_cnt = ot_alert_reg_esc_count_read(s, reg);
        ot_alert_set_dump_bits(dump, 12u + c * 32u, 32u, esc_cnt);
    }

    /* class_accum_cnt[0..3]: 4 x 16 bits at bit 140..203 */
    for (unsigned c = 0; c < s->n_classes && c < 4u; c++) {
        uint32_t accum_cnt = DVAL(s->regs.classes[c].accum_cnt) & 0xffffu;
        ot_alert_set_dump_bits(dump, 140u + c * 16u, 16u, accum_cnt);
    }

    /* loc_alert_cause[6:0]: 7 bits at bit 204..210 */
    uint32_t loc_cause = 0;
    for (unsigned i = 0; i < LOCAL_ALERT_COUNT; i++) {
        if (DVAL(s->regs.loc_alerts.cause[i]) & LOC_ALERT_CAUSE_EN_MASK) {
            loc_cause |= (1u << i);
        }
    }
    ot_alert_set_dump_bits(dump, 204u, 7u, loc_cause);

    /* alert_cause[0..64]: 65 bits at bit 211..275 */
    for (unsigned i = 0; i < s->n_alerts && i < 65u; i++) {
        uint32_t cause =
            (DVAL(s->regs.alerts.cause[i]) & ALERT_CAUSE_EN_MASK) ? 1u : 0u;
        ot_alert_set_dump_bits(dump, 211u + i, 1u, cause);
    }
}

void ot_alert_get_crash_dump(OtAlertState *s, uint32_t dump[9])
{
    if (s->crashdump_latched != 0) {
        memcpy(dump, s->latched_dump, sizeof(uint32_t) * 9u);
    } else {
        ot_alert_build_crashdump(s, dump);
    }
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

static void ot_alert_raise_local(OtAlertState *s, unsigned loc_alert);

static void ot_alert_reg_shadow_write(OtAlertState *s, unsigned reg,
                                      uint32_t value)
{
    if (s->access_table[reg].protect &&
        !(DVAL(s->regs.shadow[s->access_table[reg].protect]) & 0x1u)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Register 0x%03x write protected by 0x%03x\n",
                      __func__, (unsigned)(reg * sizeof(uint32_t)),
                      (unsigned)(s->access_table[reg].protect *
                                 sizeof(uint32_t)));
        return;
    }
    value &= s->access_table[reg].mask;
    if (ot_shadow_reg_write(&s->regs.shadow[reg], value) ==
        OT_SHADOW_REG_ERROR) {
        ot_alert_raise_local(s, LOCAL_ALERT_SHADOW_REG_UPDATE_ERROR);
    }
}

static void
ot_alert_reg_direct_rw0c_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.shadow[reg]) &= value;
}

static void ot_alert_signal_tx(void *opaque, int n, int level);

static void
ot_alert_reg_direct_rw1c_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.shadow[reg]) &= ~value;
}

static void
ot_alert_reg_cause_rw1c_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.shadow[reg]) &= ~value;

    unsigned cause_base =
        (unsigned)(uintptr_t)(s->regs.alerts.cause - s->regs.shadow);
    if (reg >= cause_base && reg < cause_base + s->n_alerts) {
        unsigned alert = reg - cause_base;
        if (s->alert_levels[alert]) {
            ot_alert_signal_tx(s, (int)alert, 1);
        }
    }
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

static void
ot_alert_reg_intr_test_write(OtAlertState *s, unsigned reg, uint32_t value)
{
    value &= s->access_table[reg].mask;
    DVAL(s->regs.intr->state) |= value;
}

static void ot_alert_set_class_timer(OtAlertState *s, unsigned nclass,
                                     uint32_t timeout)
{
    if (s->pclks[OT_ALERT_CLOCK_SRC_IO] == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: no clock\n", __func__,
                      s->ot_id);
        return;
    }
    OtAlertScheduler *atimer = &s->schedulers[nclass];
    /* TODO: update running schedulers if timeout_cyc_shadowed is updated */
    uint32_t cycles = timeout ? timeout : 1u;
    int64_t ns = (int64_t)muldiv64(cycles, NANOSECONDS_PER_SECOND,
                                   s->pclks[OT_ALERT_CLOCK_SRC_IO]);

    OtAlertAClassState state = ot_alert_get_class_state(s, nclass);
    trace_ot_alert_set_class_timer(s->ot_id, ACLASS(nclass), ST_NAME(state),
                                   ns / 1000, timeout);

    ns += qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    timer_mod_ns(&atimer->timer, ns);
}

static uint32_t ot_alert_get_phase_signals_mask(const OtAlertState *s,
                                                unsigned nclass, unsigned phase)
{
    const OtAlertAClass *aclass = &s->regs.classes[nclass];
    uint32_t ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);
    uint32_t mask = 0;

    if (SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E0) &&
        SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E0) == phase) {
        mask |= (1u << 0);
    }
    if (SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E1) &&
        SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E1) == phase) {
        mask |= (1u << 1);
    }
    if (SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E2) &&
        SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E2) == phase) {
        mask |= (1u << 2);
    }
    if (SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_EN_E3) &&
        SHARED_FIELD_EX32(ctrl, CLASS_CTRL_SHADOWED_MAP_E3) == phase) {
        mask |= (1u << 3);
    }
    return mask;
}

static void ot_alert_update_escalation_outputs(OtAlertState *s)
{
    uint32_t active_mask = 0;
    for (unsigned c = 0; c < s->n_classes; c++) {
        OtAlertAClassState st = ot_alert_get_class_state(s, c);
        if (st >= STATE_PHASE0 && st <= STATE_PHASE3) {
            active_mask |=
                ot_alert_get_phase_signals_mask(s, c, st - STATE_PHASE0);
        }
        active_mask |= s->schedulers[c].esc_tx_release_mask;
    }

    for (unsigned sig = 0; sig < PARAM_N_ESC_SEV; sig++) {
        bool next_lvl = (active_mask & (1u << sig)) != 0;
        bool prev_lvl = (bool)ibex_irq_get_level(&s->esc_txs[sig]);
        if (next_lvl != prev_lvl) {
            ibex_irq_set(&s->esc_txs[sig], next_lvl);
        }
    }
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

    if (!(DVAL(aclass->clr_regwen) & CLASS_CLR_REGWEN_EN_MASK)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: class %c cannot clear escalation: locked\n",
                      __func__, s->ot_id, ACLASS(nclass));
        return;
    }

    /*
     * "Software can clear CLASSn_ACCUM_CNT with a write to CLASSA_CLR_SHADOWED"
     */
    DVAL(aclass->accum_cnt) = 0;
    s->crashdump_latched &= ~(1u << nclass);

    /*
     * In RTL (alert_handler_esc_timer.sv:168-184, 311-317 CheckClr_A), clr_i
     * only transitions Phase0..Phase3 and Terminal back to IdleSt; it does NOT
     * exit TimeoutSt while timeout_en_i (irq[nclass]) is active.
     */
    if (state == STATE_IDLE || state == STATE_TIMEOUT) {
        return;
    }

    OtAlertScheduler *atimer = &s->schedulers[nclass];
    timer_del(&atimer->timer);
    atimer->esc_tx_release_mask = 0;

    ot_alert_set_class_state(s, nclass, STATE_IDLE);
    ot_alert_update_escalation_outputs(s);
    ot_alert_update_irqs(s);
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

static void ot_alert_enter_phase0(OtAlertState *s, unsigned nclass)
{
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    uint32_t ctrl = ot_shadow_reg_peek(&aclass->ctrl_shadowed);
    if ((ctrl & CLASS_CTRL_SHADOWED_LOCK_MASK) &&
        ot_alert_is_class_enabled(s, nclass)) {
        DVAL(aclass->clr_regwen) = 0;
    }
    ot_alert_set_class_state(s, nclass, STATE_PHASE0);
    ot_alert_update_escalation_outputs(s);
    ot_alert_configure_phase_cycles(s, nclass);
}

static void ot_alert_fsm_update(OtAlertState *s, unsigned nclass,
                                bool from_timer, bool class_trig)
{
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    OtAlertAClassState state = DVAL(aclass->state);

    bool accu_trig =
        class_trig && (DVAL(aclass->accum_cnt) >
                       ot_shadow_reg_peek(&aclass->accum_thresh_shadowed));

    trace_ot_alert_fsm_update(s->ot_id, ACLASS(nclass), ST_NAME(state),
                              from_timer, accu_trig);

    switch (state) {
    case STATE_IDLE:
        if (accu_trig && ot_alert_is_class_enabled(s, nclass)) {
            ot_alert_enter_phase0(s, nclass);
        } else {
            bool timeout_en =
                ((DVAL(s->regs.intr->state) & DVAL(s->regs.intr->enable)) >>
                 nclass) &
                0x1u;
            uint32_t timeout =
                ot_shadow_reg_peek(&aclass->timeout_cyc_shadowed);
            if (timeout_en && timeout && ot_alert_is_class_enabled(s, nclass)) {
                ot_alert_set_class_state(s, nclass, STATE_TIMEOUT);
                ot_alert_set_class_timer(s, nclass, timeout);
            }
        }
        break;
    case STATE_TIMEOUT:
        if (from_timer || (accu_trig && ot_alert_is_class_enabled(s, nclass))) {
            /* cancel timer, even if only useful on accu_trigg */
            OtAlertScheduler *atimer = &s->schedulers[nclass];
            timer_del(&atimer->timer);
            ot_alert_enter_phase0(s, nclass);
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
                unsigned prev_phase = state - STATE_PHASE0;
                s->schedulers[nclass].esc_tx_release_mask =
                    ot_alert_get_phase_signals_mask(s, nclass, prev_phase);
                state += 1;
                ot_alert_set_class_state(s, nclass, state);
                ot_alert_update_escalation_outputs(s);
                ot_alert_configure_phase_cycles(s, nclass);
                if (s->schedulers[nclass].esc_tx_release_mask) {
                    qemu_bh_schedule(s->schedulers[nclass].esc_releaser);
                }
            } else /* PHASE3 */ {
                s->schedulers[nclass].esc_tx_release_mask = 0;
                ot_alert_set_class_state(s, nclass, STATE_TERMINAL);
                ot_alert_update_escalation_outputs(s);
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

    ot_alert_fsm_update(s, scheduler->nclass, true, false);
}

static void ot_alert_release_esc_fn(void *opaque)
{
    OtAlertScheduler *scheduler = opaque;
    if (!scheduler->esc_tx_release_mask) {
        return;
    }
    scheduler->esc_tx_release_mask = 0;
    ot_alert_update_escalation_outputs(scheduler->parent);
}

static void ot_alert_signal_tx(void *opaque, int n, int level)
{
    OtAlertState *s = opaque;

    unsigned alert = (unsigned)n;

    g_assert(alert < s->n_alerts);

    s->alert_levels[alert] = (bool)level;

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

    bool class_en = ot_alert_is_class_enabled(s, nclass);

    trace_ot_alert_signal_class(s->ot_id, alert, ACLASS(nclass), class_en);

    DVAL(s->regs.intr->state) |= 1u << nclass;

    if (class_en) {
        /* saturate (no roll over) */
        if (DVAL(aclass->accum_cnt) < CLASS_ACCUM_CNT_MASK) {
            DVAL(aclass->accum_cnt) += 1u;
        }

        ot_alert_fsm_update(s, nclass, false, true);
    }

    ot_alert_update_irqs(s);
}

static void ot_alert_clock_input(void *opaque, int irq, int level)
{
    OtAlertState *s = opaque;

    g_assert((unsigned)irq < OT_ALERT_CLOCK_SRC_COUNT);

    s->pclks[irq] = (unsigned)level;

    /* TODO: reinitialize timers on PCLK change */
}

static bool ot_alert_regs_accepts(void *opaque, hwaddr addr, unsigned size,
                                  bool is_write, MemTxAttrs attrs)
{
    OtAlertState *s = opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    if (reg >= s->reg_count) {
        return false;
    }
    if (!is_write) {
        return true;
    }
    uint8_t reg_be = (uint8_t)(((1u << size) - 1u) << (addr & 3u));
    return (s->access_table[reg].permit & ~reg_be) == 0u;
}

static uint64_t ot_alert_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtAlertState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    if (reg < s->reg_count) {
        val32 = (*s->access_table[reg].read)(s, reg);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Invalid register 0x%03x\n",
                      __func__, (uint32_t)addr);
        val32 = 0u;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_alert_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(s, reg),
                               val32, pc);

    return (uint64_t)val32;
}

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
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Invalid register 0x%03x\n",
                      __func__, (uint32_t)addr);
        return;
    }

    bool was_staged = s->regs.shadow[reg].staged_p;
    (*s->access_table[reg].write)(s, reg, val32);

    if (s->access_table[reg].wpost & BIT(PWA_UPDATE_IRQ)) {
        ot_alert_update_irqs(s);
        ot_alert_update_escalation_outputs(s);
    }

    if (s->access_table[reg].wpost & BIT(PWA_CLEAR_ALERT)) {
        if (was_staged && !s->regs.shadow[reg].staged_p &&
            (ot_shadow_reg_peek(&s->regs.shadow[reg]) &
             CLASS_CLR_SHADOWED_EN_MASK)) {
            unsigned nclass = ot_alert_get_nclass_from_reg(s, reg);
            ot_alert_clear_alert(s, nclass);
        }
    }
}

static void ot_alert_raise_local(OtAlertState *s, unsigned loc_alert)
{
    g_assert(loc_alert < LOCAL_ALERT_COUNT);
    OtAlertArrays *loc_alerts = &s->regs.loc_alerts;
    bool alert_en = ot_shadow_reg_peek(&loc_alerts->en_shadowed[loc_alert]);

    if (!alert_en) {
        return;
    }

    DVAL(loc_alerts->cause[loc_alert]) |= ALERT_CAUSE_EN_MASK;

    unsigned nclass =
        ot_shadow_reg_peek(&loc_alerts->class_shadowed[loc_alert]);
    OtAlertAClass *aclass = &s->regs.classes[nclass];
    bool class_en = ot_alert_is_class_enabled(s, nclass);

    DVAL(s->regs.intr->state) |= 1u << nclass;

    if (class_en) {
        if (DVAL(aclass->accum_cnt) < CLASS_ACCUM_CNT_MASK) {
            DVAL(aclass->accum_cnt) += 1u;
        }
        ot_alert_fsm_update(s, nclass, false, true);
    }

    ot_alert_update_irqs(s);
}

static void ot_alert_check_ping_timer(OtAlertState *s)
{
    bool ping_en = (bool)ot_shadow_reg_peek(&s->regs.ping->timer_en_shadowed);
    uint32_t timeout = ot_shadow_reg_peek(&s->regs.ping->timeout_cyc_shadowed);

    if (ping_en && timeout > 0 && timeout <= 2) {
        ot_alert_raise_local(s, LOCAL_ALERT_ALERT_PINGFAIL);
        ot_alert_raise_local(s, LOCAL_ALERT_ESC_PINGFAIL);
        ot_alert_signal_tx(s, 0, 1);
    }
}

static uint32_t ot_alert_reg_cause_read(OtAlertState *s, unsigned reg)
{
    ot_alert_check_ping_timer(s);
    return DVAL(s->regs.shadow[reg]);
}

static void ot_alert_reg_ping_write(OtAlertState *s, unsigned reg, uint32_t val)
{
    ot_alert_reg_shadow_write(s, reg, val);
    ot_alert_check_ping_timer(s);
}

static void ot_alert_reg_ping_en_write(OtAlertState *s, unsigned reg,
                                       uint32_t val)
{
    uint32_t prev = ot_shadow_reg_peek(&s->regs.shadow[reg]);
    ot_alert_reg_shadow_write(s, reg, val | prev);
    s->regs.shadow[reg].committed |= prev;
    ot_alert_check_ping_timer(s);
}

static void ot_alert_fill_access_table(OtAlertState *s)
{
    OtAlertAccess *table = s->access_table;
    uint32_t intr_mask = (1u << s->n_classes) - 1u;

    table[R_INTR_STATE] = R_ACC_M_IRQ(direct_read, intr_state_write, intr_mask);
    table[R_INTR_ENABLE] = R_ACC_M_IRQ(direct_read, direct_write, intr_mask);
    table[R_INTR_TEST] = R_ACC_M_IRQ(write_only, intr_test_write, intr_mask);
    table[R_PING_TIMER_REGWEN] =
        R_ACC_M(direct_read, direct_rw0c_write, R_PING_TIMER_REGWEN_EN_MASK);
    table[R_PING_TIMEOUT_CYC_SHADOWED] =
        R_ACC_MP(shadow_read, ping_write, R_PING_TIMEOUT_CYC_SHADOWED_VAL_MASK,
                 R_PING_TIMER_REGWEN);
    table[R_PING_TIMER_EN_SHADOWED] =
        R_ACC_MP(shadow_read, ping_en_write, R_PING_TIMER_EN_SHADOWED_EN_MASK,
                 R_PING_TIMER_REGWEN);
    OtShadowReg *first_var_reg = s->regs.alerts.regwen;

    unsigned offset = (unsigned)(first_var_reg - &s->regs.shadow[0]);

    /* ALERT_REGWEN */
    unsigned alert_regwen_base = offset;
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_M(direct_read, direct_rw0c_write, ALERT_REGWEN_EN_MASK);
    }
    offset += s->n_alerts;

    /* ALERT_EN_SHADOWED */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_MP(shadow_read, shadow_write, ALERT_EN_SHADOWED_EN_MASK,
                     alert_regwen_base + ix);
    }
    offset += s->n_alerts;

    /* ALERT_CLASS_SHADOWED */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_MP(shadow_read, shadow_write, s->n_classes - 1u,
                     alert_regwen_base + ix);
    }
    offset += s->n_alerts;

    /* ALERT_CAUSE */
    for (unsigned ix = 0; ix < s->n_alerts; ix++) {
        table[offset + ix] =
            R_ACC_M(cause_read, cause_rw1c_write, ALERT_CAUSE_EN_MASK);
    }
    offset += s->n_alerts;

    /* LOC_ALERT_REGWEN */
    unsigned loc_alert_regwen_base = offset;
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_M(direct_read, direct_rw0c_write, LOC_ALERT_REGWEN_EN_MASK);
    }
    offset += LOCAL_ALERT_COUNT;

    /* LOC_ALERT_EN_SHADOWED */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_MP(shadow_read, shadow_write, LOC_ALERT_EN_SHADOWED_EN_MASK,
                     loc_alert_regwen_base + ix);
    }
    offset += LOCAL_ALERT_COUNT;

    /* LOC_ALERT_CLASS_SHADOWED */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_MP(shadow_read, shadow_write, s->n_classes - 1u,
                     loc_alert_regwen_base + ix);
    }
    offset += LOCAL_ALERT_COUNT;

    /* LOC_ALERT_CAUSE */
    for (unsigned ix = 0; ix < LOCAL_ALERT_COUNT; ix++) {
        table[offset + ix] =
            R_ACC_M(cause_read, direct_rw1c_write, LOC_ALERT_CAUSE_EN_MASK);
    }
    offset += LOCAL_ALERT_COUNT;

    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        /* CLASS_REGWEN */
        unsigned regwen = offset;
        table[offset++] =
            R_ACC_M(direct_read, direct_rw0c_write, CLASS_REGWEN_EN_MASK);

        /* CLASS_CTRL_SHADOWED */
        table[offset++] =
            R_ACC_MPA(shadow_read, shadow_write, CLASS_CTRL_SHADOWED_MASK,
                      regwen, BIT(PWA_UPDATE_IRQ));

        /* CLASS_CLR_REGWEN */
        unsigned clr_regwen = offset;
        table[offset++] =
            R_ACC_M(direct_read, direct_rw0c_write, CLASS_CLR_REGWEN_EN_MASK);

        /* CLASS_CLR_SHADOWED */
        table[offset++] =
            R_ACC_MPA(shadow_read, shadow_write, CLASS_CLR_SHADOWED_EN_MASK,
                      clr_regwen, BIT(PWA_CLEAR_ALERT));

        /* CLASS_ACCUM_CNT */
        table[offset++] = R_ACC_M(direct_read, read_only, CLASS_ACCUM_CNT_MASK);

        /* CLASS_ACCUM_THRESH_SHADOWED */
        table[offset++] =
            R_ACC_MP(shadow_read, shadow_write, UINT16_MAX, regwen);

        /* CLASS_TIMEOUT_CYC_SHADOWED */
        table[offset++] = R_ACC_MPA(shadow_read, shadow_write, UINT32_MAX,
                                    regwen, BIT(PWA_UPDATE_IRQ));

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
        table[offset++] = R_ACC_M(direct_read, read_only, 0x7u);
    }

    g_assert(offset == s->reg_count);
}

static const Property ot_alert_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtAlertState, ot_id),
    DEFINE_PROP_UINT16("n_alerts", OtAlertState, n_alerts, 0),
    DEFINE_PROP_UINT8("n_lpg", OtAlertState, n_low_power_groups, 1u),
    DEFINE_PROP_UINT8("n_classes", OtAlertState, n_classes, 4u),
    DEFINE_PROP_STRING("clock-name", OtAlertState,
                       clock_names[OT_ALERT_CLOCK_SRC_IO]),
    DEFINE_PROP_STRING("clock-name-edn", OtAlertState,
                       clock_names[OT_ALERT_CLOCK_SRC_EDN]),
    DEFINE_PROP_LINK("clock-src", OtAlertState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_LINK("edn", OtAlertState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtAlertState, edn_ep, UINT8_MAX),
};

static const MemoryRegionOps ot_alert_regs_ops = {
    .read = &ot_alert_regs_read,
    .write = &ot_alert_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
    .valid.accepts = &ot_alert_regs_accepts,
};

static void ot_alert_request_entropy(OtAlertState *s);

static void ot_alert_fill_entropy(void *opaque, uint32_t bits, bool fips)
{
    OtAlertState *s = opaque;
    (void)bits;
    (void)fips;
    s->entropy_requested = false;
    /*
     * In RTL (alert_handler_ping_timer.sv), reseed_timer_d is reloaded with
     * {wait_cyc_mask_i, {ReseedLfsrExtraBits{1'b1}}} = 19'h7ffff (524,287
     * cycles) at the 24 MHz I/O Div4 clock (~21.845 ms).
     */
    uint32_t pclk = MAX(s->pclks[OT_ALERT_CLOCK_SRC_IO], 1u);
    uint64_t reseed_ns = muldiv64(0x7ffffULL, NANOSECONDS_PER_SECOND, pclk);
    timer_mod(&s->reseed_timer,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + (int64_t)reseed_ns);
}

static void ot_alert_request_entropy(OtAlertState *s)
{
    if (!s->edn || s->edn_ep == UINT8_MAX) {
        return;
    }
    if (!s->edn_connected) {
        ot_edn_connect_endpoint(s->edn, s->edn_ep, &ot_alert_fill_entropy, s);
        s->edn_connected = true;
    }
    if (!s->entropy_requested) {
        s->entropy_requested = true;
        ot_edn_request_entropy(s->edn, s->edn_ep);
    }
}

static void ot_alert_reseed_timer_cb(void *opaque)
{
    OtAlertState *s = opaque;
    ot_alert_request_entropy(s);
}

static void ot_alert_reset_enter(Object *obj, ResetType type)
{
    OtAlertClass *c = OT_ALERT_GET_CLASS(obj);
    OtAlertState *s = OT_ALERT(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(&s->reseed_timer);
    s->entropy_requested = false;
    timer_mod(&s->reseed_timer, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + 100000LL);

    for (unsigned ix = 0; ix < s->n_classes; ix++) {
        qemu_bh_cancel(s->schedulers[ix].esc_releaser);
        timer_del(&s->schedulers[ix].timer);
        s->schedulers[ix].esc_tx_release_mask = 0;
    }

    memset(s->regs.shadow, 0, sizeof(OtShadowReg) * s->reg_count);
    memset(s->alert_levels, 0, sizeof(bool) * s->n_alerts);
    s->crashdump_latched = 0;
    memset(s->latched_dump, 0, sizeof(s->latched_dump));

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

    for (unsigned ix = 0; ix < PARAM_N_ESC_SEV; ix++) {
        ibex_irq_set(&s->esc_txs[ix], 0);
    }

    ot_alert_update_irqs(s);

    for (unsigned ix = 0; ix < OT_ALERT_CLOCK_SRC_COUNT; ix++) {
        if (s->clock_src_names[ix]) {
            continue;
        }
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_names[ix] = ic->get_clock_source(ii, s->clock_names[ix],
                                                      DEVICE(s), &error_fatal);
        qemu_irq in_irq =
            qdev_get_gpio_in_named(DEVICE(s), "clock-in", (int)ix);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_names[ix], 0,
                                    in_irq);
    }
}

static void ot_alert_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    OtAlertState *s = OT_ALERT(dev);

    g_assert(s->ot_id);
    g_assert(s->n_alerts != 0);
    g_assert(s->n_classes > 0 && s->n_classes <= 32);
    for (unsigned ix = 0; ix < OT_ALERT_CLOCK_SRC_COUNT; ix++) {
        g_assert(s->clock_names[ix]);
    }
    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    qdev_init_gpio_in_named(DEVICE(s), &ot_alert_clock_input, "clock-in",
                            OT_ALERT_CLOCK_SRC_COUNT);

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

    s->alert_levels = g_new0(bool, s->n_alerts);
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
        s->schedulers[ix].esc_tx_release_mask = 0;
    }

    timer_init_full(&s->reseed_timer, NULL, OT_VIRTUAL_CLOCK, SCALE_NS, 0,
                    &ot_alert_reseed_timer_cb, s);

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

static void ot_alert_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_alert_realize;
    device_class_set_props(dc, ot_alert_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtAlertClass *ac = OT_ALERT_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_alert_reset_enter, NULL, NULL,
                                       &ac->parent_phases);
}

static const TypeInfo ot_alert_info = {
    .name = TYPE_OT_ALERT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtAlertState),
    .class_size = sizeof(OtAlertClass),
    .class_init = &ot_alert_class_init,
};

static void ot_alert_register_types(void)
{
    type_register_static(&ot_alert_info);
}

type_init(ot_alert_register_types);
