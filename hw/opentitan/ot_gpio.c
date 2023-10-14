/*
 * QEMU OpenTitan GPIO device
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
 *  Samuel Ortiz <sameo@rivosinc.com>
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
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/hw.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_gpio.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

/*
 * Unfortunately, there is no QEMU API to properly disable serial control lines
 */
#ifndef _WIN32
#include <termios.h>
#include "chardev/char-fd.h"
#include "io/channel-file.h"
#endif


#define PARAM_NUM_ALERTS 1u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT_ERR, 0u, 1u)
REG32(DATA_IN, 0x10u)
REG32(DIRECT_OUT, 0x14u)
REG32(MASKED_OUT_LOWER, 0x18u)
    SHARED_FIELD(MASKED_VALUE, 0u, 16u)
    SHARED_FIELD(MASKED_MASK, 16u, 16u)
REG32(MASKED_OUT_UPPER, 0x1cu)
REG32(DIRECT_OE, 0x20u)
REG32(MASKED_OE_LOWER, 0x24u)
REG32(MASKED_OE_UPPER, 0x28u)
REG32(INTR_CTRL_EN_RISING, 0x2cu)
REG32(INTR_CTRL_EN_FALLING, 0x30u)
REG32(INTR_CTRL_EN_LVLHIGH, 0x34u)
REG32(INTR_CTRL_EN_LVLLOW, 0x38u)
REG32(CTRL_EN_INPUT_FILTER, 0x3cu)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_CTRL_EN_INPUT_FILTER)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define ALERT_TEST_MASK (R_ALERT_TEST_FATAL_FAULT_ERR_MASK)

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)

static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(DATA_IN),
    REG_NAME_ENTRY(DIRECT_OUT),
    REG_NAME_ENTRY(MASKED_OUT_LOWER),
    REG_NAME_ENTRY(MASKED_OUT_UPPER),
    REG_NAME_ENTRY(DIRECT_OE),
    REG_NAME_ENTRY(MASKED_OE_LOWER),
    REG_NAME_ENTRY(MASKED_OE_UPPER),
    REG_NAME_ENTRY(INTR_CTRL_EN_RISING),
    REG_NAME_ENTRY(INTR_CTRL_EN_FALLING),
    REG_NAME_ENTRY(INTR_CTRL_EN_LVLHIGH),
    REG_NAME_ENTRY(INTR_CTRL_EN_LVLLOW),
    REG_NAME_ENTRY(CTRL_EN_INPUT_FILTER),
};
#undef REG_NAME_ENTRY

struct OtGpioState {
    SysBusDevice parent_obj;

    IbexIRQ irqs[32u];
    IbexIRQ alert;

    MemoryRegion mmio;

    uint32_t regs[REGS_COUNT];
    uint32_t data_out;
    uint32_t data_oe;
    uint32_t data_in;

    char ibuf[32u]; /* backed input buffer */
    unsigned ipos;

    uint32_t reset_in; /* initial input levels */
    CharBackend chr; /* communication device */
    guint watch_tag; /* tracker for comm device change */
};

static void ot_gpio_update_irqs(OtGpioState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_gpio_irqs(s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE], level);
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_gpio_update_intr_level(OtGpioState *s)
{
    uint32_t intr_state = 0;

    intr_state |= s->regs[R_INTR_CTRL_EN_LVLLOW] & ~s->regs[R_DATA_IN];
    intr_state |= s->regs[R_INTR_CTRL_EN_LVLHIGH] & s->regs[R_DATA_IN];

    s->regs[R_INTR_STATE] |= intr_state;
}

static void ot_gpio_update_intr_edge(OtGpioState *s, uint32_t prev)
{
    uint32_t change = prev ^ s->regs[R_DATA_IN];
    uint32_t rising = change & s->regs[R_DATA_IN];
    uint32_t falling = change & ~s->regs[R_DATA_IN];

    uint32_t intr_state = 0;

    intr_state |= s->regs[R_INTR_CTRL_EN_RISING] & rising;
    intr_state |= s->regs[R_INTR_CTRL_EN_FALLING] & falling;

    s->regs[R_INTR_STATE] |= intr_state;
}

static void ot_gpio_update_data_in(OtGpioState *s)
{
    uint32_t prev = s->regs[R_DATA_IN];
    uint32_t data_mix = s->data_in & ~s->data_oe;
    data_mix |= s->data_out & s->data_oe;
    s->regs[R_DATA_IN] = data_mix;
    trace_ot_gpio_update_input(prev, s->data_in, data_mix);
    ot_gpio_update_intr_level(s);
    ot_gpio_update_intr_edge(s, prev);
    ot_gpio_update_irqs(s);
}

static void ot_gpio_update_backend(OtGpioState *s, bool oe)
{
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }

    char buf[32u];
    size_t len;

    /*
     * use the infamous MS DOS CR LF syntax because people can't help using
     * Windows-style terminal
     */
    if (oe) {
        len = snprintf(&buf[0], sizeof(buf), "D:%08x\r\n", s->data_oe);
    } else {
        len = 0;
    }

    len += snprintf(&buf[len], sizeof(buf), "O:%08x\r\n", s->data_out);

    qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, (int)len);
}

static uint64_t ot_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    OtGpioState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_DATA_IN:
    case R_DIRECT_OUT:
    case R_DIRECT_OE:
    case R_INTR_CTRL_EN_RISING:
    case R_INTR_CTRL_EN_FALLING:
    case R_INTR_CTRL_EN_LVLHIGH:
    case R_INTR_CTRL_EN_LVLLOW:
    case R_CTRL_EN_INPUT_FILTER:
        val32 = s->regs[reg];
        break;
    case R_MASKED_OUT_LOWER:
        val32 = s->data_out & MASKED_VALUE_MASK;
        break;
    case R_MASKED_OUT_UPPER:
        val32 = (s->data_out >> MASKED_MASK_SHIFT) & MASKED_VALUE_MASK;
        break;
    case R_MASKED_OE_LOWER:
        val32 = s->data_oe & MASKED_VALUE_MASK;
        break;
    case R_MASKED_OE_UPPER:
        val32 = (s->data_oe >> MASKED_MASK_SHIFT) & MASKED_VALUE_MASK;
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
        val32 = 0u;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_gpio_io_read((unsigned)addr, REG_NAME(reg), (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_gpio_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned size)
{
    OtGpioState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    uint32_t mask;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_gpio_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        s->regs[reg] &= ~val32; /* RW1C */
        ot_gpio_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        s->regs[reg] = val32;
        ot_gpio_update_irqs(s);
        break;
    case R_INTR_TEST:
        s->regs[R_INTR_STATE] |= val32;
        ot_gpio_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case R_DIRECT_OUT:
        s->regs[reg] = val32;
        s->data_out = val32;
        ot_gpio_update_backend(s, false);
        ot_gpio_update_data_in(s);
        break;
    case R_DIRECT_OE:
        s->regs[reg] = val32;
        s->data_oe = val32;
        ot_gpio_update_backend(s, true);
        ot_gpio_update_data_in(s);
        break;
    case R_MASKED_OUT_LOWER:
        s->regs[reg] = val32;
        mask = val32 >> MASKED_MASK_SHIFT;
        s->data_out &= ~mask;
        s->data_out |= val32 & mask;
        ot_gpio_update_backend(s, false);
        ot_gpio_update_data_in(s);
        break;
    case R_MASKED_OUT_UPPER:
        s->regs[reg] = val32;
        mask = val32 & MASKED_MASK_MASK;
        s->data_out &= ~mask;
        s->data_out |= (val32 << MASKED_MASK_SHIFT) & mask;
        ot_gpio_update_backend(s, false);
        ot_gpio_update_data_in(s);
        break;
    case R_MASKED_OE_LOWER:
        s->regs[reg] = val32;
        mask = val32 >> MASKED_MASK_SHIFT;
        s->data_oe &= ~mask;
        s->data_oe |= val32 & mask;
        ot_gpio_update_backend(s, true);
        ot_gpio_update_data_in(s);
        break;
    case R_MASKED_OE_UPPER:
        s->regs[reg] = val32;
        mask = val32 & MASKED_MASK_MASK;
        s->data_oe &= ~mask;
        s->data_oe |= (val32 << MASKED_MASK_SHIFT) & mask;
        ot_gpio_update_backend(s, true);
        ot_gpio_update_data_in(s);
        break;
    case R_INTR_CTRL_EN_RISING:
    case R_INTR_CTRL_EN_FALLING:
        s->regs[reg] = val32;
        break;
    case R_INTR_CTRL_EN_LVLHIGH:
    case R_INTR_CTRL_EN_LVLLOW:
        s->regs[reg] = val32;
        ot_gpio_update_data_in(s);
        break;
    case R_CTRL_EN_INPUT_FILTER:
        /* nothing can be done at QEMU level for sampling that fast */
        s->regs[reg] = val32;
        break;
    case R_DATA_IN:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static int ot_gpio_chr_can_receive(void *opaque)
{
    OtGpioState *s = opaque;

    return (int)sizeof(s->ibuf) - (int)s->ipos;
}

static void ot_gpio_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    OtGpioState *s = opaque;

    if (s->ipos + (unsigned)size > sizeof(s->ibuf)) {
        qemu_log("%s: Incoherent chardev receive\n", __func__);
        return;
    }

    memcpy(&s->ibuf[s->ipos], buf, (size_t)size);
    s->ipos += (unsigned)size;

    for (;;) {
        const char *eol = memchr(s->ibuf, (int)'\n', s->ipos);
        if (!eol) {
            if (s->ipos > 10u) {
                /* discard any garbage */
                memset(s->ibuf, 0, sizeof(s->ibuf));
                s->ipos = 0;
            }
            return;
        }
        unsigned eolpos = eol - s->ibuf;
        if (eolpos < 10u) {
            memmove(s->ibuf, eol + 1u, eolpos + 1u);
            s->ipos = 0;
            continue;
        }
        uint32_t data_in = 0;
        char cmd = '\0';
        /* NOLINTNEXTLINE */
        int ret = sscanf(s->ibuf, "%c:%08x", &cmd, &data_in);
        memmove(s->ibuf, eol + 1u, eolpos + 1u);
        s->ipos = 0;

        if (ret == 2) {
            if (cmd == 'I') {
                s->data_in = data_in;
                ot_gpio_update_data_in(s);
            } else if (cmd == 'R') {
                ot_gpio_update_backend(s, true);
            }
        }
    }
}

static void ot_gpio_chr_ignore_status_lines(OtGpioState *s)
{
/* it might be useful to move this to char-serial.c */
#ifndef _WIN32
    FDChardev *cd = FD_CHARDEV(s->chr.chr);
    QIOChannelFile *fioc = QIO_CHANNEL_FILE(cd->ioc_in);

    struct termios tty = { 0 };
    tcgetattr(fioc->fd, &tty);
    tty.c_cflag |= CLOCAL; /* ignore modem status lines */
    tcsetattr(fioc->fd, TCSANOW, &tty);
#endif
}

static void ot_gpio_chr_event_hander(void *opaque, QEMUChrEvent event)
{
    OtGpioState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        if (object_dynamic_cast(OBJECT(s->chr.chr), TYPE_CHARDEV_SERIAL)) {
            ot_gpio_chr_ignore_status_lines(s);
        }

        ot_gpio_update_backend(s, true);

        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            return;
        }

        /* query backend for current input status */
        char buf[16u];
        int len = snprintf(buf, sizeof(buf), "Q:%08x\r\n", s->data_oe);
        qemu_chr_fe_write(&s->chr, (const uint8_t *)buf, len);
    }
}

static gboolean ot_gpio_chr_watch_cb(void *do_not_use, GIOCondition cond,
                                     void *opaque)
{
    OtGpioState *s = opaque;
    (void)do_not_use;
    (void)cond;

    s->watch_tag = 0;

    return FALSE;
}

static int ot_gpio_chr_be_change(void *opaque)
{
    OtGpioState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, &ot_gpio_chr_can_receive,
                             &ot_gpio_chr_receive, &ot_gpio_chr_event_hander,
                             &ot_gpio_chr_be_change, s, NULL, true);

    memset(s->ibuf, 0, sizeof(s->ibuf));
    s->ipos = 0;

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             &ot_gpio_chr_watch_cb, s);
    }

    return 0;
}

static const MemoryRegionOps ot_gpio_regs_ops = {
    .read = &ot_gpio_read,
    .write = &ot_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static Property ot_gpio_properties[] = {
    DEFINE_PROP_UINT32("in", OtGpioState, reset_in, 0u),
    DEFINE_PROP_CHR("chardev", OtGpioState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_gpio_reset(DeviceState *dev)
{
    OtGpioState *s = OT_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->data_out = 0;
    s->data_oe = 0;
    s->data_in = s->reset_in;
    s->regs[R_DATA_IN] = s->reset_in;

    ot_gpio_update_irqs(s);
    ibex_irq_set(&s->alert, 0);

    ot_gpio_update_backend(s, true);

    /*
     * do not reset the input backed buffer as external GPIO changes is fully
     * async with OT reset. However, it should be reset when the backend changes
     */
}

static void ot_gpio_realize(DeviceState *dev, Error **errp)
{
    OtGpioState *s = OT_GPIO(dev);
    (void)errp;

    qemu_chr_fe_set_handlers(&s->chr, &ot_gpio_chr_can_receive,
                             &ot_gpio_chr_receive, &ot_gpio_chr_event_hander,
                             &ot_gpio_chr_be_change, s, NULL, true);
}

static void ot_gpio_init(Object *obj)
{
    OtGpioState *s = OT_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &ot_gpio_regs_ops, s, TYPE_OT_GPIO,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);
}

static void ot_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_gpio_reset;
    dc->realize = &ot_gpio_realize;
    device_class_set_props(dc, ot_gpio_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_gpio_info = {
    .name = TYPE_OT_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtGpioState),
    .instance_init = &ot_gpio_init,
    .class_init = &ot_gpio_class_init,
};

static void ot_gpio_register_types(void)
{
    type_register_static(&ot_gpio_info);
}

type_init(ot_gpio_register_types)
