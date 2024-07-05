/*
 * QEMU OpenTitan Reset Manager device
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
 * Note: for now, only a minimalist subset of Power Manager device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include <assert.h>
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/hw_accel.h"
#include "sysemu/runstate.h"
#include "trace.h"


#define PARAM_RD_WIDTH         32u
#define PARAM_IDX_WIDTH        4u
#define PARAM_NUM_HW_RESETS    5u
#define PARAM_NUM_SW_RESETS    8u
#define PARAM_NUM_TOTAL_RESETS 8u
#define PARAM_NUM_ALERTS       2u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_CNSTY_FAULT, 1u, 1u)
REG32(RESET_REQ, 0x4u)
    FIELD(RESET_REQ, VAL, 0u, 4u)
REG32(RESET_INFO, 0x8u)
    FIELD(RESET_INFO, POR, 0u, 1u)
    FIELD(RESET_INFO, LOW_POWER_EXIT, 1u, 1u)
    FIELD(RESET_INFO, SW_RESET, 2u, 1u)
    FIELD(RESET_INFO, HW_REQ, 3u, 5u)
REG32(ALERT_REGWEN, 0xcu)
    FIELD(ALERT_REGWEN, EN, 0u, 1u)
REG32(ALERT_INFO_CTRL, 0x10u)
    FIELD(ALERT_INFO_CTRL, EN, 0u, 1u)
    FIELD(ALERT_INFO_CTRL, INDEX, 4u, 4u)
REG32(ALERT_INFO_ATTR, 0x14u)
    FIELD(ALERT_INFO_ATTR, CNT_AVAIL, 0u, 4u)
REG32(ALERT_INFO, 0x18u)
REG32(CPU_REGWEN, 0x1cu)
    FIELD(CPU_REGWEN, EN, 0u, 1u)
REG32(CPU_INFO_CTRL, 0x20u)
    FIELD(CPU_INFO_CTRL, EN, 0u, 1u)
    FIELD(CPU_INFO_CTRL, INDEX, 0u, 4u)
REG32(CPU_INFO_ATTR, 0x24u)
    FIELD(CPU_INFO_ATTR, CNT_AVAIL, 0u, 4u)
REG32(CPU_INFO, 0x28u)
REG32(SW_RST_REGWEN_0, 0x2cu)
    SHARED_FIELD(SW_RST_REGWEN_EN, 0u, 1u)
REG32(SW_RST_REGWEN_1, 0x30u)
REG32(SW_RST_REGWEN_2, 0x34u)
REG32(SW_RST_REGWEN_3, 0x38u)
REG32(SW_RST_REGWEN_4, 0x3cu)
REG32(SW_RST_REGWEN_5, 0x40u)
REG32(SW_RST_REGWEN_6, 0x44u)
REG32(SW_RST_REGWEN_7, 0x48u)
REG32(SW_RST_CTRL_N_0, 0x4cu)
    SHARED_FIELD(SW_RST_CTRL_VAL, 0u, 1u)
REG32(SW_RST_CTRL_N_1, 0x50u)
REG32(SW_RST_CTRL_N_2, 0x54u)
REG32(SW_RST_CTRL_N_3, 0x58u)
REG32(SW_RST_CTRL_N_4, 0x5cu)
REG32(SW_RST_CTRL_N_5, 0x60u)
REG32(SW_RST_CTRL_N_6, 0x64u)
REG32(SW_RST_CTRL_N_7, 0x68u)
REG32(ERR_CODE, 0x6cu)
    FIELD(ERR_CODE, REG_INTG_ERR, 0u, 1u)
    FIELD(ERR_CODE, RESET_CONSISTENCY_ERR, 1u, 1u)
    FIELD(ERR_CODE, FSM_ERR, 2u, 1u)
/* clang-format on */

#define ALERT_TEST_MASK \
    (R_ALERT_TEST_FATAL_FAULT_MASK | R_ALERT_TEST_FATAL_CNSTY_FAULT_MASK)
#define RESET_INFO_MASK \
    (R_RESET_INFO_POR_MASK | R_RESET_INFO_LOW_POWER_EXIT_MASK | \
     R_RESET_INFO_SW_RESET_MASK | R_RESET_INFO_HW_REQ_MASK)
#define ALERT_INFO_CTRL_MASK \
    (R_ALERT_INFO_CTRL_EN_MASK | R_ALERT_INFO_CTRL_INDEX_MASK)
#define CPU_INFO_CTRL_MASK \
    (R_CPU_INFO_CTRL_EN_MASK | R_CPU_INFO_CTRL_INDEX_MASK)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_ERR_CODE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),      REG_NAME_ENTRY(RESET_REQ),
    REG_NAME_ENTRY(RESET_INFO),      REG_NAME_ENTRY(ALERT_REGWEN),
    REG_NAME_ENTRY(ALERT_INFO_CTRL), REG_NAME_ENTRY(ALERT_INFO_ATTR),
    REG_NAME_ENTRY(ALERT_INFO),      REG_NAME_ENTRY(CPU_REGWEN),
    REG_NAME_ENTRY(CPU_INFO_CTRL),   REG_NAME_ENTRY(CPU_INFO_ATTR),
    REG_NAME_ENTRY(CPU_INFO),        REG_NAME_ENTRY(SW_RST_REGWEN_0),
    REG_NAME_ENTRY(SW_RST_REGWEN_1), REG_NAME_ENTRY(SW_RST_REGWEN_2),
    REG_NAME_ENTRY(SW_RST_REGWEN_3), REG_NAME_ENTRY(SW_RST_REGWEN_4),
    REG_NAME_ENTRY(SW_RST_REGWEN_5), REG_NAME_ENTRY(SW_RST_REGWEN_6),
    REG_NAME_ENTRY(SW_RST_REGWEN_7), REG_NAME_ENTRY(SW_RST_CTRL_N_0),
    REG_NAME_ENTRY(SW_RST_CTRL_N_1), REG_NAME_ENTRY(SW_RST_CTRL_N_2),
    REG_NAME_ENTRY(SW_RST_CTRL_N_3), REG_NAME_ENTRY(SW_RST_CTRL_N_4),
    REG_NAME_ENTRY(SW_RST_CTRL_N_5), REG_NAME_ENTRY(SW_RST_CTRL_N_6),
    REG_NAME_ENTRY(SW_RST_CTRL_N_7), REG_NAME_ENTRY(ERR_CODE),
};
#undef REG_NAME_ENTRY

struct OtRstMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ sw_reset;
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    QEMUBH *bus_reset_bh;
    CPUState *cpu;

    uint32_t *regs;

    char *ot_id;
    uint32_t fatal_reset;
    bool por; /* Power-On Reset property */
};

typedef struct {
    const char *typename;
    unsigned idx;
} OtRstMgrResettable;

typedef struct {
    char *path;
    bool reset;
} OtRstMgrResetDesc;

static const OtRstMgrResettable SW_RESETTABLE_DEVICES[PARAM_NUM_SW_RESETS] = {
    [0u] = { NULL, 0u },
    [1u] = { TYPE_OT_SPI_HOST, 0u },
    [2u] = { TYPE_OT_SPI_HOST, 1u },
    [3u] = { NULL, 0u },
    [4u] = { NULL, 0u },
    [5u] = { NULL, 0u },
    [6u] = { NULL, 0u },
    [7u] = { NULL, 0u },
};

static_assert(PARAM_NUM_TOTAL_RESETS == OT_RSTMGR_RESET_COUNT,
              "Invalid reset count");

#define REQ_NAME_ENTRY(_req_) [OT_RSTMGR_RESET_##_req_] = stringify(_req_)
static const char *OT_RST_MGR_REQUEST_NAMES[] = {
    REQ_NAME_ENTRY(POR),
    REQ_NAME_ENTRY(LOW_POWER),
    REQ_NAME_ENTRY(SW),
    REQ_NAME_ENTRY(SYSCTRL),
    REQ_NAME_ENTRY(AON_TIMER),
    REQ_NAME_ENTRY(PWRMGR),
    REQ_NAME_ENTRY(ALERT_HANDLER),
    REQ_NAME_ENTRY(RV_DM),
};
#undef REQ_NAME_ENTRY
#define REQ_NAME(_req_) \
    ((_req_) < ARRAY_SIZE(OT_RST_MGR_REQUEST_NAMES)) ? \
        OT_RST_MGR_REQUEST_NAMES[(_req_)] : \
        "?"

/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static void ot_rstmgr_update_alerts(OtRstMgrState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_rstmgr_reset_bus(void *opaque)
{
    OtRstMgrState *s = opaque;

    /* request the vCPU to stop */
    s->cpu->stop = true;

    /* wait for the vCPU to stop */
    while (!s->cpu->stopped) {
        bql_unlock();
        qemu_cpu_kick(s->cpu);
        bql_lock();
    }
    qemu_notify_event();

    cpu_synchronize_state(s->cpu);
    /* Reset all OpenTitan devices connected to RSTMGR parent bus */
    bus_cold_reset(s->parent_obj.parent_obj.parent_bus);
    cpu_synchronize_post_reset(s->cpu);

    /* TODO: manage reset tree (depending on power domains, etc.) */
}

static int ot_rstmgr_sw_rst_walker(DeviceState *dev, void *opaque)
{
    OtRstMgrResetDesc *desc = opaque;

    int match =
        strcmp(object_get_canonical_path_component(OBJECT(dev)), desc->path);

    if (match) {
        /* not the instance that is seeked, resume walk */
        return 0;
    }

    trace_ot_rstmgr_sw_rst(desc->path, desc->reset);

    if (desc->reset) {
        resettable_assert_reset(OBJECT(dev), RESET_TYPE_COLD);
    } else {
        resettable_release_reset(OBJECT(dev), RESET_TYPE_COLD);
    }

    /* abort walk immediately */
    return -1;
}

static void ot_rstmgr_update_sw_reset(OtRstMgrState *s, unsigned devix)
{
    assert(devix < ARRAY_SIZE(SW_RESETTABLE_DEVICES));

    const OtRstMgrResettable *rst = &SW_RESETTABLE_DEVICES[devix];
    if (!rst->typename) {
        qemu_log_mask(LOG_UNIMP, "%s: %s Reset for slot %u not yet implemented",
                      __func__, s->ot_id, devix);
        return;
    }

    OtRstMgrResetDesc desc;

    desc.path = g_strdup_printf("%s[%d]", rst->typename, rst->idx);
    desc.reset = !s->regs[R_SW_RST_CTRL_N_0 + devix];

    trace_ot_rstmgr_sw_reset(desc.path);

    /* search for the device on the same local bus */
    int res =
        qbus_walk_children(s->parent_obj.parent_obj.parent_bus,
                           &ot_rstmgr_sw_rst_walker, NULL, NULL, NULL, &desc);
    if (res >= 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Unable to locate device %s",
                      __func__, s->ot_id, desc.path);
    }

    g_free(desc.path);
}

static void ot_rstmgr_reset_req(void *opaque, int irq, int level)
{
    OtRstMgrState *s = opaque;

    if (!level) {
        /* reset line released */
        return;
    }

    g_assert(irq == 0);

    bool fastclk = ((unsigned)level >> 8u) & 1u;

    level &= 0xff;
    g_assert(level < OT_RSTMGR_RESET_COUNT);

    OtRstMgrResetReq req = (OtRstMgrResetReq)level;
    s->regs[R_RESET_INFO] = 1u << req;

    trace_ot_rstmgr_reset_req(REQ_NAME(req), req, fastclk);

    qemu_bh_schedule(s->bus_reset_bh);
}

static uint64_t ot_rstmgr_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtRstMgrState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_RESET_REQ:
    case R_RESET_INFO:
    case R_ALERT_REGWEN:
    case R_ALERT_INFO_CTRL:
    case R_ALERT_INFO_ATTR:
    case R_ALERT_INFO:
    case R_CPU_REGWEN:
    case R_CPU_INFO_CTRL:
    case R_CPU_INFO_ATTR:
    case R_CPU_INFO:
    case R_SW_RST_REGWEN_0:
    case R_SW_RST_REGWEN_1:
    case R_SW_RST_REGWEN_2:
    case R_SW_RST_REGWEN_3:
    case R_SW_RST_REGWEN_4:
    case R_SW_RST_REGWEN_5:
    case R_SW_RST_REGWEN_6:
    case R_SW_RST_REGWEN_7:
    case R_SW_RST_CTRL_N_0:
    case R_SW_RST_CTRL_N_1:
    case R_SW_RST_CTRL_N_2:
    case R_SW_RST_CTRL_N_3:
    case R_SW_RST_CTRL_N_4:
    case R_SW_RST_CTRL_N_5:
    case R_SW_RST_CTRL_N_6:
    case R_SW_RST_CTRL_N_7:
    case R_ERR_CODE:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x02%" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
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
    trace_ot_rstmgr_io_read_out((uint32_t)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
};

static void ot_rstmgr_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtRstMgrState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_rstmgr_io_write((uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_RESET_REQ:
        val32 &= R_RESET_REQ_VAL_MASK;
        s->regs[reg] = val32;
        if (val32 == OT_MULTIBITBOOL4_TRUE) {
            /*
             * "Upon completion of reset, this bit is automatically cleared by
             * hardware."
             */
            ibex_irq_set(&s->sw_reset, (int)true);
            if (s->fatal_reset) {
                s->fatal_reset--;
                if (!s->fatal_reset) {
                    error_report("fatal reset triggered");
                    qemu_system_shutdown_request_with_code(
                        SHUTDOWN_CAUSE_GUEST_SHUTDOWN, 1);
                }
            }
        }
        break;
    case R_RESET_INFO:
        val32 &= RESET_INFO_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        break;
    case R_ALERT_REGWEN:
        val32 &= R_ALERT_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_ALERT_INFO_CTRL:
        if (s->regs[R_ALERT_REGWEN]) {
            val32 &= ALERT_INFO_CTRL_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                          __func__, s->ot_id, REG_NAME(reg));
        }
        break;
    case R_CPU_REGWEN:
        val32 &= R_CPU_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CPU_INFO_CTRL:
        if (s->regs[R_CPU_REGWEN]) {
            val32 &= CPU_INFO_CTRL_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                          __func__, s->ot_id, REG_NAME(reg));
        }
        break;
    case R_SW_RST_REGWEN_0:
    case R_SW_RST_REGWEN_1:
    case R_SW_RST_REGWEN_2:
    case R_SW_RST_REGWEN_3:
    case R_SW_RST_REGWEN_4:
    case R_SW_RST_REGWEN_5:
    case R_SW_RST_REGWEN_6:
    case R_SW_RST_REGWEN_7:
        val32 &= SW_RST_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_SW_RST_CTRL_N_0:
    case R_SW_RST_CTRL_N_1:
    case R_SW_RST_CTRL_N_2:
    case R_SW_RST_CTRL_N_3:
    case R_SW_RST_CTRL_N_4:
    case R_SW_RST_CTRL_N_5:
    case R_SW_RST_CTRL_N_6:
    case R_SW_RST_CTRL_N_7:
        if (s->regs[reg - R_SW_RST_CTRL_N_0 + R_SW_RST_REGWEN_0]) {
            val32 &= SW_RST_CTRL_VAL_MASK;
            uint32_t change = s->regs[reg] ^ val32;
            s->regs[reg] = val32;
            unsigned devix = (unsigned)reg - R_SW_RST_CTRL_N_0;
            if (change & (1u << devix)) {
                ot_rstmgr_update_sw_reset(s, devix);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                          __func__, s->ot_id, REG_NAME(reg));
        }
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        s->regs[reg] = val32;
        ot_rstmgr_update_alerts(s);
        break;
    case R_ALERT_INFO_ATTR:
    case R_ALERT_INFO:
    case R_CPU_INFO_ATTR:
    case R_CPU_INFO:
    case R_ERR_CODE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x02%" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
};

static Property ot_rstmgr_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtRstMgrState, ot_id),
    DEFINE_PROP_UINT32("fatal_reset", OtRstMgrState, fatal_reset, 0),
    /* this property is only used to store initial reset reason state */
    DEFINE_PROP_BOOL("por", OtRstMgrState, por, true),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_rstmgr_regs_ops = {
    .read = &ot_rstmgr_regs_read,
    .write = &ot_rstmgr_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_rstmgr_reset(DeviceState *dev)
{
    OtRstMgrState *s = OT_RSTMGR(dev);

    if (!s->ot_id) {
        s->ot_id =
            g_strdup(object_get_canonical_path_component(OBJECT(s)->parent));
    }

    trace_ot_rstmgr_reset();

    if (!s->cpu) {
        CPUState *cpu = ot_common_get_local_cpu(DEVICE(s));
        if (!cpu) {
            error_setg(&error_fatal, "%s: Could not find the associated vCPU",
                       s->ot_id);
            g_assert_not_reached();
        }
        s->cpu = cpu;
    }

    s->regs[R_RESET_REQ] = OT_MULTIBITBOOL4_FALSE;
    if (s->por) {
        memset(s->regs, 0, REGS_SIZE);
        s->regs[R_RESET_INFO] = R_RESET_INFO_POR_MASK;
        s->por = false;
    } else {
        /* TODO: need to check which registers are actually reset when !PoR */
        s->regs[R_ALERT_TEST] = 0u;
    }

    s->regs[R_ALERT_REGWEN] = 0x1u;
    s->regs[R_CPU_REGWEN] = 0x1u;
    for (unsigned ix = 0; ix < PARAM_NUM_SW_RESETS; ix++) {
        s->regs[R_SW_RST_REGWEN_0 + ix] = 0x1u;
    }
    for (unsigned ix = 0; ix < PARAM_NUM_SW_RESETS; ix++) {
        s->regs[R_SW_RST_CTRL_N_0 + ix] = 0x1u;
    }

    ibex_irq_set(&s->sw_reset, 0);
    ot_rstmgr_update_alerts(s);
}

static void ot_rstmgr_init(Object *obj)
{
    OtRstMgrState *s = OT_RSTMGR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_rstmgr_regs_ops, s, TYPE_OT_RSTMGR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);

    ibex_qdev_init_irq(obj, &s->sw_reset, OT_RSTMGR_SW_RST);
    ibex_qdev_init_irqs(obj, s->alerts, OT_DEVICE_ALERT, PARAM_NUM_ALERTS);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_rstmgr_reset_req,
                            OT_RSTMGR_RST_REQ, 1);

    s->bus_reset_bh = qemu_bh_new(&ot_rstmgr_reset_bus, s);
}

static void ot_rstmgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_rstmgr_reset;
    device_class_set_props(dc, ot_rstmgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_rstmgr_info = {
    .name = TYPE_OT_RSTMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtRstMgrState),
    .instance_init = &ot_rstmgr_init,
    .class_init = &ot_rstmgr_class_init,
};

static void ot_rstmgr_register_types(void)
{
    type_register_static(&ot_rstmgr_info);
}

type_init(ot_rstmgr_register_types);
