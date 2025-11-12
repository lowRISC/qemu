/*
 * QEMU OpenTitan SoC Debug Controller
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
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
 * Based on OpenTitan c5507b4cdc
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_lc_ctrl.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_soc_dbg_ctrl.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_gpio.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

#define NUM_ALERTS 2u

/* clang-format off */

/*
 * registers on core bus
 * note: use shared register definition to avoid defining unreadable constants
 */
REG32(CORE_ALERT_TEST, 0x00u)
    FIELD(CORE_ALERT_TEST, FATAL_FAULT, 0u, 1u)
    FIELD(CORE_ALERT_TEST, RECOV_CTRL_UPDATE_ERR, 1u, 1u)
REG32(CORE_DEBUG_POLICY_VALID_SHADOWED, 0x04u)
    SHARED_FIELD(DEBUG_POLICY_VALID, 0u, OT_MULTIBITBOOL4_WIDTH)
REG32(CORE_DEBUG_POLICY_CATEGORY_SHADOWED, 0x08u)
    SHARED_FIELD(DEBUG_POLICY_CATEGORY, 0u, 7u)
REG32(CORE_DEBUG_POLICY_RELOCKED, 0x0cu)
    SHARED_FIELD(DEBUG_POLICY_RELOCKED, 0u, OT_MULTIBITBOOL4_WIDTH)
REG32(CORE_TRACE_DEBUG_POLICY_CATEGORY, 0x10u)
    SHARED_FIELD(TRACE_DEBUG_POLICY_CATEGORY, 0u, 7u)
REG32(CORE_TRACE_DEBUG_POLICY_VALID_RELOCKED, 0x14u)
    SHARED_FIELD(TRACE_DEBUG_POLICY_VALID_RELOCKED_VALID, 0u,
                 OT_MULTIBITBOOL4_WIDTH)
    SHARED_FIELD(TRACE_DEBUG_POLICY_VALID_RELOCKED_RELOCKED, 4u,
                 OT_MULTIBITBOOL4_WIDTH)
REG32(CORE_STATUS, 0x18u)
    SHARED_FIELD(AUTH_DEBUG_INTENT_SET, 0u, 1u)
    SHARED_FIELD(AUTH_WINDOW_OPEN, 4u, 1u)
    SHARED_FIELD(AUTH_WINDOW_CLOSED, 5u, 1u)
    SHARED_FIELD(AUTH_UNLOCK_SUCCESS, 6u, 1u)
    SHARED_FIELD(AUTH_UNLOCK_FAILED, 7u, 1u)

/* registers reachable from JTAG */
REG32(JTAG_TRACE_DEBUG_POLICY_CATEGORY, 0x00u)
REG32(JTAG_TRACE_DEBUG_POLICY_VALID_RELOCKED, 0x04u)
REG32(JTAG_CONTROL, 0x08u)
    FIELD(JTAG_CONTROL, BOOT_CONTINUE, 0u, 1u)
REG32(JTAG_STATUS, 0x0cu)
REG32(JTAG_BOOT_STATUS, 0x10u)
    FIELD(JTAG_BOOT_STATUS, MAIN_CLK_STATUS, 0u, 1u)
    FIELD(JTAG_BOOT_STATUS, IO_CLK_STATUS, 1u, 1u)
    FIELD(JTAG_BOOT_STATUS, OTP_DONE, 2u, 1u)
    FIELD(JTAG_BOOT_STATUS, LC_DONE, 3u, 1u)
    FIELD(JTAG_BOOT_STATUS, CPU_FETCH_EN, 4u, 1u)
    FIELD(JTAG_BOOT_STATUS, HALT_FSM_STATE, 5u, 6u)
    FIELD(JTAG_BOOT_STATUS, BOOT_GREENLIGHT_DONE, 11u, 3u)
    FIELD(JTAG_BOOT_STATUS, BOOT_GREENLIGHT_GOOD, 14u, 3u)
REG32(JTAG_TRACE_SOC_DBG_STATE, 0x14u)

/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_CORE_LAST_REG (R_CORE_STATUS)
#define REGS_CORE_COUNT (R_CORE_LAST_REG + 1u)
#define REGS_CORE_SIZE  (REGS_CORE_COUNT * sizeof(uint32_t))

#define R_JTAG_LAST_REG (R_JTAG_TRACE_SOC_DBG_STATE)
#define REGS_JTAG_COUNT (R_JTAG_LAST_REG + 1u)
#define REGS_JTAG_SIZE  (REGS_JTAG_COUNT * sizeof(uint32_t))

#define CORE_ALERT_TEST_WMASK \
    (R_CORE_ALERT_TEST_FATAL_FAULT_MASK | \
     R_CORE_ALERT_TEST_RECOV_CTRL_UPDATE_ERR_MASK)
#define CORE_STATUS_WMASK \
    (AUTH_DEBUG_INTENT_SET_MASK | AUTH_WINDOW_OPEN_MASK | \
     AUTH_WINDOW_CLOSED_MASK | AUTH_UNLOCK_SUCCESS_MASK | \
     AUTH_UNLOCK_FAILED_MASK)
#define BOOT_ROM_MASK \
    ((1u << (R_JTAG_BOOT_STATUS_BOOT_GREENLIGHT_DONE_LENGTH)) - 1u)

enum {
    CONTINUE_CPU_BOOT_GOOD,
    CONTINUE_CPU_BOOT_DONE,
    CONTINUE_CPU_BOOT_COUNT,
} OtSoCDbgContinueCpuBoot;

typedef enum {
    ST_IDLE,
    ST_CHECK_LC_STATE,
    ST_WAIT4DFT_EN,
    ST_CHECK_HALT_PIN,
    ST_CHECK_JTAG_GO,
    ST_HALT_DONE,
    ST_COUNT,
} OtSoCDbgCtrlFsmState;

typedef enum {
    ALERT_FATAL,
    ALERT_RECOV,
    ALERT_COUNT,
} OtSoCDbgAlert;

typedef enum {
    ALERT_RECOV_POLICY_VALID,
    ALERT_RECOV_POLICY_CATEGORY,
    ALERT_RECOV_COUNT,
} OtSoCDbgRecovAlert;

typedef enum {
    OT_SOC_DBG_CATEGORY_LOCKED = 0b1010000,
    OT_SOC_DBG_CATEGORY_2 = 0b1001101,
    OT_SOC_DBG_CATEGORY_3 = 0b0001010,
    OT_SOC_DBG_CATEGORY_4 = 0b1100011,
} OtSocDbgCategory;

typedef struct {
    /* can be any value, comes either from a register or local value */
    uint8_t cat;
    /* can be any 4-bit value, from a register or local value */
    ot_mb4_t relocked;
    /* only local, a regular bool is sufficient */
    bool valid;
} OtSocDbgPolicy;

typedef struct {
    struct {
        OtShadowReg valid;
        OtShadowReg category;
        uint32_t relocked;
    } debug_policy;
    uint32_t status;
} OtSoCDbgCtrlRegs;

struct OtSoCDbgCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion core;
    MemoryRegion jtag;
    IbexIRQ alerts[ALERT_COUNT];
    IbexIRQ debug_policy;
    IbexIRQ continue_cpu_boot[CONTINUE_CPU_BOOT_COUNT];
    QEMUBH *fsm_tick_bh;

    OtSoCDbgCtrlRegs regs;
    OtSoCDbgCtrlFsmState fsm_state;
    OtSoCDbgState soc_dbg_state;
    OtSocDbgPolicy soc_dbg_policy; /* calculated policy */
    unsigned fsm_tick_count;
    OtPwrMgrBootStatus pwr_boot_status;
    uint16_t lc_broadcast_bm; /* OtLcCtrlBroadcast fields */
    uint8_t fatal_alert_bm;
    uint8_t recov_alert_bm;
    bool halt_cpu_boot;
    bool boot_continue;

    char *ot_id;
    OtLcCtrlState *lc_ctrl;
    bool dft_ignore;
};

struct OtSoCDbgCtrlClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

/* clang-format off */
static const uint8_t OT_SOC_DBG_FSM_STATE[ST_COUNT] = {
    [ST_IDLE] = 0b101000u,
    [ST_CHECK_LC_STATE] = 0b011101u,
    [ST_WAIT4DFT_EN] = 0b000110u,
    [ST_CHECK_HALT_PIN] = 0b110011u,
    [ST_CHECK_JTAG_GO] = 0b111110u,
    [ST_HALT_DONE] = 0b100101u,
};
/* clang-format on */

static_assert(ALERT_COUNT == NUM_ALERTS, "Invalid alert count");
static_assert(sizeof(OtSocDbgDebugPolicy) == sizeof(int),
              "Invalid OtSocDbgDebugPolicy size");
static_assert(OT_SOC_DBG_ST_COUNT == OT_LC_SOC_DBG_STATE_COUNT,
              "Invalid SoC Debug state count");

#define ROM_MASK ((1u << (R_BOOT_STATUS_ROM_CTRL_DONE_LENGTH - 1u)) - 1u)

#define REG_NAME(_kind_, _reg_) \
    ((((_reg_) <= REGS_##_kind_##_COUNT) && REG_##_kind_##_NAMES[_reg_]) ? \
         REG_##_kind_##_NAMES[_reg_] : \
         "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

static const char *REG_CORE_NAMES[REGS_CORE_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(CORE_ALERT_TEST),
    REG_NAME_ENTRY(CORE_DEBUG_POLICY_VALID_SHADOWED),
    REG_NAME_ENTRY(CORE_DEBUG_POLICY_CATEGORY_SHADOWED),
    REG_NAME_ENTRY(CORE_DEBUG_POLICY_RELOCKED),
    REG_NAME_ENTRY(CORE_TRACE_DEBUG_POLICY_CATEGORY),
    REG_NAME_ENTRY(CORE_TRACE_DEBUG_POLICY_VALID_RELOCKED),
    REG_NAME_ENTRY(CORE_STATUS),
    /* clang-format on */
};

static const char *REG_JTAG_NAMES[REGS_JTAG_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(JTAG_TRACE_DEBUG_POLICY_CATEGORY),
    REG_NAME_ENTRY(JTAG_TRACE_DEBUG_POLICY_VALID_RELOCKED),
    REG_NAME_ENTRY(JTAG_CONTROL),
    REG_NAME_ENTRY(JTAG_STATUS),
    REG_NAME_ENTRY(JTAG_BOOT_STATUS),
    REG_NAME_ENTRY(JTAG_TRACE_SOC_DBG_STATE),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

#define LC_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
static const char *LC_BROADCAST_NAMES[] = {
    /* clang-format off */
    LC_NAME_ENTRY(OT_LC_RAW_TEST_RMA),
    LC_NAME_ENTRY(OT_LC_DFT_EN),
    LC_NAME_ENTRY(OT_LC_NVM_DEBUG_EN),
    LC_NAME_ENTRY(OT_LC_HW_DEBUG_EN),
    LC_NAME_ENTRY(OT_LC_CPU_EN),
    LC_NAME_ENTRY(OT_LC_KEYMGR_EN),
    LC_NAME_ENTRY(OT_LC_ESCALATE_EN),
    LC_NAME_ENTRY(OT_LC_CHECK_BYP_EN),
    LC_NAME_ENTRY(OT_LC_CREATOR_SEED_SW_RW_EN),
    LC_NAME_ENTRY(OT_LC_OWNER_SEED_SW_RW_EN),
    LC_NAME_ENTRY(OT_LC_ISO_PART_SW_RD_EN),
    LC_NAME_ENTRY(OT_LC_ISO_PART_SW_WR_EN),
    LC_NAME_ENTRY(OT_LC_SEED_HW_RD_EN),
    LC_NAME_ENTRY(OT_LC_RMA),
    /* clang-format on */
};
#undef LC_NAME_ENTRY

#define LC_BCAST_NAME(_bit_) \
    (((unsigned)(_bit_)) < ARRAY_SIZE(LC_BROADCAST_NAMES) ? \
         LC_BROADCAST_NAMES[(_bit_)] : \
         "?")

#define SOC_DBG_NAME_ENTRY(_st_) [OT_SOC_DBG_ST_##_st_] = stringify(_st_)
static const char *SOC_DBG_NAMES[] = {
    /* clang-format off */
    SOC_DBG_NAME_ENTRY(BLANK),
    SOC_DBG_NAME_ENTRY(PRE_PROD),
    SOC_DBG_NAME_ENTRY(PROD),
    /* clang-format on */
};
#undef SOC_DBG_NAME_ENTRY

#define SOC_DBG_NAME(_st_) \
    (((unsigned)(_st_)) < ARRAY_SIZE(SOC_DBG_NAMES) ? SOC_DBG_NAMES[(_st_)] : \
                                                      "?")

#define STATE_NAME_ENTRY(_st_) [ST_##_st_] = stringify(_st_)
static const char *STATE_NAMES[ST_COUNT] = {
    /* clang-format off */
    STATE_NAME_ENTRY(IDLE),
    STATE_NAME_ENTRY(CHECK_LC_STATE),
    STATE_NAME_ENTRY(WAIT4DFT_EN),
    STATE_NAME_ENTRY(CHECK_HALT_PIN),
    STATE_NAME_ENTRY(CHECK_JTAG_GO),
    STATE_NAME_ENTRY(HALT_DONE),
    /* clang-format on */
};
#undef STATE_NAME_ENTRY
#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")
#define CHANGE_STATE(_s_, _st_) \
    ot_soc_dbg_ctrl_change_state_line(_s_, ST_##_st_, __LINE__)
#define SCHEDULE_FSM(_s_) ot_soc_dbg_ctrl_schedule_fsm(_s_, __func__, __LINE__)

static void ot_soc_dbg_ctrl_change_state_line(
    OtSoCDbgCtrlState *s, OtSoCDbgCtrlFsmState state, int line)
{
    trace_ot_soc_dbg_ctrl_change_state(s->ot_id, line, STATE_NAME(s->fsm_state),
                                       s->fsm_state, STATE_NAME(state), state);

    s->fsm_state = state;
}

static inline bool
ot_soc_dbg_ctrl_lc_test(const OtSoCDbgCtrlState *s, OtLcCtrlBroadcast lc_bc)
{
    return (bool)(s->lc_broadcast_bm & (1u << (unsigned)lc_bc));
}

static void
ot_soc_dbg_ctrl_update_alerts(OtSoCDbgCtrlState *s, uint32_t test_bm)
{
    bool alert;

    alert = (bool)s->fatal_alert_bm |
            (bool)(test_bm & R_CORE_ALERT_TEST_FATAL_FAULT_MASK);
    ibex_irq_set(&s->alerts[ALERT_FATAL], (int)alert);

    alert = (bool)s->recov_alert_bm |
            (bool)(test_bm & R_CORE_ALERT_TEST_RECOV_CTRL_UPDATE_ERR_MASK);
    ibex_irq_set(&s->alerts[ALERT_RECOV], (int)alert);

    if (test_bm) {
        /* alert test is transient */
        ibex_irq_set(&s->alerts[ALERT_FATAL], (int)(bool)s->fatal_alert_bm);
        ibex_irq_set(&s->alerts[ALERT_RECOV], (int)(bool)s->recov_alert_bm);
    }
}

static void
ot_soc_dbg_ctrl_schedule_fsm(OtSoCDbgCtrlState *s, const char *func, int line)
{
    s->fsm_tick_count += 1u;
    trace_ot_soc_dbg_ctrl_schedule_fsm(s->ot_id, func, line, s->fsm_tick_count);
    qemu_bh_schedule(s->fsm_tick_bh);
}

static void ot_soc_dbg_ctrl_tick_fsm(OtSoCDbgCtrlState *s)
{
    bool cpu_boot_done = false;

    trace_ot_soc_dbg_ctrl_tick_fsm(s->ot_id, STATE_NAME(s->fsm_state),
                                   s->lc_broadcast_bm, s->dft_ignore,
                                   s->pwr_boot_status.lc_done, s->halt_cpu_boot,
                                   s->boot_continue);

    switch (s->fsm_state) {
    case ST_IDLE:
        if (s->pwr_boot_status.lc_done) {
            CHANGE_STATE(s, CHECK_LC_STATE);
        }
        break;
    case ST_CHECK_LC_STATE:
        if ((s->lc_broadcast_bm & (1u << OT_LC_RAW_TEST_RMA)) &&
            !s->dft_ignore) {
            CHANGE_STATE(s, WAIT4DFT_EN);
        } else {
            CHANGE_STATE(s, HALT_DONE);
        }
        break;
    case ST_WAIT4DFT_EN:
        if (s->lc_broadcast_bm & (1u << OT_LC_DFT_EN)) {
            CHANGE_STATE(s, CHECK_HALT_PIN);
        }
        break;
    case ST_CHECK_HALT_PIN:
        if (s->halt_cpu_boot) {
            CHANGE_STATE(s, CHECK_JTAG_GO);
        } else {
            CHANGE_STATE(s, HALT_DONE);
        }
        break;
    case ST_CHECK_JTAG_GO:
        if (s->boot_continue) {
            CHANGE_STATE(s, HALT_DONE);
        }
        break;
    case ST_HALT_DONE:
        cpu_boot_done = true;
        break;
    default:
        /*
         * it does not seem there is a special state for this case, i.e.
         * the FSM does not enter a special error state, it only raises
         * an alert.
         */
        ibex_irq_set(&s->alerts[ALERT_FATAL], (int)true);
        return;
    }

    /* as with PwrMgr, use simple boolean value, not MuBi4 */
    int cpu_boot_done_i = (int)cpu_boot_done;
    if (ibex_irq_get_level(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_COUNT]) !=
        cpu_boot_done_i) {
        trace_ot_soc_dbg_ctrl_cpu_boot_done(s->ot_id, cpu_boot_done_i);
    }
    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_DONE],
                 cpu_boot_done_i);
}

static void ot_soc_dbg_ctrl_update_policy(OtSoCDbgCtrlState *s)
{
    OtSocDbgPolicy policy;

    ot_mb4_t relocked =
        (ot_mb4_t)SHARED_FIELD_EX32(s->regs.debug_policy.relocked,
                                    DEBUG_POLICY_RELOCKED);

    switch (s->soc_dbg_state) {
    case OT_SOC_DBG_ST_BLANK:
        policy.cat = ot_soc_dbg_ctrl_lc_test(s, OT_LC_DFT_EN) ||
                             ot_soc_dbg_ctrl_lc_test(s, OT_LC_HW_DEBUG_EN) ?
                         OT_SOC_DBG_CATEGORY_4 :
                         OT_SOC_DBG_CATEGORY_LOCKED;
        policy.valid = (bool)s->pwr_boot_status.lc_done;
        policy.relocked = OT_MULTIBITBOOL4_FALSE;
        break;
    case OT_SOC_DBG_ST_PRE_PROD:
        policy.cat = OT_SOC_DBG_CATEGORY_4;
        policy.valid = (bool)s->pwr_boot_status.lc_done;
        policy.relocked = OT_MULTIBITBOOL4_FALSE;
        break;
    case OT_SOC_DBG_ST_PROD:
        if (ot_soc_dbg_ctrl_lc_test(s, OT_LC_RMA)) {
            policy.cat = OT_SOC_DBG_CATEGORY_4;
            policy.valid = true;
            policy.relocked = OT_MULTIBITBOOL4_FALSE;
        } else if (ot_soc_dbg_ctrl_lc_test(s, OT_LC_CPU_EN)) {
            policy.cat = ot_shadow_reg_peek(&s->regs.debug_policy.category);
            policy.valid = true;
            policy.relocked = relocked;
        } else {
            policy.cat = OT_SOC_DBG_CATEGORY_LOCKED;
            policy.valid = (bool)s->pwr_boot_status.lc_done;
            policy.relocked = OT_MULTIBITBOOL4_FALSE;
        }
        break;
    default:
        policy.cat = OT_SOC_DBG_CATEGORY_LOCKED;
        policy.valid = false;
        policy.relocked = OT_MULTIBITBOOL4_FALSE;
        break;
    }

    /* detect valid rising edge */
    if (policy.valid && !s->soc_dbg_policy.valid) {
        /* decode the values into the combined output signal */
        OtSocDbgDebugPolicy debug_policy = {
            .cat_bm = 0u,
            .relocked = policy.relocked == OT_MULTIBITBOOL4_TRUE,
        };

        if (policy.cat == OT_SOC_DBG_CATEGORY_4) {
            debug_policy.cat_bm |= 1u << 4u;
        }
        if (!relocked) {
            if (policy.cat == OT_SOC_DBG_CATEGORY_3 ||
                (debug_policy.cat_bm & (1u << 4u))) {
                debug_policy.cat_bm |= 1u << 3u;
            }
            if (policy.cat == OT_SOC_DBG_CATEGORY_2 ||
                (debug_policy.cat_bm & ((1u << 4u) | (1u << 3u)))) {
                debug_policy.cat_bm |= 1u << 2u;
            }
        }

        int prev_policy = ibex_irq_get_level(&s->debug_policy);
        if (prev_policy != debug_policy.i32) {
            trace_ot_soc_dbg_ctrl_update_policy(s->ot_id,
                                                SOC_DBG_NAME(s->soc_dbg_state),
                                                STATE_NAME(s->fsm_state),
                                                debug_policy.cat_bm, relocked);
        }
        ibex_irq_set(&s->debug_policy, debug_policy.i32);

        /* store current policy for edge detection and trace registers */
        s->soc_dbg_policy = policy;
    } else {
        /* store only valid level for edge detection */
        s->soc_dbg_policy.valid = policy.valid;
    }
}

static void ot_soc_dbg_ctrl_fsm_tick(void *opaque)
{
    OtSoCDbgCtrlState *s = opaque;

    OtSoCDbgCtrlFsmState fsm_state = s->fsm_state;
    g_assert(s->fsm_tick_count);
    while (s->fsm_tick_count) {
        ot_soc_dbg_ctrl_update_policy(s);
        s->fsm_tick_count--;
        ot_soc_dbg_ctrl_tick_fsm(s);
    }
    if (fsm_state != s->fsm_state) {
        /* schedule FSM update once more if its state has changed */
        SCHEDULE_FSM(s);
    }
}


static void ot_soc_dbg_ctrl_halt_cpu_boot(void *opaque, int n, int level)
{
    OtSoCDbgCtrlState *s = opaque;

    g_assert(n == 0);

    trace_ot_soc_dbg_ctrl_rcv(s->ot_id, "HALT_CPU_BOOT", 0,
                              ibex_gpio_repr(level));

    /* expect an Ibex GPIO signal */
    g_assert(ibex_gpio_check(level));

    /* active low */
    s->halt_cpu_boot = !ibex_gpio_level(level);

    SCHEDULE_FSM(s);
}

static void ot_soc_dbg_ctrl_lc_broadcast(void *opaque, int n, int level)
{
    OtSoCDbgCtrlState *s = opaque;

    unsigned bcast = (unsigned)n;
    g_assert(bcast < OT_LC_BROADCAST_COUNT);
    g_assert(!ibex_gpio_check(level));

    trace_ot_soc_dbg_ctrl_rcv(s->ot_id, LC_BCAST_NAME(bcast), bcast,
                              level ? '1' : '0');

    switch (n) {
    case OT_LC_RAW_TEST_RMA:
    case OT_LC_DFT_EN:
    case OT_LC_HW_DEBUG_EN:
    case OT_LC_CPU_EN:
        if (level) {
            s->lc_broadcast_bm |= (1u << bcast);
        } else {
            s->lc_broadcast_bm &= (~1u << bcast);
        }
        break;
    /* NOLINTBEGIN(bugprone-branch-clone) */
    case OT_LC_NVM_DEBUG_EN:
    case OT_LC_KEYMGR_EN:
    case OT_LC_ISO_PART_SW_RD_EN:
    case OT_LC_ISO_PART_SW_WR_EN:
    case OT_LC_OWNER_SEED_SW_RW_EN:
        /* do not seem to be routed... */
        break;
    case OT_LC_CREATOR_SEED_SW_RW_EN:
    case OT_LC_SEED_HW_RD_EN:
    case OT_LC_ESCALATE_EN:
    case OT_LC_CHECK_BYP_EN:
        /* verbatim from RTL: "Use unused signals to make lint clean" */
        break;
    /* NOLINTEND(bugprone-branch-clone) */
    default:
        g_assert_not_reached();
    }

    SCHEDULE_FSM(s);
}

static void ot_soc_dbg_ctrl_boot_status(void *opaque, int n, int level)
{
    OtSoCDbgCtrlState *s = opaque;

    g_assert(n == 0);

    OtPwrMgrBootStatus bs = { .i32 = level };

    g_assert(bs.rom_mask != 0);
    g_assert(!(bs.rom_mask & ~BOOT_ROM_MASK));

    trace_ot_soc_dbg_ctrl_boot_status(s->ot_id, (bool)bs.main_clk_status,
                                      (bool)bs.io_clk_status, (bool)bs.otp_done,
                                      (bool)bs.lc_done, (bool)bs.cpu_fetch_en,
                                      bs.rom_done & bs.rom_mask,
                                      bs.rom_good & bs.rom_mask);
    s->pwr_boot_status = bs;

    SCHEDULE_FSM(s);
}

static uint32_t ot_soc_dbg_ctrl_get_jtag_boot_status(const OtSoCDbgCtrlState *s)
{
    const OtPwrMgrBootStatus *bs = &s->pwr_boot_status;

    g_assert(s->fsm_state < ST_COUNT);

    uint32_t bs_bm = 0;

    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, MAIN_CLK_STATUS,
                       bs->main_clk_status);
    bs_bm =
        FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, IO_CLK_STATUS, bs->io_clk_status);
    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, OTP_DONE, bs->otp_done);
    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, LC_DONE, bs->lc_done);
    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, BOOT_GREENLIGHT_DONE,
                       bs->rom_done & bs->rom_mask);
    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, BOOT_GREENLIGHT_GOOD,
                       bs->rom_good & bs->rom_mask);
    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, CPU_FETCH_EN, bs->cpu_fetch_en);
    bs_bm = FIELD_DP32(bs_bm, JTAG_BOOT_STATUS, HALT_FSM_STATE,
                       (uint32_t)OT_SOC_DBG_FSM_STATE[s->fsm_state]);

    return bs_bm;
}

static void ot_soc_dbg_ctrl_soc_dbg_state(void *opaque, int n, int level)
{
    OtSoCDbgCtrlState *s = opaque;

    g_assert(n == 0);
    g_assert(!ibex_gpio_check(level));

    switch (level) {
    case 0:
        s->soc_dbg_state = OT_SOC_DBG_ST_BLANK;
        break;
    case 1:
        s->soc_dbg_state = OT_SOC_DBG_ST_PRE_PROD;
        break;
    case 2:
        s->soc_dbg_state = OT_SOC_DBG_ST_PROD;
        break;
    default:
        g_assert_not_reached();
    }

    trace_ot_soc_dbg_ctrl_rcv(s->ot_id, "SOC_DBG_STATE", 0,
                              (char)('0' + level));

    trace_ot_soc_dbg_ctrl_soc_dbg_state(s->ot_id,
                                        SOC_DBG_NAME(s->soc_dbg_state));

    SCHEDULE_FSM(s);
}

static uint64_t
ot_soc_dbg_ctrl_core_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSoCDbgCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = 0;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_CORE_DEBUG_POLICY_VALID_SHADOWED:
        val32 = ot_shadow_reg_read(&s->regs.debug_policy.valid);
        break;
    case R_CORE_DEBUG_POLICY_CATEGORY_SHADOWED:
        val32 = ot_shadow_reg_read(&s->regs.debug_policy.category);
        break;
    case R_CORE_DEBUG_POLICY_RELOCKED:
        val32 = s->regs.debug_policy.relocked;
        break;
    case R_CORE_TRACE_DEBUG_POLICY_CATEGORY:
        val32 = (uint32_t)s->soc_dbg_policy.cat;
        break;
    case R_CORE_TRACE_DEBUG_POLICY_VALID_RELOCKED:
        val32 = (uint32_t)s->soc_dbg_policy.relocked;
        break;
    case R_CORE_STATUS:
        val32 = s->regs.status;
        break;
    case R_CORE_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(CORE, reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_soc_dbg_ctrl_core_io_read_out(s->ot_id, (uint32_t)addr,
                                           REG_NAME(CORE, reg), val32, pc);

    return (uint32_t)val32;
}

static void ot_soc_dbg_ctrl_core_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned size)
{
    OtSoCDbgCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_soc_dbg_ctrl_core_io_write(s->ot_id, (uint32_t)addr,
                                        REG_NAME(CORE, reg), val32, pc);

    switch (reg) {
    case R_CORE_ALERT_TEST:
        val32 &= CORE_ALERT_TEST_WMASK;
        ot_soc_dbg_ctrl_update_alerts(s, val32);
        break;
    case R_CORE_DEBUG_POLICY_VALID_SHADOWED:
        val32 &= DEBUG_POLICY_VALID_MASK;
        switch (ot_shadow_reg_write(&s->regs.debug_policy.valid, val32)) {
        case OT_SHADOW_REG_STAGED:
        case OT_SHADOW_REG_COMMITTED:
            s->recov_alert_bm &= ~(1u << ALERT_RECOV_POLICY_VALID);
            break;
        case OT_SHADOW_REG_ERROR:
            s->recov_alert_bm |= 1u << ALERT_RECOV_POLICY_VALID;
        default:
            break;
        }
        ot_soc_dbg_ctrl_update_alerts(s, 0u);
        break;
    case R_CORE_DEBUG_POLICY_CATEGORY_SHADOWED:
        val32 &= DEBUG_POLICY_CATEGORY_MASK;
        if (ot_shadow_reg_peek(&s->regs.debug_policy.valid) ==
            OT_MULTIBITBOOL4_FALSE) {
            /* debug policy not yet validated, can still update it */
            switch (
                ot_shadow_reg_write(&s->regs.debug_policy.category, val32)) {
            case OT_SHADOW_REG_STAGED:
            case OT_SHADOW_REG_COMMITTED:
                s->recov_alert_bm &= ~(1u << ALERT_RECOV_POLICY_CATEGORY);
                break;
            case OT_SHADOW_REG_ERROR:
                s->recov_alert_bm |= 1u << ALERT_RECOV_POLICY_CATEGORY;
            default:
                break;
            }
            ot_soc_dbg_ctrl_update_alerts(s, 0u);
        }
        break;
    case R_CORE_DEBUG_POLICY_RELOCKED:
        val32 &= DEBUG_POLICY_RELOCKED_MASK;
        s->regs.debug_policy.relocked = val32;
        break;
    case R_CORE_TRACE_DEBUG_POLICY_CATEGORY:
    case R_CORE_TRACE_DEBUG_POLICY_VALID_RELOCKED:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: R/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(CORE, reg));
        break;
    case R_CORE_STATUS:
        /*
         * this register does nothing but masks some bits and make them
         * readable from JTAG side.
         */
        val32 &= CORE_STATUS_WMASK;
        s->regs.status = val32;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        break;
    }
}

static uint64_t
ot_soc_dbg_ctrl_jtag_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSoCDbgCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = 0;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_JTAG_TRACE_DEBUG_POLICY_CATEGORY:
        val32 = (uint32_t)s->soc_dbg_policy.cat;
        break;
    case R_JTAG_TRACE_DEBUG_POLICY_VALID_RELOCKED:
        val32 = (uint32_t)s->soc_dbg_policy.relocked;
        break;
    case R_JTAG_CONTROL:
        val32 = s->boot_continue ? R_JTAG_CONTROL_BOOT_CONTINUE_MASK : 0u;
        break;
    case R_JTAG_STATUS:
        val32 = s->regs.status;
        break;
    case R_JTAG_BOOT_STATUS:
        if (ot_soc_dbg_ctrl_lc_test(s, OT_LC_DFT_EN)) {
            val32 = ot_soc_dbg_ctrl_get_jtag_boot_status(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: BootStatus disabled (no DFT)\n", __func__,
                          s->ot_id);
            val32 = 0;
        }
        break;
    case R_JTAG_TRACE_SOC_DBG_STATE:
        if (ot_soc_dbg_ctrl_lc_test(s, OT_LC_DFT_EN)) {
            OtLcCtrlClass *lc = OT_LC_CTRL_GET_CLASS(s->lc_ctrl);
            val32 =
                lc->get_soc_dbg_state(s->lc_ctrl, (unsigned)s->soc_dbg_state);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: SocDbgState disabled (no DFT)\n", __func__,
                          s->ot_id);
            val32 = 0u;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        val32 = 0;
        break;
    }

    trace_ot_soc_dbg_ctrl_jtag_io_read_out(s->ot_id, (uint32_t)addr,
                                           REG_NAME(JTAG, reg), val32);

    return (uint32_t)val32;
}

static void ot_soc_dbg_ctrl_jtag_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned size)
{
    OtSoCDbgCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)value;

    hwaddr reg = R32_OFF(addr);

    trace_ot_soc_dbg_ctrl_jtag_io_write(s->ot_id, (uint32_t)addr,
                                        REG_NAME(JTAG, reg), val32);

    switch (reg) {
    case R_JTAG_CONTROL:
        s->boot_continue = (bool)(val32 & R_JTAG_CONTROL_BOOT_CONTINUE_MASK);
        SCHEDULE_FSM(s);
        break;
    case R_JTAG_TRACE_DEBUG_POLICY_CATEGORY:
    case R_JTAG_TRACE_DEBUG_POLICY_VALID_RELOCKED:
    case R_JTAG_STATUS:
    case R_JTAG_BOOT_STATUS:
    case R_JTAG_TRACE_SOC_DBG_STATE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: R/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(JTAG, reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s, Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        break;
    }
}

static Property ot_soc_dbg_ctrl_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtSoCDbgCtrlState, ot_id),
    DEFINE_PROP_LINK("lc-ctrl", OtSoCDbgCtrlState, lc_ctrl, TYPE_OT_LC_CTRL,
                     OtLcCtrlState *),
    DEFINE_PROP_BOOL("dft-ignore", OtSoCDbgCtrlState, dft_ignore, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_soc_dbg_ctrl_core_ops = {
    .read = &ot_soc_dbg_ctrl_core_read,
    .write = &ot_soc_dbg_ctrl_core_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_soc_dbg_ctrl_jtag_ops = {
    .read = &ot_soc_dbg_ctrl_jtag_read,
    .write = &ot_soc_dbg_ctrl_jtag_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_soc_dbg_ctrl_reset_enter(Object *obj, ResetType type)
{
    OtSoCDbgCtrlClass *c = OT_SOC_DBG_CTRL_GET_CLASS(obj);
    OtSoCDbgCtrlState *s = OT_SOC_DBG_CTRL(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    ot_shadow_reg_init(&s->regs.debug_policy.valid, OT_MULTIBITBOOL4_FALSE);
    ot_shadow_reg_init(&s->regs.debug_policy.category,
                       OT_SOC_DBG_CATEGORY_LOCKED);
    s->regs.debug_policy.relocked = OT_MULTIBITBOOL4_FALSE;

    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_DONE], (int)false);
    ibex_irq_set(&s->debug_policy, 0);

    memset(&s->soc_dbg_policy, 0, sizeof(s->soc_dbg_policy));

    CHANGE_STATE(s, IDLE);
    s->fsm_state = ST_IDLE;
    s->fsm_tick_count = 0u;
    s->pwr_boot_status.i32 = 0;
    s->lc_broadcast_bm = 0u;
    s->soc_dbg_state = OT_SOC_DBG_ST_PROD;
    s->halt_cpu_boot = false;
    s->boot_continue = false;
    s->fatal_alert_bm = 0u;
    s->recov_alert_bm = 0u;

    ot_soc_dbg_ctrl_update_alerts(s, 0u);
}

static void ot_soc_dbg_ctrl_reset_exit(Object *obj, ResetType type)
{
    OtSoCDbgCtrlClass *c = OT_SOC_DBG_CTRL_GET_CLASS(obj);
    OtSoCDbgCtrlState *s = OT_SOC_DBG_CTRL(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    qemu_bh_cancel(s->fsm_tick_bh);

    /* this fake ROM signal is hardcoded to true and never updated. */
    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_GOOD], (int)true);

    ot_soc_dbg_ctrl_update_policy(s);

    SCHEDULE_FSM(s);
}

static void ot_soc_dbg_ctrl_realize(DeviceState *dev, Error **errp)
{
    OtSoCDbgCtrlState *s = OT_SOC_DBG_CTRL(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->lc_ctrl);
}

static void ot_soc_dbg_ctrl_init(Object *obj)
{
    OtSoCDbgCtrlState *s = OT_SOC_DBG_CTRL(obj);

    memory_region_init_io(&s->core, obj, &ot_soc_dbg_ctrl_core_ops, s,
                          TYPE_OT_SOC_DBG_CTRL, REGS_CORE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->core);

    memory_region_init_io(&s->jtag, obj, &ot_soc_dbg_ctrl_jtag_ops, s,
                          TYPE_OT_SOC_DBG_CTRL, REGS_JTAG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->jtag);

    for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }
    ibex_qdev_init_irq(obj, &s->debug_policy, OT_SOC_DBG_DEBUG_POLICY);
    ibex_qdev_init_irqs(obj, s->continue_cpu_boot, OT_SOC_DBG_CONTINUE_CPU_BOOT,
                        ARRAY_SIZE(s->continue_cpu_boot));

    qdev_init_gpio_in_named(DEVICE(obj), &ot_soc_dbg_ctrl_halt_cpu_boot,
                            OT_SOC_DBG_HALT_CPU_BOOT, 1);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_soc_dbg_ctrl_lc_broadcast,
                            OT_SOC_DBG_LC_BCAST, OT_LC_BROADCAST_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_soc_dbg_ctrl_soc_dbg_state,
                            OT_SOC_DBG_STATE, 1);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_soc_dbg_ctrl_boot_status,
                            OT_SOC_DBG_BOOT_STATUS, 1u);

    s->fsm_tick_bh = qemu_bh_new(&ot_soc_dbg_ctrl_fsm_tick, s);
}

static void ot_soc_dbg_ctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_soc_dbg_ctrl_realize;
    device_class_set_props(dc, ot_soc_dbg_ctrl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtSoCDbgCtrlClass *sc = OT_SOC_DBG_CTRL_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_soc_dbg_ctrl_reset_enter, NULL,
                                       &ot_soc_dbg_ctrl_reset_exit,
                                       &sc->parent_phases);
}

static const TypeInfo ot_soc_dbg_ctrl_info = {
    .name = TYPE_OT_SOC_DBG_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSoCDbgCtrlState),
    .instance_init = &ot_soc_dbg_ctrl_init,
    .class_size = sizeof(OtSoCDbgCtrlClass),
    .class_init = &ot_soc_dbg_ctrl_class_init,
};

static void ot_soc_dbg_ctrl_register_types(void)
{
    type_register_static(&ot_soc_dbg_ctrl_info);
}

type_init(ot_soc_dbg_ctrl_register_types);
