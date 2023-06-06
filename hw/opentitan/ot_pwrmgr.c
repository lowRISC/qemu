/*
 * QEMU OpenTitan Power Manager device
 *
 * Copyright (c) 2023 Rivos, Inc.
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
#include "hw/core/cpu.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
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
    SHARED_FIELD(RESET_CHANNEL_0, 0u, 1u)
    SHARED_FIELD(RESET_CHANNEL_1, 1u, 1u)
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
#define RESET_MASK (RESET_CHANNEL_0_MASK | RESET_CHANNEL_1_MASK)
#define WAKE_INFO_MASK \
    (R_WAKE_INFO_REASONS_MASK | R_WAKE_INFO_FALL_THROUGH_MASK | \
     R_WAKE_INFO_ABORT_MASK)

#define CDC_SYNC_PULSE_DURATION_NS 100000u /* 100us */

/* Verbatim definitions from RTL */
#define NUM_SW_RST_REQ 1u
#define HW_RESET_WIDTH \
    (PARAM_NUM_RST_REQS + PARAM_NUM_INT_RST_REQS + PARAM_NUM_DEBUG_RST_REQS)
#define TOTAL_RESET_WIDTH (HW_RESET_WIDTH + NUM_SW_RST_REQ)
#define RESET_SW_REQ_IDX  (TOTAL_RESET_WIDTH - 1u)

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

typedef struct {
    bool good;
    bool done;
} OtPwrMgrRomStatus;

typedef struct {
    OtRstMgrResetReq req;
    bool domain;
} OtPwrMgrResetReq;

struct OtPwrMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QEMUTimer *cdc_sync;
    IbexIRQ irq; /* wake from low power */
    IbexIRQ alert;

    uint8_t num_rom;
    OtPwrMgrRomStatus *roms;

    QEMUBH *reset_bh;

    OtRstMgrState *rstmgr;

    uint32_t *regs;
    OtPwrMgrResetReq reset_req;
};

static const char *WAKEUP_NAMES[OT_PWRMGR_WAKEUP_COUNT] = {
    [OT_PWRMGR_WAKEUP_SYSRST] = "SYSRST",
    [OT_PWRMGR_WAKEUP_ADC_CTRL] = "ADC_CTRL",
    [OT_PWRMGR_WAKEUP_PINMUX] = "PINMUX",
    [OT_PWRMGR_WAKEUP_USBDEV] = "USBDEV",
    [OT_PWRMGR_WAKEUP_AON_TIMER] = "AON_TIMER",
    [OT_PWRMGR_WAKEUP_SENSOR] = "SENSOR",
};
#define WAKEUP_NAME(_clk_) \
    ((_clk_) < ARRAY_SIZE(WAKEUP_NAMES) ? WAKEUP_NAMES[(_clk_)] : "?")

static const char *RST_REQ_NAMES[OT_PWRMGR_RST_REQ_COUNT] = {
    [OT_PWRMGR_RST_REQ_SYSRST] = "SYSRST",
    [OT_PWRMGR_RST_REQ_AON_TIMER] = "AON_TIMER",
};
#define RST_REQ_NAME(_clk_) \
    ((_clk_) < ARRAY_SIZE(RST_REQ_NAMES) ? RST_REQ_NAMES[(_clk_)] : "?")

static void ot_pwrmgr_update_irq(OtPwrMgrState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    ibex_irq_set(&s->irq, level);
}

static void ot_pwrmgr_cdc_sync(void *opaque)
{
    OtPwrMgrState *s = opaque;

    s->regs[R_CFG_CDC_SYNC] &= ~R_CFG_CDC_SYNC_SYNC_MASK;
}

static void ot_pwrmgr_rom_good(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(irq < s->num_rom);

    s->roms[irq].good = level;
}

static void ot_pwrmgr_rom_done(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(irq < s->num_rom);

    s->roms[irq].done = level;

    trace_ot_pwrmgr_rom_done(irq, s->roms[irq].good, s->roms[irq].done);

    /* compute combined ROM check status */
    bool good = true;
    bool done = true;
    for (unsigned idx = 0; idx < s->num_rom; idx++) {
        good &= s->roms[idx].good;
        done &= s->roms[idx].done;
    }

    /* if all ROM checks are done, start vCPU or report error */
    if (done) {
        if (good) {
            CPUState *cpu = ot_common_get_local_cpu(DEVICE(s));
            if (cpu) {
                cpu->halted = 0;
                cpu_resume(cpu);
            } else {
                error_report("ot_pwrmgr: Could not find a vCPU to start!");
            }
        } else {
            warn_report(
                "ot_pwrmgr: ROM controller reports failed digest check, "
                "will not start vCPU");
        }
    }
}

static void ot_pwrmgr_wkup(void *opaque, int irq, int level)
{
    /* not implemented yet */
    unsigned src = (unsigned)irq;

    assert(src < OT_PWRMGR_WAKEUP_COUNT);

    trace_ot_pwrmgr_wkup(WAKEUP_NAME(src), src, (bool)level);
}

static void ot_pwrmgr_rst_req(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    unsigned src = (unsigned)irq;

    trace_ot_pwrmgr_rst_req(RST_REQ_NAME(src), src, (bool)level);

    switch (irq) {
    case OT_PWRMGR_RST_REQ_SYSRST:
        s->reset_req.req = OT_RSTMGR_RESET_SYSCTRL, s->reset_req.domain = false;
        break;
    case OT_PWRMGR_RST_REQ_AON_TIMER:
        s->reset_req.req = OT_RSTMGR_RESET_AON_TIMER,
        s->reset_req.domain = false;
        break;
    default:
        g_assert_not_reached();
        break;
    }

    uint32_t rstmask = 1u << src; /*rst_req are stored in the LSBs */
    s->regs[R_RESET_STATUS] |= rstmask;

    /*
     * for now, there is no FSM in PWRMGR implementation.
     * simply forward the request to the RSTMGR
     */
    qemu_bh_schedule(s->reset_bh);
    trace_ot_pwrmgr_reset_req("scheduling reset", src);
}

static void ot_pwrmgr_sw_rst_req(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    unsigned src = (unsigned)irq;
    assert(src < NUM_SW_RST_REQ);

    trace_ot_pwrmgr_sw_rst_req(src, (bool)level);
    if (!level) {
        return;
    }

    uint32_t rstmask = 1u << (NUM_SW_RST_REQ + src);
    if (!(s->regs[R_RESET_EN] & rstmask)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: SW reset %u not enabled\n",
                      __func__, src);
        return;
    }

    s->regs[R_RESET_STATUS] |= rstmask;

    /*
     * for now, there is no FSM in PWRMGR implementation.
     * simply forward the request to the RSTMGR
     */
    s->reset_req.req = OT_RSTMGR_RESET_SW;
    s->reset_req.domain = true;
    qemu_bh_schedule(s->reset_bh);
    trace_ot_pwrmgr_reset_req("scheduling SW reset", 0);
}

static void ot_pwrmgr_trigger_reset(void *opaque)
{
    OtPwrMgrState *s = opaque;

    ot_rstmgr_reset_req(s->rstmgr, s->reset_req.domain, s->reset_req.req);
}

static uint64_t ot_pwrmgr_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPwrMgrState *s = opaque;
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
    case R_RESET_STATUS:
    case R_ESCALATE_RESET_STATUS:
    case R_WAKE_INFO_CAPTURE_DIS:
    case R_WAKE_INFO:
    case R_FAULT_STATUS:
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
    trace_ot_pwrmgr_io_read_out((unsigned)addr, REG_NAME(reg), (uint64_t)val32,
                                pc);

    return (uint64_t)val32;
};

static void ot_pwrmgr_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtPwrMgrState *s = opaque;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_pwrmgr_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

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
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case R_CONTROL:
        /* TODO: clear LOW_POWER_HINT on next WFI? */
        val32 &= CONTROL_MASK;
        s->regs[reg] = val32;
        break;
    case R_CFG_CDC_SYNC:
        val32 &= R_CFG_CDC_SYNC_SYNC_MASK;
        timer_mod(s->cdc_sync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                   CDC_SYNC_PULSE_DURATION_NS);
        break;
    case R_WAKEUP_EN_REGWEN:
        val32 &= R_WAKEUP_EN_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_WAKEUP_EN:
        if (s->regs[R_WAKEUP_EN_REGWEN] & R_WAKEUP_EN_REGWEN_EN_MASK) {
            val32 &= WAKEUP_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_RESET_EN_REGWEN:
        val32 &= R_RESET_EN_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_RESET_EN:
        if (s->regs[R_RESET_EN_REGWEN] & R_RESET_EN_REGWEN_EN_MASK) {
            val32 &= RESET_MASK;
            s->regs[reg] = val32;
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
                      "R/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_pwrmgr_properties[] = {
    DEFINE_PROP_UINT8("num-rom", OtPwrMgrState, num_rom, 0),
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

static void ot_pwrmgr_reset(DeviceState *dev)
{
    OtPwrMgrState *s = OT_PWRMGR(dev);

    assert(s->rstmgr);

    timer_del(s->cdc_sync);
    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_CTRL_CFG_REGWEN] = 0x1u;
    s->regs[R_CONTROL] = 0x180u;
    s->regs[R_WAKEUP_EN_REGWEN] = 0x1u;
    s->regs[R_RESET_EN_REGWEN] = 0x1u;

    ot_pwrmgr_update_irq(s);
    ibex_irq_set(&s->alert, 0);

    memset(s->roms, 0, s->num_rom * sizeof(OtPwrMgrRomStatus));
}

static void ot_pwrmgr_realize(DeviceState *dev, Error **errp)
{
    OtPwrMgrState *s = OT_PWRMGR(dev);

    g_assert(s->num_rom);

    s->roms = g_new0(OtPwrMgrRomStatus, s->num_rom);

    qdev_init_gpio_in_named(dev, &ot_pwrmgr_rom_good, OPENTITAN_PWRMGR_ROM_GOOD,
                            s->num_rom);
    qdev_init_gpio_in_named(dev, &ot_pwrmgr_rom_done, OPENTITAN_PWRMGR_ROM_DONE,
                            s->num_rom);
}

static void ot_pwrmgr_init(Object *obj)
{
    OtPwrMgrState *s = OT_PWRMGR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pwrmgr_regs_ops, s, TYPE_OT_PWRMGR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    ibex_sysbus_init_irq(obj, &s->irq);
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
    s->cdc_sync = timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_pwrmgr_cdc_sync, s);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_wkup,
                            OPENTITAN_PWRMGR_WKUP_REQ, OT_PWRMGR_WAKEUP_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_rst_req,
                            OPENTITAN_PWRMGR_RST_REQ, OT_PWRMGR_RST_REQ_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_sw_rst_req,
                            OPENTITAN_PWRMGR_SW_RST_REQ, NUM_SW_RST_REQ);

    s->reset_bh = qemu_bh_new(&ot_pwrmgr_trigger_reset, s);
}

static void ot_pwrmgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = &ot_pwrmgr_realize;
    dc->reset = &ot_pwrmgr_reset;
    device_class_set_props(dc, ot_pwrmgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_pwrmgr_info = {
    .name = TYPE_OT_PWRMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPwrMgrState),
    .instance_init = &ot_pwrmgr_init,
    .class_init = &ot_pwrmgr_class_init,
};

static void ot_pwrmgr_register_types(void)
{
    type_register_static(&ot_pwrmgr_info);
}

type_init(ot_pwrmgr_register_types)
