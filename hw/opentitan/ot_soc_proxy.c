/*
 * QEMU OpenTitan SocProxy
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
 * IMPLIED, INCLUDING BUT NOT LIMIT_ADDRED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Note that system-side interrupts are not managed by the DOE Mailbox.
 * Registers dedicated to system-side interrupt management are only storage
 * space that the guest software (called the host side to get more confusing,
 * not related to the VM host) should read and act accordingly, using other
 * devices to signal the requester that a response is ready to be read.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/memory.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_soc_proxy.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

#define PARAM_NUM_EXTERNAL_IRQS 32u
#define PARAM_NUM_ALERTS        29u

/* clang-format off */

REG32(INTR_STATE, 0x0)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)

/* clang-format on */

#define ALERT_TEST_FATAL_ALERT_INTG           0
#define ALERT_TEST_FATAL_ALERT_EXTERNAL_BASE  1u
#define ALERT_TEST_FATAL_ALERT_EXTERNAL_COUNT 24u
#define ALERT_TEST_RECOV_ALERT_EXTERNAL_BASE  25u
#define ALERT_TEST_RECOV_ALERT_EXTERNAL_COUNT 4u

#define ALERT_TEST_MASK ((1u << PARAM_NUM_ALERTS) - 1u)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_ALERT_TEST)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define INTR_COUNT PARAM_NUM_EXTERNAL_IRQS

static_assert(1u + ALERT_TEST_FATAL_ALERT_EXTERNAL_COUNT +
                      ALERT_TEST_RECOV_ALERT_EXTERNAL_COUNT ==
                  PARAM_NUM_ALERTS,
              "Invalid external IRQ configuration");
static_assert(OT_SOC_PROXY_REGS_COUNT == REGS_COUNT, "Invalid regs");

#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

/* clang-format off */
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
};
/* clang-format on */

#undef REG_NAME_ENTRY

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
};

typedef struct {
} OtMbxHost;

struct OtSoCProxyState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[INTR_COUNT];
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    uint32_t regs[REGS_COUNT];

    char *ot_id;
};

static void ot_soc_proxy_update_irqs(OtSoCProxyState *s)
{
    uint32_t levels = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        int level = (int)(bool)(levels & (1u << ix));
        if (level != ibex_irq_get_level(&s->irqs[ix])) {
            trace_ot_soc_proxy_update_irq(s->ot_id, ix,
                                          ibex_irq_get_level(&s->irqs[ix]),
                                          level);
        }
        ibex_irq_set(&s->irqs[ix], level);
    }
}

static void ot_soc_proxy_update_alerts(OtSoCProxyState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_soc_proxy_ingress_irq(void *opaque, int n, int level)
{
    OtSoCProxyState *s = opaque;

    g_assert(n < INTR_COUNT);

    trace_ot_soc_proxy_ingress_irq(s->ot_id, (unsigned)n, (bool)level);

    uint32_t bm = 1u << n;
    if (level) { /* RW1S */
        s->regs[R_INTR_STATE] |= bm;
        ot_soc_proxy_update_irqs(s);
    }
}

static uint64_t ot_soc_proxy_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSoCProxyState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
        val32 = s->regs[reg];
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_soc_proxy_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                   val32, pc);

    return (uint64_t)val32;
};

static void ot_soc_proxy_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtSoCProxyState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_soc_proxy_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                pc);

    switch (reg) {
    case R_INTR_STATE:
        s->regs[reg] &= ~val32; /* RW1C */
        ot_soc_proxy_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->regs[reg] = val32;
        ot_soc_proxy_update_irqs(s);
        break;
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32;
        ot_soc_proxy_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        s->regs[reg] = val32;
        ot_soc_proxy_update_alerts(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static Property ot_soc_proxy_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtSoCProxyState, ot_id),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_soc_proxy_regs_ops = {
    .read = &ot_soc_proxy_regs_read,
    .write = &ot_soc_proxy_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_soc_proxy_reset(DeviceState *dev)
{
    OtSoCProxyState *s = OT_SOC_PROXY(dev);

    g_assert(s->ot_id);

    memset(s->regs, 0, sizeof(s->regs));

    ot_soc_proxy_update_irqs(s);
    ot_soc_proxy_update_alerts(s);
}

static void ot_soc_proxy_init(Object *obj)
{
    OtSoCProxyState *s = OT_SOC_PROXY(obj);

    memory_region_init_io(&s->mmio, obj, &ot_soc_proxy_regs_ops, s,
                          TYPE_OT_SOC_PROXY, REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    qdev_init_gpio_in_named_with_opaque(DEVICE(s), &ot_soc_proxy_ingress_irq, s,
                                        NULL, INTR_COUNT);
}

static void ot_soc_proxy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_soc_proxy_reset;
    device_class_set_props(dc, ot_soc_proxy_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_soc_proxy_info = {
    .name = TYPE_OT_SOC_PROXY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSoCProxyState),
    .instance_init = &ot_soc_proxy_init,
    .class_init = &ot_soc_proxy_class_init,
};

static void ot_soc_proxy_register_types(void)
{
    type_register_static(&ot_soc_proxy_info);
}

type_init(ot_soc_proxy_register_types);
