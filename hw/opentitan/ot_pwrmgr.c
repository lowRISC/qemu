/*
 * QEMU OpenTitan Power Manager device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
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
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/runstate.h"
#include "trace.h"

#define PARAM_NUM_RST_REQS       2u
#define PARAM_NUM_INT_RST_REQS   2u
#define PARAM_NUM_DEBUG_RST_REQS 1u
#define PARAM_RESET_MAIN_PWR_IDX 2u
#define PARAM_RESET_ESC_IDX      3u
#define PARAM_RESET_NDM_IDX      4u
#define PARAM_NUM_ALERTS         1u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(WAKEUP, 0u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CTRL_CFG_REGWEN, 0x10u)
    FIELD(CTRL_CFG_REGWEN, EN, 0u, 1u)
REG32(CONTROL, 0x14u)
    FIELD(CONTROL, LOW_POWER_HINT, 0u, 1u)
    FIELD(CONTROL, CORE_CLK_EN, 4u, 1u)
    FIELD(CONTROL, IO_CLK_EN, 5u, 1u)
    FIELD(CONTROL, USB_CLK_EN_LP, 6u, 1u)
    FIELD(CONTROL, USB_CLK_EN_ACTIVE, 7u, 1u)
    FIELD(CONTROL, MAIN_PD_N, 8u, 1u)
REG32(CFG_CDC_SYNC, 0x18u)
    FIELD(CFG_CDC_SYNC, SYNC, 0u, 1u)
REG32(WAKEUP_EN_REGWEN, 0x1cu)
    FIELD(WAKEUP_EN_REGWEN, EN, 0u, 1u)
REG32(WAKEUP_EN, 0x20u)
    SHARED_FIELD(WAKEUP_CHANNEL_0, 0u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_1, 1u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_2, 2u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_3, 3u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_4, 4u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_5, 5u, 1u)
REG32(WAKE_STATUS, 0x24u)
REG32(RESET_EN_REGWEN, 0x28u)
    FIELD(RESET_EN_REGWEN, EN, 0u, 1u)
REG32(RESET_EN, 0x2cu)
REG32(RESET_STATUS, 0x30u)
REG32(ESCALATE_RESET_STATUS, 0x34u)
    FIELD(ESCALATE_RESET_STATUS, VAL, 0u, 1u)
REG32(WAKE_INFO_CAPTURE_DIS, 0x38u)
    FIELD(WAKE_INFO_CAPTURE_DIS, VAL, 0u, 1u)
REG32(WAKE_INFO, 0x3cu)
    FIELD(WAKE_INFO, REASONS, 0u, 5u)
    FIELD(WAKE_INFO, FALL_THROUGH, 6u, 1u)
    FIELD(WAKE_INFO, ABORT, 7u, 1u)
REG32(FAULT_STATUS, 0x40u)
    FIELD(FAULT_STATUS, REG_INTG_ERR, 0u, 1u)
    FIELD(FAULT_STATUS, ESC_TIMEOUT, 1u, 1u)
    FIELD(FAULT_STATUS, MAIN_PD_GLITCH, 2u, 1u)
/* clang-format on */

#define CONTROL_MASK \
    (R_CONTROL_LOW_POWER_HINT_MASK | R_CONTROL_CORE_CLK_EN_MASK | \
     R_CONTROL_IO_CLK_EN_MASK | R_CONTROL_USB_CLK_EN_LP_MASK | \
     R_CONTROL_USB_CLK_EN_ACTIVE_MASK | R_CONTROL_MAIN_PD_N_MASK)
#define WAKEUP_MASK \
    (WAKEUP_CHANNEL_0_MASK | WAKEUP_CHANNEL_1_MASK | WAKEUP_CHANNEL_2_MASK | \
     WAKEUP_CHANNEL_3_MASK | WAKEUP_CHANNEL_4_MASK | WAKEUP_CHANNEL_5_MASK)
#define WAKE_INFO_MASK \
    (R_WAKE_INFO_REASONS_MASK | R_WAKE_INFO_FALL_THROUGH_MASK | \
     R_WAKE_INFO_ABORT_MASK)

#define CDC_SYNC_PULSE_DURATION_NS 100000u /* 100us */

#define PWRMGR_WAKEUP_MAX 6u

/* special exit error code to report escalation panic */
#define EXIT_ESCALATION_PANIC 39

/* Verbatim definitions from RTL */
#define NUM_SW_RST_REQ 1u
#define HW_RESET_WIDTH \
    (PARAM_NUM_RST_REQS + PARAM_NUM_INT_RST_REQS + PARAM_NUM_DEBUG_RST_REQS)
#define RESET_SW_REQ_IDX (HW_RESET_WIDTH)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_FAULT_STATUS)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CTRL_CFG_REGWEN),
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(CFG_CDC_SYNC),
    REG_NAME_ENTRY(WAKEUP_EN_REGWEN),
    REG_NAME_ENTRY(WAKEUP_EN),
    REG_NAME_ENTRY(WAKE_STATUS),
    REG_NAME_ENTRY(RESET_EN_REGWEN),
    REG_NAME_ENTRY(RESET_EN),
    REG_NAME_ENTRY(RESET_STATUS),
    REG_NAME_ENTRY(ESCALATE_RESET_STATUS),
    REG_NAME_ENTRY(WAKE_INFO_CAPTURE_DIS),
    REG_NAME_ENTRY(WAKE_INFO),
    REG_NAME_ENTRY(FAULT_STATUS),
};
#undef REG_NAME_ENTRY

typedef enum {
    OT_PWRMGR_INIT_OTP,
    OT_PWRMGR_INIT_LC_CTRL,
    OT_PWRMGR_INIT_COUNT,
} OtPwrMgrInit;

typedef enum {
    OT_PWR_FAST_ST_LOW_POWER,
    OT_PWR_FAST_ST_ENABLE_CLOCKS,
    OT_PWR_FAST_ST_RELEASE_LC_RST,
    OT_PWR_FAST_ST_OTP_INIT,
    OT_PWR_FAST_ST_LC_INIT,
    OT_PWR_FAST_ST_STRAP,
    OT_PWR_FAST_ST_ACK_PWR_UP,
    OT_PWR_FAST_ST_ROM_CHECK_DONE,
    OT_PWR_FAST_ST_ROM_CHECK_GOOD,
    OT_PWR_FAST_ST_ACTIVE,
    OT_PWR_FAST_ST_DIS_CLKS,
    OT_PWR_FAST_ST_FALL_THROUGH,
    OT_PWR_FAST_ST_NVM_IDLE_CHK,
    OT_PWR_FAST_ST_LOW_POWER_PREP,
    OT_PWR_FAST_ST_NVM_SHUT_DOWN, /* not used in DJ */
    OT_PWR_FAST_ST_RESET_PREP,
    OT_PWR_FAST_ST_RESET_WAIT,
    OT_PWR_FAST_ST_REQ_PWR_DN,
    OT_PWR_FAST_ST_INVALID,
} OtPwrMgrFastState;

typedef enum {
    OT_PWR_SLOW_ST_RESET,
    OT_PWR_SLOW_ST_LOW_POWER,
    OT_PWR_SLOW_ST_MAIN_POWER_ON,
    OT_PWR_SLOW_ST_PWR_CLAMP_OFF,
    OT_PWR_SLOW_ST_CLOCKS_ON,
    OT_PWR_SLOW_ST_REQ_PWR_UP,
    OT_PWR_SLOW_ST_IDLE,
    OT_PWR_SLOW_ST_ACK_PWR_DN,
    OT_PWR_SLOW_ST_CLOCKS_OFF,
    OT_PWR_SLOW_ST_PWR_CLAMP_ON,
    OT_PWR_SLOW_ST_MAIN_POWER_OFF,
    OT_PWR_SLOW_ST_INVALID,
} OtPwrMgrSlowState;

typedef struct {
    bool good;
    bool done;
} OtPwrMgrRomStatus;

typedef enum {
    OT_PWRMGR_NO_DOMAIN,
    OT_PWRMGR_SLOW_DOMAIN,
    OT_PWRMGR_FAST_DOMAIN,
} OtPwrMgrClockDomain;

typedef struct {
    OtPwrMgrClockDomain domain;
    int req;
} OtPwrMgrResetReq;

typedef union {
    uint32_t bitmap;
    struct {
        uint8_t hw_reset : 1; /* HW reset request */
        uint8_t sw_reset : 1; /* SW reset request */
        uint8_t otp_done : 1;
        uint8_t lc_done : 1;
        uint8_t escalate : 1; /* escalation from alert handler */
        uint8_t holdon_fetch : 1; /* custom extension */
        uint8_t rom_good; /* up to 8 ROMs */
        uint8_t rom_done; /* up to 8 ROMs */
    };
} OtPwrMgrEvents;

static_assert(sizeof(OtPwrMgrEvents) == sizeof(uint32_t),
              "Invalid OtPwrMgrEvents definition");

struct OtPwrMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QEMUTimer *cdc_sync;
    QEMUBH *fsm_tick_bh;
    IbexIRQ irq; /* wake from low power */
    IbexIRQ alert;
    IbexIRQ strap;
    IbexIRQ cpu_enable;
    IbexIRQ pwr_lc_req;
    IbexIRQ pwr_otp_req;

    OtPwrMgrFastState f_state;
    OtPwrMgrSlowState s_state;
    OtPwrMgrEvents fsm_events;

    uint32_t *regs;
    OtPwrMgrResetReq reset_req;

    char *ot_id;
    OtRstMgrState *rstmgr;
    uint8_t num_rom;
    uint8_t version;
    bool main; /* main power manager (for machines w/ multiple PwrMgr) */
    bool fetch_ctrl;
};

struct OtPwrMgrClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#define PWRMGR_NAME_ENTRY(_pre_, _name_) [_pre_##_##_name_] = stringify(_name_)

#define FAST_ST_NAME_ENTRY(_name_) PWRMGR_NAME_ENTRY(OT_PWR_FAST_ST, _name_)
/* clang-format off */
static const char *FAST_ST_NAMES[] = {
    FAST_ST_NAME_ENTRY(LOW_POWER),
    FAST_ST_NAME_ENTRY(ENABLE_CLOCKS),
    FAST_ST_NAME_ENTRY(RELEASE_LC_RST),
    FAST_ST_NAME_ENTRY(OTP_INIT),
    FAST_ST_NAME_ENTRY(LC_INIT),
    FAST_ST_NAME_ENTRY(STRAP),
    FAST_ST_NAME_ENTRY(ACK_PWR_UP),
    FAST_ST_NAME_ENTRY(ROM_CHECK_DONE),
    FAST_ST_NAME_ENTRY(ROM_CHECK_GOOD),
    FAST_ST_NAME_ENTRY(ACTIVE),
    FAST_ST_NAME_ENTRY(DIS_CLKS),
    FAST_ST_NAME_ENTRY(FALL_THROUGH),
    FAST_ST_NAME_ENTRY(NVM_IDLE_CHK),
    FAST_ST_NAME_ENTRY(LOW_POWER_PREP),
    FAST_ST_NAME_ENTRY(NVM_SHUT_DOWN),
    FAST_ST_NAME_ENTRY(RESET_PREP),
    FAST_ST_NAME_ENTRY(RESET_WAIT),
    FAST_ST_NAME_ENTRY(REQ_PWR_DN),
    FAST_ST_NAME_ENTRY(INVALID),
};
/* clang-format on */
#undef FAST_ST_NAME_ENTRY
#define FST_NAME(_st_) \
    ((_st_) < ARRAY_SIZE(FAST_ST_NAMES) ? FAST_ST_NAMES[(_st_)] : "?")

#define SLOW_ST_NAME_ENTRY(_name_) PWRMGR_NAME_ENTRY(OT_PWR_SLOW_ST, _name_)
/* clang-format off */
static const char *SLOW_ST_NAMES[] = {
    SLOW_ST_NAME_ENTRY(RESET),
    SLOW_ST_NAME_ENTRY(LOW_POWER),
    SLOW_ST_NAME_ENTRY(MAIN_POWER_ON),
    SLOW_ST_NAME_ENTRY(PWR_CLAMP_OFF),
    SLOW_ST_NAME_ENTRY(CLOCKS_ON),
    SLOW_ST_NAME_ENTRY(REQ_PWR_UP),
    SLOW_ST_NAME_ENTRY(IDLE),
    SLOW_ST_NAME_ENTRY(ACK_PWR_DN),
    SLOW_ST_NAME_ENTRY(CLOCKS_OFF),
    SLOW_ST_NAME_ENTRY(PWR_CLAMP_ON),
    SLOW_ST_NAME_ENTRY(MAIN_POWER_OFF),
    SLOW_ST_NAME_ENTRY(INVALID),
};
/* clang-format on */
#undef SLOW_ST_NAME_ENTRY
#define SST_NAME(_st_) \
    ((_st_) < ARRAY_SIZE(SLOW_ST_NAMES) ? SLOW_ST_NAMES[(_st_)] : "?")

typedef struct {
    unsigned wakeup_count;
    unsigned reset_count;
    uint32_t reset_mask;
} OtPwrMgrConfig;

/* clang-format off */
static const OtPwrMgrConfig PWRMGR_CONFIG[OT_PWMGR_VERSION_COUNT] = {
    [OT_PWMGR_VERSION_EG] = {
        .wakeup_count = 6u,
        .reset_count = 2u,
        .reset_mask = 0x3u
    },
    [OT_PWMGR_VERSION_DJ] = {
        .wakeup_count = 6u,
        .reset_count = 2u,
        .reset_mask = 0x3u
    },
};

static int PWRMGR_RESET_DISPATCH[OT_PWMGR_VERSION_COUNT][PARAM_NUM_RST_REQS] = {
    [OT_PWMGR_VERSION_EG] = {
        [0] = OT_RSTMGR_RESET_SYSCTRL,
        [1] = OT_RSTMGR_RESET_AON_TIMER,
    },
    [OT_PWMGR_VERSION_DJ] = {
        [0] = OT_RSTMGR_RESET_AON_TIMER,
        [1] = OT_RSTMGR_RESET_SOC_PROXY,
    },
};

static const char *
PWRMGR_WAKEUP_NAMES[OT_PWMGR_VERSION_COUNT][PWRMGR_WAKEUP_MAX] = {
    [OT_PWMGR_VERSION_EG] = {
        [0] = "SYSRST",
        [1] = "ADC_CTRL",
        [2] = "PINMUX",
        [3] = "USBDEV",
        [4] = "AON_TIMER",
        [5] = "SENSOR",
    },
    [OT_PWMGR_VERSION_DJ] = {
        [0] = "PINMUX",
        [1] = "USBDEV",
        [2] = "AON_TIMER",
        [3] = "SENSOR",
        [4] = "SOC_PROXY_INT",
        [5] = "SOC_PROXY_EXT",
    },
};

static const char *PWRMGR_RST_NAMES[OT_PWMGR_VERSION_COUNT][PARAM_NUM_RST_REQS] = {
    [OT_PWMGR_VERSION_EG] = {
        [0] = "SYSRST",
        [1] = "AON_TIMER",
    },
    [OT_PWMGR_VERSION_DJ] = {
        [0] = "AON_TIMER",
        [1] = "SOC_PROXY",
    }
};
/* clang-format on */

#define WAKEUP_NAME(_s_, _w_) \
    ((_w_) < PWRMGR_CONFIG[(_s_)->version].wakeup_count ? \
         PWRMGR_WAKEUP_NAMES[(_s_)->version][(_w_)] : \
         "?")
#define RST_NAME(_s_, _r_) \
    ((_r_) < PWRMGR_CONFIG[(_s_)->version].reset_count ? \
         PWRMGR_RST_NAMES[(_s_)->version][(_r_)] : \
         "?")

#undef PWRMGR_NAME_ENTRY

#define PWR_CHANGE_FAST_STATE(_s_, _st_) \
    ot_pwrmgr_change_fast_state_line(_s_, OT_PWR_FAST_ST_##_st_, __LINE__)

#define PWR_CHANGE_SLOW_STATE(_s_, _st_) \
    ot_pwrmgr_change_slow_state_line(_s_, OT_PWR_SLOW_ST_##_st_, __LINE__)

static void ot_pwrmgr_change_fast_state_line(OtPwrMgrState *s,
                                             OtPwrMgrFastState state, int line)
{
    trace_ot_pwrmgr_change_state(s->ot_id, line, "fast", FST_NAME(s->f_state),
                                 s->f_state, FST_NAME(state), state);

    s->f_state = state;
}

static void ot_pwrmgr_change_slow_state_line(OtPwrMgrState *s,
                                             OtPwrMgrSlowState state, int line)
{
    trace_ot_pwrmgr_change_state(s->ot_id, line, "slow", SST_NAME(s->s_state),
                                 s->s_state, SST_NAME(state), state);

    s->s_state = state;
}

static void ot_pwrmgr_update_irq(OtPwrMgrState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    ibex_irq_set(&s->irq, (int)(bool)level);
}

#define ot_pwrmgr_schedule_fsm(_s_) \
    ot_pwrmgr_xschedule_fsm(_s_, __func__, __LINE__)

static void ot_pwrmgr_xschedule_fsm(OtPwrMgrState *s, const char *func,
                                    int line)
{
    trace_ot_pwrmgr_schedule_fsm(s->ot_id, func, line);
    qemu_bh_schedule(s->fsm_tick_bh);
}

static void ot_pwrmgr_cdc_sync(void *opaque)
{
    OtPwrMgrState *s = opaque;

    s->regs[R_CFG_CDC_SYNC] &= ~R_CFG_CDC_SYNC_SYNC_MASK;
}

static void ot_pwrmgr_rom_good(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert((unsigned)n < s->num_rom);

    trace_ot_pwrmgr_rom(s->ot_id, n, "good", level);

    if (level) {
        s->fsm_events.rom_good |= 1u << n;
        ot_pwrmgr_schedule_fsm(s);
    }
}

static void ot_pwrmgr_rom_done(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert((unsigned)n < s->num_rom);

    trace_ot_pwrmgr_rom(s->ot_id, n, "done", level);

    if (level) {
        s->fsm_events.rom_done |= 1u << n;
        ot_pwrmgr_schedule_fsm(s);
    }
}

static void ot_pwrmgr_wkup(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;
    unsigned src = (unsigned)irq;

    assert(src < PWRMGR_WAKEUP_MAX);

    trace_ot_pwrmgr_wkup(s->ot_id, WAKEUP_NAME(s, src), src, (bool)level);
}

static void ot_pwrmgr_rst_req(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    unsigned src = (unsigned)irq;
    g_assert(src < PWRMGR_CONFIG[s->version].reset_count);

    uint32_t rstbit = 1u << src; /* rst_req are stored in the LSBs */

    if (level) {
        trace_ot_pwrmgr_rst_req(s->ot_id, RST_NAME(s, src), src);
        uint32_t rstmask = PWRMGR_CONFIG[s->version].reset_mask;

        /* if HW reset is maskable and not HW reset is not enabled */
        if ((rstbit & rstmask) && !(s->regs[R_RESET_EN] & rstbit)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: HW reset #%u not enabled 0x%08x 0x%08x\n",
                          __func__, src, s->regs[R_RESET_EN], rstbit);
            return;
        }

        if (s->regs[R_RESET_STATUS]) {
            /* do nothing if a reset is already in progress */
            /* TODO: is it true for HW vs. SW request ?*/
            trace_ot_pwrmgr_ignore_req("reset on-going");
            return;
        }
        s->regs[R_RESET_STATUS] |= rstbit;

        g_assert(s->reset_req.domain == OT_PWRMGR_NO_DOMAIN);

        s->reset_req.domain = OT_PWRMGR_SLOW_DOMAIN;

        int req = PWRMGR_RESET_DISPATCH[s->version][src];
        if (req < 0) {
            /* not yet implemented */
            g_assert_not_reached();
        }
        s->reset_req.req = req;

        trace_ot_pwrmgr_reset_req(s->ot_id, "scheduling reset", src);

        s->fsm_events.hw_reset = true;
        ot_pwrmgr_schedule_fsm(s);
    }
}

static void ot_pwrmgr_sw_rst_req(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    unsigned src = (unsigned)irq;
    g_assert(src < NUM_SW_RST_REQ);

    uint32_t rstbit = 1u << (RESET_SW_REQ_IDX + src);

    if (level) {
        trace_ot_pwrmgr_rst_req(s->ot_id, "SW", src);

        if (s->regs[R_RESET_STATUS]) {
            /* do nothing if a reset is already in progress */
            trace_ot_pwrmgr_ignore_req("reset on-going");
            return;
        }

        s->regs[R_RESET_STATUS] |= rstbit;

        g_assert(s->reset_req.domain == OT_PWRMGR_NO_DOMAIN);

        s->reset_req.req = OT_RSTMGR_RESET_SW;
        s->reset_req.domain = OT_PWRMGR_FAST_DOMAIN;

        trace_ot_pwrmgr_reset_req(s->ot_id, "scheduling SW reset", 0);

        s->fsm_events.sw_reset = true;
        ot_pwrmgr_schedule_fsm(s);
    }
}


static void ot_pwrmgr_slow_fsm_tick(OtPwrMgrState *s)
{
    /* fast forward to IDLE slow FSM state for now */
    if (s->s_state == OT_PWR_SLOW_ST_RESET) {
        PWR_CHANGE_SLOW_STATE(s, IDLE);
    }
}

static void ot_pwrmgr_fast_fsm_tick(OtPwrMgrState *s)
{
    if (s->s_state != OT_PWR_SLOW_ST_IDLE) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: slow FSM not supported\n", __func__,
                      s->ot_id);
        return;
    }

    if (s->fsm_events.escalate) {
        PWR_CHANGE_FAST_STATE(s, REQ_PWR_DN);
        trace_ot_pwrmgr_shutdown(s->ot_id, s->main);
        if (s->main) {
            qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,
                                                   EXIT_ESCALATION_PANIC);
        }
    }

    switch (s->f_state) {
    case OT_PWR_FAST_ST_LOW_POWER:
        PWR_CHANGE_FAST_STATE(s, ENABLE_CLOCKS);
        break;
    case OT_PWR_FAST_ST_ENABLE_CLOCKS:
        PWR_CHANGE_FAST_STATE(s, RELEASE_LC_RST);
        // TODO: need to release ROM controllers from reset here to emulate
        // they are clocked and start to verify their contents.
        break;
    case OT_PWR_FAST_ST_RELEASE_LC_RST:
        PWR_CHANGE_FAST_STATE(s, OTP_INIT);
        ibex_irq_set(&s->pwr_otp_req, (int)true);
        break;
    case OT_PWR_FAST_ST_OTP_INIT:
        if (s->fsm_events.otp_done) {
            /* release the request signal */
            ibex_irq_set(&s->pwr_otp_req, (int)false);
            PWR_CHANGE_FAST_STATE(s, LC_INIT);
            ibex_irq_set(&s->pwr_lc_req, (int)true);
        }
        break;
    case OT_PWR_FAST_ST_LC_INIT:
        if (s->fsm_events.lc_done) {
            /* release the request signal */
            ibex_irq_set(&s->pwr_lc_req, (int)false);
            PWR_CHANGE_FAST_STATE(s, ACK_PWR_UP);
        }
        break;
    case OT_PWR_FAST_ST_ACK_PWR_UP:
        PWR_CHANGE_FAST_STATE(s, STRAP);
        break;
    case OT_PWR_FAST_ST_STRAP:
        ibex_irq_set(&s->strap, (int)true);
        PWR_CHANGE_FAST_STATE(s, ROM_CHECK_DONE);
        break;
    case OT_PWR_FAST_ST_ROM_CHECK_DONE:
        ibex_irq_set(&s->strap, (int)false);
        if (s->fsm_events.rom_done == (1u << s->num_rom) - 1u) {
            PWR_CHANGE_FAST_STATE(s, ROM_CHECK_GOOD);
        }
        break;
    case OT_PWR_FAST_ST_ROM_CHECK_GOOD:
        if ((s->fsm_events.rom_good == (1u << s->num_rom) - 1u) &&
            !s->fsm_events.holdon_fetch) {
            PWR_CHANGE_FAST_STATE(s, ACTIVE);
        }
        break;
    case OT_PWR_FAST_ST_ACTIVE:
        if (!s->regs[R_RESET_STATUS]) {
            ibex_irq_set(&s->cpu_enable, (int)true);
        } else {
            ibex_irq_set(&s->cpu_enable, (int)false);
            PWR_CHANGE_FAST_STATE(s, DIS_CLKS);
        }
        break;
    case OT_PWR_FAST_ST_DIS_CLKS:
        PWR_CHANGE_FAST_STATE(s, RESET_PREP);
        break;
    case OT_PWR_FAST_ST_FALL_THROUGH:
    case OT_PWR_FAST_ST_NVM_IDLE_CHK:
    case OT_PWR_FAST_ST_LOW_POWER_PREP:
    case OT_PWR_FAST_ST_NVM_SHUT_DOWN:
        qemu_log_mask(LOG_UNIMP, "%s: low power modes are not implemented\n",
                      __func__);
        /* fallthrough */
    case OT_PWR_FAST_ST_RESET_PREP:
        PWR_CHANGE_FAST_STATE(s, RESET_WAIT);
        ot_rstmgr_reset_req(s->rstmgr, (bool)s->reset_req.domain,
                            s->reset_req.req);
        s->reset_req.domain = OT_PWRMGR_NO_DOMAIN;
        break;
    /* NOLINTNEXTLINE */
    case OT_PWR_FAST_ST_RESET_WAIT:
        /* wait here for QEMU to reset the Power Manager */
        break;
    case OT_PWR_FAST_ST_REQ_PWR_DN:
    case OT_PWR_FAST_ST_INVALID:
        break;
    }
}

static void ot_pwrmgr_fsm_tick(void *opaque)
{
    OtPwrMgrState *s = opaque;

    ot_pwrmgr_slow_fsm_tick(s);

    OtPwrMgrFastState f_state = s->f_state;
    ot_pwrmgr_fast_fsm_tick(s);
    if (f_state != s->f_state) {
        /* schedule FSM update once more if its state has changed */
        ot_pwrmgr_schedule_fsm(s);
    } else {
        /* otherwise, go idle and wait for an external event */
        trace_ot_pwrmgr_go_idle(s->ot_id, FST_NAME(s->f_state));
    }
}

/*
 * Input lines
 */

static void ot_pwrmgr_escalate_rx(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(n == 0);

    trace_ot_pwrmgr_escalate_rx(s->ot_id, (bool)level);

    if (level) {
        s->regs[R_ESCALATE_RESET_STATUS] |= R_ESCALATE_RESET_STATUS_VAL_MASK;
        s->fsm_events.escalate = true;
        ot_pwrmgr_schedule_fsm(s);
    }
}

static void ot_pwrmgr_pwr_lc_rsp(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(n == 0);

    if (level) {
        s->fsm_events.lc_done = true;
        ot_pwrmgr_schedule_fsm(s);
    }
}

static void ot_pwrmgr_pwr_otp_rsp(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(n == 0);

    if (level) {
        s->fsm_events.otp_done = true;
        ot_pwrmgr_schedule_fsm(s);
    }
}

static void ot_pwrmgr_holdon_fetch(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(n == 0);

    s->fsm_events.holdon_fetch = (bool)level;
    ot_pwrmgr_schedule_fsm(s);
}

static uint64_t ot_pwrmgr_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPwrMgrState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CTRL_CFG_REGWEN:
    case R_CONTROL:
    case R_CFG_CDC_SYNC:
    case R_WAKEUP_EN_REGWEN:
    case R_WAKEUP_EN:
    case R_WAKE_STATUS:
    case R_RESET_EN_REGWEN:
    case R_RESET_EN:
    case R_ESCALATE_RESET_STATUS:
    case R_WAKE_INFO_CAPTURE_DIS:
    case R_WAKE_INFO:
    case R_FAULT_STATUS:
        val32 = s->regs[reg];
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0;
        break;
    case R_RESET_STATUS:
        val32 = s->regs[reg] & PWRMGR_CONFIG[s->version].reset_mask;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pwrmgr_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                pc);

    return (uint64_t)val32;
};

static void ot_pwrmgr_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtPwrMgrState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pwrmgr_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                             pc);
    switch (reg) {
    case R_INTR_STATE:
        val32 &= WAKEUP_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_pwrmgr_update_irq(s);
        break;
    case R_INTR_ENABLE:
        val32 &= WAKEUP_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_pwrmgr_update_irq(s);
        break;
    case R_INTR_TEST:
        val32 &= WAKEUP_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_pwrmgr_update_irq(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        s->regs[reg] = val32;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case R_CONTROL:
        /* TODO: clear LOW_POWER_HINT on next WFI? */
        val32 &= CONTROL_MASK;
        s->regs[reg] = val32;
        break;
    case R_CFG_CDC_SYNC:
        val32 &= R_CFG_CDC_SYNC_SYNC_MASK;
        s->regs[reg] |= val32; /* not described as RW1S, but looks like it */
        if (val32) {
            timer_mod(s->cdc_sync, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) +
                                       CDC_SYNC_PULSE_DURATION_NS);
        }
        break;
    case R_WAKEUP_EN_REGWEN:
        val32 &= R_WAKEUP_EN_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_WAKEUP_EN:
        if (s->regs[R_WAKEUP_EN_REGWEN] & R_WAKEUP_EN_REGWEN_EN_MASK) {
            val32 &= WAKEUP_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                          __func__, s->ot_id, REG_NAME(reg));
        }
        break;
    case R_RESET_EN_REGWEN:
        val32 &= R_RESET_EN_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_RESET_EN:
        if (s->regs[R_RESET_EN_REGWEN] & R_RESET_EN_REGWEN_EN_MASK) {
            val32 &= PWRMGR_CONFIG[s->version].reset_mask;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                          __func__, s->ot_id, REG_NAME(reg));
        }
        break;
    case R_WAKE_INFO_CAPTURE_DIS:
        val32 &= R_WAKE_INFO_CAPTURE_DIS_VAL_MASK;
        s->regs[reg] = val32;
        break;
    case R_WAKE_INFO:
        val32 &= WAKE_INFO_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        break;
    case R_CTRL_CFG_REGWEN:
    case R_WAKE_STATUS:
    case R_RESET_STATUS:
    case R_ESCALATE_RESET_STATUS:
    case R_FAULT_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
};

static Property ot_pwrmgr_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtPwrMgrState, ot_id),
    DEFINE_PROP_UINT8("num-rom", OtPwrMgrState, num_rom, 0),
    DEFINE_PROP_UINT8("version", OtPwrMgrState, version, UINT8_MAX),
    DEFINE_PROP_BOOL("fetch-ctrl", OtPwrMgrState, fetch_ctrl, false),
    DEFINE_PROP_BOOL("main", OtPwrMgrState, main, true),
    DEFINE_PROP_LINK("rstmgr", OtPwrMgrState, rstmgr, TYPE_OT_RSTMGR,
                     OtRstMgrState *),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_pwrmgr_regs_ops = {
    .read = &ot_pwrmgr_regs_read,
    .write = &ot_pwrmgr_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_pwrmgr_reset_enter(Object *obj, ResetType type)
{
    OtPwrMgrClass *c = OT_PWRMGR_GET_CLASS(obj);
    OtPwrMgrState *s = OT_PWRMGR(obj);

    g_assert(s->version < OT_PWMGR_VERSION_COUNT);

    /* sanity checks for platform reset count and mask */
    g_assert(PWRMGR_CONFIG[s->version].reset_count <= PARAM_NUM_RST_REQS);
    g_assert(ctpop32(PWRMGR_CONFIG[s->version].reset_mask + 1u) == 1);
    g_assert(PWRMGR_CONFIG[s->version].reset_mask <
             (1u << PWRMGR_CONFIG[s->version].reset_count));

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }

    trace_ot_pwrmgr_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    assert(s->rstmgr);

    timer_del(s->cdc_sync);
    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_CTRL_CFG_REGWEN] = 0x1u;
    s->regs[R_CONTROL] = 0x180u;
    s->regs[R_WAKEUP_EN_REGWEN] = 0x1u;
    s->regs[R_RESET_EN_REGWEN] = 0x1u;
    s->fsm_events.bitmap = 0;
    s->fsm_events.holdon_fetch = s->fetch_ctrl;

    PWR_CHANGE_FAST_STATE(s, LOW_POWER);
    PWR_CHANGE_SLOW_STATE(s, RESET);

    ot_pwrmgr_update_irq(s);
    ibex_irq_set(&s->strap, 0);
    ibex_irq_set(&s->cpu_enable, 0);
    ibex_irq_set(&s->pwr_otp_req, 0);
    ibex_irq_set(&s->pwr_lc_req, 0);
    ibex_irq_set(&s->alert, 0);
}

static void ot_pwrmgr_reset_exit(Object *obj)
{
    OtPwrMgrClass *c = OT_PWRMGR_GET_CLASS(obj);
    OtPwrMgrState *s = OT_PWRMGR(obj);

    trace_ot_pwrmgr_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj);
    }

    ot_pwrmgr_schedule_fsm(s);
}

static void ot_pwrmgr_realize(DeviceState *dev, Error **errp)
{
    OtPwrMgrState *s = OT_PWRMGR(dev);
    (void)errp;

    if (s->num_rom) {
        if (s->num_rom > 8u * sizeof(uint8_t)) {
            error_setg(&error_fatal, "too many ROMs\n");
            g_assert_not_reached();
        }
        qdev_init_gpio_in_named(dev, &ot_pwrmgr_rom_good, OT_PWRMGR_ROM_GOOD,
                                s->num_rom);
        qdev_init_gpio_in_named(dev, &ot_pwrmgr_rom_done, OT_PWRMGR_ROM_DONE,
                                s->num_rom);
    }

    if (s->fetch_ctrl) {
        qdev_init_gpio_in_named(dev, &ot_pwrmgr_holdon_fetch,
                                OT_PWRMGR_HOLDON_FETCH, 1u);
    }
}

static void ot_pwrmgr_init(Object *obj)
{
    OtPwrMgrState *s = OT_PWRMGR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pwrmgr_regs_ops, s, TYPE_OT_PWRMGR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    ibex_sysbus_init_irq(obj, &s->irq);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
    ibex_qdev_init_irq(obj, &s->pwr_lc_req, OT_PWRMGR_LC_REQ);
    ibex_qdev_init_irq(obj, &s->pwr_otp_req, OT_PWRMGR_OTP_REQ);
    ibex_qdev_init_irq(obj, &s->cpu_enable, OT_PWRMGR_CPU_EN);
    ibex_qdev_init_irq(obj, &s->strap, OT_PWRMGR_STRAP);

    s->cdc_sync = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_pwrmgr_cdc_sync, s);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_wkup, OT_PWRMGR_WKUP,
                            PWRMGR_WAKEUP_MAX);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_rst_req, OT_PWRMGR_RST,
                            PARAM_NUM_RST_REQS);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_sw_rst_req,
                            OT_PWRMGR_SW_RST, NUM_SW_RST_REQ);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_pwr_lc_rsp,
                            OT_PWRMGR_LC_RSP, 1);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_pwr_otp_rsp,
                            OT_PWRMGR_OTP_RSP, 1);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_escalate_rx,
                            OT_ALERT_ESCALATE, 1);

    s->fsm_tick_bh = qemu_bh_new(&ot_pwrmgr_fsm_tick, s);
}

static void ot_pwrmgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_pwrmgr_realize;
    device_class_set_props(dc, ot_pwrmgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(dc);
    OtPwrMgrClass *pc = OT_PWRMGR_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_pwrmgr_reset_enter, NULL,
                                       &ot_pwrmgr_reset_exit,
                                       &pc->parent_phases);
}

static const TypeInfo ot_pwrmgr_info = {
    .name = TYPE_OT_PWRMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPwrMgrState),
    .instance_init = &ot_pwrmgr_init,
    .class_init = &ot_pwrmgr_class_init,
    .class_size = sizeof(OtPwrMgrClass)
};

static void ot_pwrmgr_register_types(void)
{
    type_register_static(&ot_pwrmgr_info);
}

type_init(ot_pwrmgr_register_types);
