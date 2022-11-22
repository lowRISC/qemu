/*
 * QEMU OpenTitan UART device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * Based on original ibex_uart implementation:
 *  Copyright (c) 2020 Western Digital
 *  Alistair Francis <alistair.francis@wdc.com>
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
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_TX_WATERMARK, 0u, 1u)
    SHARED_FIELD(INTR_RX_WATERMARK, 1u, 1u)
    SHARED_FIELD(INTR_TX_EMPTY, 2u, 1u)
    SHARED_FIELD(INTR_RX_OVERFLOW, 3u, 1u)
    SHARED_FIELD(INTR_RX_FRAME_ERR, 4u, 1u)
    SHARED_FIELD(INTR_RX_BREAK_ERR, 5u, 1u)
    SHARED_FIELD(INTR_RX_TIMEOUT, 6u, 1u)
    SHARED_FIELD(INTR_RX_PARITY_ERR, 7u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CTRL, 0x10u)
    FIELD(CTRL, TX, 0u, 1u)
    FIELD(CTRL, RX, 1u, 1u)
    FIELD(CTRL, NF, 2u, 1u)
    FIELD(CTRL, SLPBK, 4u, 1u)
    FIELD(CTRL, LLPBK, 5u, 1u)
    FIELD(CTRL, PARITY_EN, 6u, 1u)
    FIELD(CTRL, PARITY_ODD, 7u, 1u)
    FIELD(CTRL, RXBLVL, 8u, 2u)
    FIELD(CTRL, NCO, 16u, 16u)
REG32(STATUS, 0x14u)
    FIELD(STATUS, TXFULL, 0u, 1u)
    FIELD(STATUS, RXFULL, 1u, 1u)
    FIELD(STATUS, TXEMPTY, 2u, 1u)
    FIELD(STATUS, TXIDLE, 3u, 1u)
    FIELD(STATUS, RXIDLE, 4u, 1u)
    FIELD(STATUS, RXEMPTY, 5u, 1u)
REG32(RDATA, 0x18u)
    FIELD(RDATA, RDATA, 0u, 8u)
REG32(WDATA, 0x1cu)
    FIELD(WDATA, WDATA, 0u, 8u)
REG32(FIFO_CTRL, 0x20u)
    FIELD(FIFO_CTRL, RXRST, 0u, 1u)
    FIELD(FIFO_CTRL, TXRST, 1u, 1u)
    FIELD(FIFO_CTRL, RXILVL, 2u, 3u)
    FIELD(FIFO_CTRL, TXILVL, 5u, 3u)
REG32(FIFO_STATUS, 0x24u)
    FIELD(FIFO_STATUS, TXLVL, 0u, 8u)
    FIELD(FIFO_STATUS, RXLVL, 16u, 8u)
REG32(OVRD, 0x28u)
    FIELD(OVRD, TXEN, 0u, 1u)
    FIELD(OVRD, TXVAL, 1u, 1u)
REG32(VAL, 0x2cu)
    FIELD(VAL, RX, 0u, 16u)
REG32(TIMEOUT_CTRL, 0x30u)
    FIELD(TIMEOUT_CTRL, VAL, 0u, 24)
    FIELD(TIMEOUT_CTRL, EN, 31u, 1u)
/* clang-format on */

#define INTR_MASK \
    (INTR_TX_WATERMARK_MASK | INTR_RX_WATERMARK_MASK | INTR_TX_EMPTY_MASK | \
     INTR_RX_OVERFLOW_MASK | INTR_RX_FRAME_ERR_MASK | INTR_RX_BREAK_ERR_MASK | \
     INTR_RX_TIMEOUT_MASK | INTR_RX_PARITY_ERR_MASK)

#define CTRL_MASK \
    (R_CTRL_TX_MASK | R_CTRL_RX_MASK | R_CTRL_NF_MASK | R_CTRL_SLPBK_MASK | \
     R_CTRL_LLPBK_MASK | R_CTRL_PARITY_EN_MASK | R_CTRL_PARITY_ODD_MASK | \
     R_CTRL_RXBLVL_MASK | R_CTRL_NCO_MASK)

#define CTRL_SUP_MASK \
    (R_CTRL_RX_MASK | R_CTRL_TX_MASK | R_CTRL_SLPBK_MASK | R_CTRL_NCO_MASK)

#define OT_UART_NCO_BITS     16u
#define OT_UART_TX_FIFO_SIZE 128u
#define OT_UART_RX_FIFO_SIZE 128u
#define OT_UART_IRQ_NUM      8u

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_TIMEOUT_CTRL)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CTRL),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(RDATA),
    REG_NAME_ENTRY(WDATA),
    REG_NAME_ENTRY(FIFO_CTRL),
    REG_NAME_ENTRY(FIFO_STATUS),
    REG_NAME_ENTRY(OVRD),
    REG_NAME_ENTRY(VAL),
    REG_NAME_ENTRY(TIMEOUT_CTRL),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

struct OtUARTState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    Fifo8 tx_fifo;
    Fifo8 rx_fifo;
    uint32_t tx_watermark_level;

    uint32_t regs[REGS_COUNT];
    uint32_t pclk;

    CharBackend chr;
    guint watch_tag;

    IbexIRQ irqs[OT_UART_IRQ_NUM];
};

static uint32_t ot_uart_get_tx_watermark_level(OtUARTState *s)
{
    uint32_t tx_ilvl = (s->regs[R_FIFO_CTRL] & R_FIFO_CTRL_TXILVL_MASK) >>
                       R_FIFO_CTRL_TXILVL_SHIFT;

    return tx_ilvl < 7u ? (1u << tx_ilvl) : 64u;
}

static uint32_t ot_uart_get_rx_watermark_level(OtUARTState *s)
{
    uint32_t rx_ilvl = (s->regs[R_FIFO_CTRL] & R_FIFO_CTRL_RXILVL_MASK) >>
                       R_FIFO_CTRL_RXILVL_SHIFT;

    return rx_ilvl < 7u ? (1u << rx_ilvl) : 126u;
}

static void ot_uart_update_irqs(OtUARTState *s)
{
    uint32_t state_masked = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    trace_ot_uart_irqs(s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                       state_masked);

    for (int index = 0; index < OT_UART_IRQ_NUM; index++) {
        bool level = (state_masked & (1U << index)) != 0;
        ibex_irq_set(&s->irqs[index], level);
    }
}

static bool ot_uart_is_sys_loopack_enabled(OtUARTState *s)
{
    return (bool)FIELD_EX32(s->regs[R_CTRL], CTRL, SLPBK);
}

static bool ot_uart_is_tx_enabled(OtUARTState *s)
{
    return (bool)FIELD_EX32(s->regs[R_CTRL], CTRL, TX);
}

static bool ot_uart_is_rx_enabled(OtUARTState *s)
{
    return (bool)FIELD_EX32(s->regs[R_CTRL], CTRL, RX);
}

static void ot_uart_reset_rx_fifo(OtUARTState *s)
{
    fifo8_reset(&s->rx_fifo);
    s->regs[R_INTR_STATE] &= ~INTR_RX_WATERMARK_MASK;
    s->regs[R_INTR_STATE] &= ~INTR_RX_OVERFLOW_MASK;
    if (ot_uart_is_rx_enabled(s) && !ot_uart_is_sys_loopack_enabled(s)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static int ot_uart_can_receive(void *opaque)
{
    OtUARTState *s = opaque;

    if (s->regs[R_CTRL] & R_CTRL_RX_MASK) {
        return fifo8_num_free(&s->rx_fifo);
    }

    return 0;
}

static void ot_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    OtUARTState *s = opaque;
    uint32_t rx_watermark_level;
    size_t count = MIN(fifo8_num_free(&s->rx_fifo), (size_t)size);

    for (int index = 0; index < size; index++) {
        fifo8_push(&s->rx_fifo, buf[index]);
    }

    /* update INTR_STATE */
    if (count != size) {
        s->regs[R_INTR_STATE] |= INTR_RX_OVERFLOW_MASK;
    }
    rx_watermark_level = ot_uart_get_rx_watermark_level(s);
    if (rx_watermark_level && size >= rx_watermark_level) {
        s->regs[R_INTR_STATE] |= INTR_RX_WATERMARK_MASK;
    }

    ot_uart_update_irqs(s);
}

static uint8_t ot_uart_read_rx_fifo(OtUARTState *s)
{
    uint8_t val;

    if (!(s->regs[R_CTRL] & R_CTRL_RX_MASK)) {
        return 0;
    }

    if (fifo8_is_empty(&s->rx_fifo)) {
        return 0;
    }

    val = fifo8_pop(&s->rx_fifo);

    if (ot_uart_is_rx_enabled(s) && !ot_uart_is_sys_loopack_enabled(s)) {
        qemu_chr_fe_accept_input(&s->chr);
    }

    return val;
}

static void ot_uart_reset_tx_fifo(OtUARTState *s)
{
    fifo8_reset(&s->tx_fifo);
    s->regs[R_INTR_STATE] |= INTR_TX_EMPTY_MASK;
    if (s->tx_watermark_level) {
        s->regs[R_INTR_STATE] |= INTR_TX_WATERMARK_MASK;
        s->tx_watermark_level = 0;
    }
}

static void ot_uart_xmit(OtUARTState *s)
{
    const uint8_t *buf;
    uint32_t size;
    int ret;

    if (fifo8_is_empty(&s->tx_fifo)) {
        return;
    }

    if (ot_uart_is_sys_loopack_enabled(s)) {
        /* system loopback mode, just forward to RX FIFO */
        uint32_t count = fifo8_num_used(&s->tx_fifo);
        buf = fifo8_pop_buf(&s->tx_fifo, count, &size);
        ot_uart_receive(s, buf, size);
        count -= size;
        /*
         * there may be more data to send if data wraps around the end of TX
         * FIFO
         */
        if (count) {
            buf = fifo8_pop_buf(&s->tx_fifo, count, &size);
            ot_uart_receive(s, buf, size);
        }
    } else {
        /* instant drain the fifo when there's no back-end */
        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            ot_uart_reset_tx_fifo(s);
            ot_uart_update_irqs(s);
            return;
        }

        /* get a continuous buffer from the FIFO */
        buf = fifo8_peek_buf(&s->tx_fifo, fifo8_num_used(&s->tx_fifo), &size);
        /* send as much as possible */
        ret = qemu_chr_fe_write(&s->chr, buf, size);
        /* if some characters where sent, remove them from the FIFO */
        if (ret >= 0) {
            fifo8_consume_all(&s->tx_fifo, ret);
        }
    }

    /* update INTR_STATE */
    if (fifo8_is_empty(&s->tx_fifo)) {
        s->regs[R_INTR_STATE] |= INTR_TX_EMPTY_MASK;
    }
    if (s->tx_watermark_level &&
        fifo8_num_used(&s->tx_fifo) < s->tx_watermark_level) {
        s->regs[R_INTR_STATE] |= INTR_TX_WATERMARK_MASK;
        s->tx_watermark_level = 0;
    }

    ot_uart_update_irqs(s);
}

static gboolean ot_uart_watch_cb(void *do_not_use, GIOCondition cond,
                                 void *opaque)
{
    OtUARTState *s = opaque;

    s->watch_tag = 0;
    ot_uart_xmit(s);

    return FALSE;
}

static void uart_write_tx_fifo(OtUARTState *s, uint8_t val)
{
    if (fifo8_is_full(&s->tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ot_uart: TX FIFO overflow");
        return;
    }

    fifo8_push(&s->tx_fifo, val);

    s->tx_watermark_level = ot_uart_get_tx_watermark_level(s);
    if (fifo8_num_used(&s->tx_fifo) < s->tx_watermark_level) {
        /*
         * TX watermark interrupt is raised when FIFO depth goes from above
         * watermark to below. If we haven't reached watermark, reset cached
         * watermark level
         */
        s->tx_watermark_level = 0;
    }

    if (ot_uart_is_tx_enabled(s)) {
        ot_uart_xmit(s);
    }
}

static uint64_t ot_uart_read(void *opaque, hwaddr addr, unsigned int size)
{
    OtUARTState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CTRL:
    case R_FIFO_CTRL:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        /* assume that UART always report RXIDLE */
        val32 = R_STATUS_RXIDLE_MASK;
        /* report RXEMPTY or RXFULL */
        switch (fifo8_num_used(&s->rx_fifo)) {
        case 0:
            val32 |= R_STATUS_RXEMPTY_MASK;
            break;
        case OT_UART_RX_FIFO_SIZE:
            val32 |= R_STATUS_RXFULL_MASK;
            break;
        }
        /* report TXEMPTY+TXIDLE or TXFULL */
        switch (fifo8_num_used(&s->tx_fifo)) {
        case 0:
            val32 |= R_STATUS_TXEMPTY_MASK | R_STATUS_TXIDLE_MASK;
            break;
        case OT_UART_TX_FIFO_SIZE:
            val32 |= R_STATUS_TXFULL_MASK;
            break;
        }
        if (!ot_uart_is_tx_enabled(s)) {
            val32 |= R_STATUS_TXIDLE_MASK;
        }
        if (!ot_uart_is_rx_enabled(s)) {
            val32 |= R_STATUS_RXIDLE_MASK;
        }
        break;
    case R_RDATA:
        val32 = (uint32_t)ot_uart_read_rx_fifo(s);
        break;
    case R_FIFO_STATUS:
        val32 =
            (fifo8_num_used(&s->rx_fifo) & 0xffu) << R_FIFO_STATUS_RXLVL_SHIFT;
        val32 |=
            (fifo8_num_used(&s->tx_fifo) & 0xffu) << R_FIFO_STATUS_TXLVL_SHIFT;
        break;
    case R_OVRD:
    case R_VAL:
    case R_TIMEOUT_CTRL:
        val32 = s->regs[reg];
        qemu_log_mask(LOG_UNIMP, "%s: %s is not supported\n", __func__,
                      REG_NAME(reg));
        break;
    case R_ALERT_TEST:
    case R_INTR_TEST:
    case R_WDATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
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
    trace_ot_uart_io_read((unsigned)addr, REG_NAME(reg), (uint64_t)val32, pc);

    return (uint64_t)val32;
}

static void ot_uart_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned int size)
{
    OtUARTState *s = opaque;
    uint32_t val32 = val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_uart_io_write((unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_uart_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_uart_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_uart_update_irqs(s);
        break;
    case R_CTRL:
        if (val32 & ~CTRL_SUP_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: UART_CTRL feature not supported: 0x%08x\n",
                          __func__, val32 & ~CTRL_SUP_MASK);
        }
        uint32_t prev = s->regs[R_CTRL];
        s->regs[R_CTRL] = val32 & CTRL_MASK;
        uint32_t change = prev ^ s->regs[R_CTRL];
        if ((change & R_CTRL_RX_MASK) && ot_uart_is_rx_enabled(s) &&
            !ot_uart_is_sys_loopack_enabled(s)) {
            qemu_chr_fe_accept_input(&s->chr);
        }
        if ((change & R_CTRL_TX_MASK) && ot_uart_is_tx_enabled(s)) {
            /* try sending pending data from TX FIFO if any */
            ot_uart_xmit(s);
        }
        break;
    case R_WDATA:
        uart_write_tx_fifo(s, (uint8_t)(val32 & R_WDATA_WDATA_MASK));
        break;
    case R_FIFO_CTRL:
        s->regs[R_FIFO_CTRL] =
            val32 & (R_FIFO_CTRL_RXILVL_MASK | R_FIFO_CTRL_TXILVL_MASK);
        if (val32 & R_FIFO_CTRL_RXRST_MASK) {
            ot_uart_reset_rx_fifo(s);
            ot_uart_update_irqs(s);
        }
        if (val32 & R_FIFO_CTRL_TXRST_MASK) {
            ot_uart_reset_tx_fifo(s);
            ot_uart_update_irqs(s);
        }
        break;
    case R_OVRD:
        if (val32 & R_OVRD_TXEN_MASK) {
            qemu_log_mask(LOG_UNIMP, "%s: OVRD.TXEN is not supported\n",
                          __func__);
        }
        s->regs[R_OVRD] = val32 & R_OVRD_TXVAL_MASK;
        break;
    case R_TIMEOUT_CTRL:
        s->regs[R_TIMEOUT_CTRL] =
            val32 & (R_TIMEOUT_CTRL_EN_MASK | R_TIMEOUT_CTRL_VAL_MASK);
        break;
    case R_STATUS:
    case R_RDATA:
    case R_FIFO_STATUS:
    case R_VAL:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ot_uart_ops = {
    .read = ot_uart_read,
    .write = ot_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static Property ot_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", OtUARTState, chr),
    DEFINE_PROP_UINT32("pclk", OtUARTState, pclk, 0u),
    DEFINE_PROP_END_OF_LIST(),
};

static int ot_uart_be_change(void *opaque)
{
    OtUARTState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, ot_uart_can_receive, ot_uart_receive,
                             NULL, ot_uart_be_change, s, NULL, true);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             ot_uart_watch_cb, s);
    }

    return 0;
}

static void ot_uart_realize(DeviceState *dev, Error **errp)
{
    OtUARTState *s = OT_UART(dev);

    fifo8_create(&s->tx_fifo, OT_UART_TX_FIFO_SIZE);
    fifo8_create(&s->rx_fifo, OT_UART_RX_FIFO_SIZE);

    qemu_chr_fe_set_handlers(&s->chr, ot_uart_can_receive, ot_uart_receive,
                             NULL, ot_uart_be_change, s, NULL, true);
}

static void ot_uart_reset(DeviceState *dev)
{
    OtUARTState *s = OT_UART(dev);

    memset(&s->regs[0], 0, sizeof(s->regs));

    s->tx_watermark_level = 0;
    for (unsigned index = 0; index < ARRAY_SIZE(s->irqs); index++) {
        ibex_irq_set(&s->irqs[index], 0);
    }
    ot_uart_reset_tx_fifo(s);
    ot_uart_reset_rx_fifo(s);

    ot_uart_update_irqs(s);
}

static void ot_uart_init(Object *obj)
{
    OtUARTState *s = OT_UART(obj);

    for (unsigned index = 0; index < OT_UART_IRQ_NUM; index++) {
        ibex_sysbus_init_irq(obj, &s->irqs[index]);
    }

    memory_region_init_io(&s->mmio, obj, &ot_uart_ops, s, TYPE_OT_UART,
                          REGS_COUNT * sizeof(uint32_t));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void ot_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ot_uart_realize;
    dc->reset = ot_uart_reset;
    device_class_set_props(dc, ot_uart_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo ot_uart_info = {
    .name = TYPE_OT_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtUARTState),
    .instance_init = ot_uart_init,
    .class_init = ot_uart_class_init,
};

static void ot_uart_register_types(void)
{
    type_register_static(&ot_uart_info);
}

type_init(ot_uart_register_types)
