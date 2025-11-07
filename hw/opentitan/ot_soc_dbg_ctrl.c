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

/* clang-format off */

/* registers on core bus */
REG32(CORE_ALERT_TEST, 0x0cu)
    FIELD(CORE_ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CORE_DEBUG_POLICY_CTRL, 0x10u)
    /* 4 bits as it seems to be relock (1) + policy (3) */
    FIELD(CORE_DEBUG_POLICY_CTRL, LEVEL, 0u, 4u)
REG32(CORE_DEBUG_POLICY_VALID, 0x14u)
    FIELD(CORE_DEBUG_POLICY_VALID, VALID, 0u, 1u)
REG32(CORE_STATUS_MBX, 0x18u)
    /* shared by CORE_STATUS_MBX and JTAG_STATUS */
    SHARED_FIELD(AUTH_DEBUG_INTENT_SET, 0u, 1u)
    SHARED_FIELD(AUTH_WINDOW_OPEN, 4u, 1u)
    SHARED_FIELD(AUTH_WINDOW_CLOSED, 5u, 1u)
    SHARED_FIELD(AUTH_UNLOCK_SUCCESS, 6u, 1u)
    SHARED_FIELD(AUTH_UNLOCK_FAILED, 7u, 1u)
    /* this is not HW-connected to CORE_DEBUG_POLICY_CTRL */
    SHARED_FIELD(CURRENT_POLICY, 8u, 4u)
    SHARED_FIELD(REQUESTED_POLICY, 12u, 4u)

/* registers reachable from JTAG */
REG32(JTAG_CONTROL, 0x0)
    FIELD(JTAG_CONTROL, BOOT_CONTINUE, 0u, 1u)
    REG32(JTAG_STATUS, 0x4)
    REG32(JTAG_BOOT_STATUS, 0x8)

/* boot_status_bm fields */
REG16(BOOT_STATUS, 0x0)
    FIELD(BOOT_STATUS, MAIN_CLK_STATUS, 0u, 1u)
    FIELD(BOOT_STATUS, IO_CLK_STATUS, 1u, 1u)
    FIELD(BOOT_STATUS, USB_CLK_STATUS, 2u, 1u)
    FIELD(BOOT_STATUS, OTP_DONE, 3u, 1u)
    FIELD(BOOT_STATUS, LC_DONE, 4u, 1u)
    FIELD(BOOT_STATUS, ROM_CTRL_DONE, 5u, 3u)
    FIELD(BOOT_STATUS, ROM_CTRL_GOOD, 8u, 3u)
    FIELD(BOOT_STATUS, CPU_FETCH_EN, 11u, 1u)

/* soc_dbg_bm fields */
REG16(SOC_DBG, 0x0)
    FIELD(SOC_DBG, HALT_CPU_BOOT, 2u, 1u)

/* debug_policy fields */
SHARED_FIELD(POLICY_CAT, 0u, 2u)
SHARED_FIELD(POLICY_RELOCK, 2u, 1u)
SHARED_FIELD(POLICY_UNUSED, 3u, 1u)

/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_CORE_LAST_REG (R_CORE_STATUS_MBX)
#define REGS_CORE_COUNT (R_CORE_LAST_REG + 1u)
#define REGS_CORE_SIZE  (REGS_CORE_COUNT * sizeof(uint32_t))

#define R_JTAG_LAST_REG (R_JTAG_BOOT_STATUS)
#define REGS_JTAG_COUNT (R_JTAG_LAST_REG + 1u)
#define REGS_JTAG_SIZE  (REGS_JTAG_COUNT * sizeof(uint32_t))

#define CORE_ALERT_TEST_MASK (R_CORE_ALERT_TEST_FATAL_FAULT_MASK)
#define STATUS_MASK \
    (AUTH_DEBUG_INTENT_SET_MASK | AUTH_WINDOW_OPEN_MASK | \
     AUTH_WINDOW_CLOSED_MASK | AUTH_UNLOCK_SUCCESS_MASK | \
     AUTH_UNLOCK_FAILED_MASK | CURRENT_POLICY_MASK | REQUESTED_POLICY_MASK)

#define DEFAULT_DBG_UNLOCKED 0u
#define DEFAULT_DBG_LOCKED   7u

enum {
    CONTINUE_CPU_BOOT_GOOD,
    CONTINUE_CPU_BOOT_DONE,
    CONTINUE_CPU_BOOT_COUNT,
} OtSoCDbgContinueCpuBoot;

typedef enum {
    ST_IDLE,
    ST_CHECK_LC_ST,
    ST_WAIT4_DFT_EN,
    ST_CHECK_HALT_PIN,
    ST_CHECK_JTAG_GO,
    ST_CONTINUE_BOOT,
    ST_HALT_DONE,
} OtSoCDbgCtrlFsmState;

struct OtSoCDbgCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion core;
    MemoryRegion jtag;
    IbexIRQ alert;
    IbexIRQ policy;
    IbexIRQ continue_cpu_boot[CONTINUE_CPU_BOOT_COUNT];
    QEMUBH *fsm_tick_bh;

    uint32_t regs[REGS_CORE_COUNT];
    OtSoCDbgCtrlFsmState fsm_state;
    OtSoCDbgState soc_dbg_state;
    unsigned debug_policy;
    unsigned fsm_tick_count;
    uint16_t boot_status_bm; /* BOOT_STATUS fields */
    uint16_t soc_dbg_bm; /* SOC_DBG fields */
    uint16_t lc_broadcast_bm; /* OtLcCtrlBroadcast fields */
    bool boot_continue;
    bool debug_valid;

    char *ot_id;
    bool halt_function;
    bool dft_ignore;
};

struct OtSoCDbgCtrlClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#define ROM_MASK ((1u << (R_BOOT_STATUS_ROM_CTRL_DONE_LENGTH - 1u)) - 1u)

#define REG_NAME(_kind_, _reg_) \
    ((((_reg_) <= REGS_##_kind_##_COUNT) && REG_##_kind_##_NAMES[_reg_]) ? \
         REG_##_kind_##_NAMES[_reg_] : \
         "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

static const char *REG_CORE_NAMES[REGS_CORE_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(CORE_ALERT_TEST),
    REG_NAME_ENTRY(CORE_DEBUG_POLICY_CTRL),
    REG_NAME_ENTRY(CORE_DEBUG_POLICY_VALID),
    REG_NAME_ENTRY(CORE_STATUS_MBX),
    /* clang-format on */
};

static const char *REG_JTAG_NAMES[REGS_JTAG_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(JTAG_CONTROL),
    REG_NAME_ENTRY(JTAG_STATUS),
    REG_NAME_ENTRY(JTAG_BOOT_STATUS),
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
static const char *STATE_NAMES[] = {
    /* clang-format off */
    STATE_NAME_ENTRY(IDLE),
    STATE_NAME_ENTRY(CHECK_LC_ST),
    STATE_NAME_ENTRY(WAIT4_DFT_EN),
    STATE_NAME_ENTRY(CHECK_HALT_PIN),
    STATE_NAME_ENTRY(CHECK_JTAG_GO),
    STATE_NAME_ENTRY(CONTINUE_BOOT),
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
                                   s->boot_status_bm, s->lc_broadcast_bm,
                                   s->soc_dbg_bm, s->dft_ignore,
                                   s->boot_continue);

    switch (s->fsm_state) {
    case ST_IDLE:
        if (s->boot_status_bm & R_BOOT_STATUS_LC_DONE_MASK) {
            CHANGE_STATE(s, CHECK_LC_ST);
        }
        break;
    case ST_CHECK_LC_ST:
        if ((s->lc_broadcast_bm & (1u << OT_LC_RAW_TEST_RMA)) &&
            !s->dft_ignore) {
            CHANGE_STATE(s, WAIT4_DFT_EN);
        } else {
            CHANGE_STATE(s, CONTINUE_BOOT);
        }
        break;
    case ST_WAIT4_DFT_EN:
        if (s->lc_broadcast_bm & (1u << OT_LC_DFT_EN)) {
            CHANGE_STATE(s, CHECK_HALT_PIN);
        }
        break;
    case ST_CHECK_HALT_PIN:
        if (s->soc_dbg_bm & R_SOC_DBG_HALT_CPU_BOOT_MASK) {
            CHANGE_STATE(s, CHECK_JTAG_GO);
        } else {
            CHANGE_STATE(s, CONTINUE_BOOT);
        }
        break;
    case ST_CHECK_JTAG_GO:
        if (s->boot_continue) {
            CHANGE_STATE(s, CONTINUE_BOOT);
        }
        break;
    case ST_CONTINUE_BOOT:
        CHANGE_STATE(s, HALT_DONE);
        break;
    case ST_HALT_DONE:
        cpu_boot_done = true;
        break;
    default:
        /* it does not seem there is a special state for this case */
        ibex_irq_set(&s->alert, (int)true);
        return;
    }

    /* as with PwrMgr, use simple boolean value, not MuBi4 */
    int cpu_boot_done_i = (int)cpu_boot_done;
    if (ibex_irq_get_level(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_DONE]) !=
        cpu_boot_done_i) {
        trace_ot_soc_dbg_ctrl_cpu_boot_done(s->ot_id, cpu_boot_done_i);
    }
    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_DONE],
                 cpu_boot_done_i);
}

static void ot_soc_dbg_ctrl_update(OtSoCDbgCtrlState *s)
{
    switch (s->soc_dbg_state) {
    case OT_SOC_DBG_ST_BLANK:
        s->debug_policy =
            s->lc_broadcast_bm & (1u << OT_LC_DFT_EN) ||
                    s->lc_broadcast_bm & (1u << OT_LC_HW_DEBUG_EN) ?
                DEFAULT_DBG_UNLOCKED :
                DEFAULT_DBG_LOCKED;
        s->debug_valid = (bool)(s->boot_status_bm & R_BOOT_STATUS_LC_DONE_MASK);
        break;
    case OT_SOC_DBG_ST_PRE_PROD:
        s->debug_policy = DEFAULT_DBG_UNLOCKED;
        s->debug_valid = (bool)(s->boot_status_bm & R_BOOT_STATUS_LC_DONE_MASK);
        break;
    case OT_SOC_DBG_ST_PROD:
        s->debug_policy = s->regs[R_CORE_DEBUG_POLICY_CTRL] &
                          (POLICY_CAT_MASK | POLICY_RELOCK_MASK);
        s->debug_valid = (bool)s->regs[R_CORE_DEBUG_POLICY_VALID];
        break;
    default:
        s->debug_policy = DEFAULT_DBG_LOCKED;
        s->debug_valid = false;
    }

    int policy = (int)((s->debug_policy & OT_SOC_DBG_DEBUG_POLICY_MASK) |
                       (s->debug_valid ? OT_SOC_DBG_DEBUG_VALID_MASK : 0));

    int prev_policy = ibex_irq_get_level(&s->policy);
    if (prev_policy != policy) {
        trace_ot_soc_dbg_ctrl_update(s->ot_id, SOC_DBG_NAME(s->soc_dbg_state),
                                     STATE_NAME(s->fsm_state), s->debug_policy,
                                     s->debug_valid);
    }
    ibex_irq_set(&s->policy, policy);
}

static void ot_soc_dbg_ctrl_fsm_tick(void *opaque)
{
    OtSoCDbgCtrlState *s = opaque;

    OtSoCDbgCtrlFsmState fsm_state = s->fsm_state;
    g_assert(s->fsm_tick_count);
    while (s->fsm_tick_count) {
        ot_soc_dbg_ctrl_update(s);
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
    if (!ibex_gpio_level(level)) {
        s->soc_dbg_bm |= R_SOC_DBG_HALT_CPU_BOOT_MASK;
    } else {
        s->soc_dbg_bm &= ~R_SOC_DBG_HALT_CPU_BOOT_MASK;
    }

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
        /* verbatim from RTL: "Use unused singals to make lint clean" */

        /* why do we explictly route signals that are then discarded? */
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
    trace_ot_soc_dbg_ctrl_boot_status(s->ot_id, (bool)bs.main_clk_status,
                                      (bool)bs.io_clk_status, (bool)bs.otp_done,
                                      (bool)bs.lc_done, (bool)bs.cpu_fetch_en,
                                      bs.rom_done & ROM_MASK,
                                      bs.rom_good & ROM_MASK);
    uint16_t bs_bm = 0;
    bs_bm = FIELD_DP16(bs_bm, BOOT_STATUS, MAIN_CLK_STATUS, bs.main_clk_status);
    bs_bm = FIELD_DP16(bs_bm, BOOT_STATUS, IO_CLK_STATUS, bs.io_clk_status);
    bs_bm = FIELD_DP16(bs_bm, BOOT_STATUS, OTP_DONE, bs.otp_done);
    bs_bm = FIELD_DP16(bs_bm, BOOT_STATUS, LC_DONE, bs.lc_done);
    bs_bm =
        FIELD_DP16(bs_bm, BOOT_STATUS, ROM_CTRL_DONE, bs.rom_done & ROM_MASK);
    bs_bm =
        FIELD_DP16(bs_bm, BOOT_STATUS, ROM_CTRL_DONE, bs.rom_good & ROM_MASK);
    bs_bm = FIELD_DP16(bs_bm, BOOT_STATUS, CPU_FETCH_EN, bs.cpu_fetch_en);
    s->boot_status_bm = bs_bm;

    SCHEDULE_FSM(s);
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
    /* note: interrupt usage is not specified */
    case R_CORE_DEBUG_POLICY_CTRL:
    case R_CORE_DEBUG_POLICY_VALID:
    case R_CORE_STATUS_MBX:
        val32 = s->regs[reg];
        break;
    case R_CORE_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(CORE, reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
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
        val32 &= CORE_ALERT_TEST_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, 1);
        }
        break;
    case R_CORE_DEBUG_POLICY_CTRL:
        val32 &= R_CORE_DEBUG_POLICY_CTRL_LEVEL_MASK;
        s->regs[reg] = val32;
        break;
    case R_CORE_DEBUG_POLICY_VALID:
        val32 &= R_CORE_DEBUG_POLICY_VALID_VALID_MASK;
        s->regs[reg] = val32;
        break;
    case R_CORE_STATUS_MBX:
        val32 &= STATUS_MASK;
        s->regs[reg] = val32;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
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
    case R_JTAG_CONTROL:
        val32 = s->boot_continue ? R_JTAG_CONTROL_BOOT_CONTINUE_MASK : 0u;
        break;
    case R_JTAG_STATUS:
        val32 = s->regs[R_CORE_STATUS_MBX]; /* mirror of the core I/F */
        break;
    case R_JTAG_BOOT_STATUS:
        if (s->lc_broadcast_bm & (1u << OT_LC_DFT_EN)) {
            val32 = (uint32_t)s->boot_status_bm;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: BootStatus disabled (no DFT)\n", __func__,
                          s->ot_id);
            val32 = 0;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
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
    case R_JTAG_STATUS:
    case R_JTAG_BOOT_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(JTAG, reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s, Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
    }
}

static Property ot_soc_dbg_ctrl_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtSoCDbgCtrlState, ot_id),
    DEFINE_PROP_BOOL("halt_function", OtSoCDbgCtrlState, halt_function, true),
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

    memset(s->regs, 0, sizeof(s->regs));

    ibex_irq_set(&s->alert, 0);
    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_GOOD], (int)false);
    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_DONE], (int)false);

    CHANGE_STATE(s, IDLE);
    s->fsm_tick_count = 0u;
    s->soc_dbg_bm = 0u;
    s->boot_status_bm = 0u;
    s->lc_broadcast_bm = 0u;
    s->soc_dbg_state = OT_SOC_DBG_ST_PROD;
    s->debug_policy = DEFAULT_DBG_LOCKED;
    s->debug_valid = false;
    s->boot_continue = false;
}

static void ot_soc_dbg_ctrl_reset_exit(Object *obj, ResetType type)
{
    OtSoCDbgCtrlClass *c = OT_SOC_DBG_CTRL_GET_CLASS(obj);
    OtSoCDbgCtrlState *s = OT_SOC_DBG_CTRL(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    qemu_bh_cancel(s->fsm_tick_bh);

    /*
     * ROM signal which does not comes from a ROM but from this device to
     * signal the status of the Ibex core, but used as a ROM in PwrMgr:
     * always on....
     */
    s->boot_status_bm |= ROM_MASK << R_BOOT_STATUS_ROM_CTRL_GOOD_SHIFT;

    ibex_irq_set(&s->continue_cpu_boot[CONTINUE_CPU_BOOT_GOOD], (int)true);

    SCHEDULE_FSM(s);
}

static void ot_soc_dbg_ctrl_realize(DeviceState *dev, Error **errp)
{
    OtSoCDbgCtrlState *s = OT_SOC_DBG_CTRL(dev);
    (void)errp;

    g_assert(s->ot_id);
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

    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
    ibex_qdev_init_irq(obj, &s->policy, OT_SOC_DBG_DEBUG_POLICY);
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
