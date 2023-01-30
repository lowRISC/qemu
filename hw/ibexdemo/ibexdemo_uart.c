/*
 * QEMU lowRISC Ibex Demo UART device
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/ibexdemo/ibexdemo_uart.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"

/* clang-format off */
REG32(RX, 0x00u)
    FIELD(RX, DATA, 0u, 8u)
REG32(TX, 0x04u)
    FIELD(TX, DATA, 0u, 8u)
REG32(STATUS, 0x08u)
    FIELD(STATUS, RX_FIFO_EMPTY, 0u, 1u)
    FIELD(STATUS, TX_FIFO_FULL, 1u, 1u)
/* clang-format on */

#define IBEXDEMO_UART_RX_FIFO_SIZE 128u
#define IBEXDEMO_UART_TX_FIFO_SIZE 128u

struct IbexDemoUARTState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irq;

    CharBackend chr;
    guint watch_tag;

    Fifo8 rx_fifo;
    Fifo8 tx_fifo;
    bool irq_level;
};

static void ibexdemo_uart_update_irq(IbexDemoUARTState *s)
{
    bool irq_level = !fifo8_is_empty(&s->rx_fifo);
    ibex_irq_set(&s->irq, irq_level);
}

static int ibexdemo_uart_can_receive(void *opaque)
{
    IbexDemoUARTState *s = opaque;

    return (int)fifo8_num_free(&s->rx_fifo);
}

static void ibexdemo_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    IbexDemoUARTState *s = opaque;

    while (size && !fifo8_is_full(&s->rx_fifo)) {
        fifo8_push(&s->rx_fifo, *buf++);
        size--;
    }

    if (size) {
        qemu_log_mask(LOG_GUEST_ERROR, "ibexdemo_uart: RX FIFO overflow");
    }

    ibexdemo_uart_update_irq(s);
}

static void ibexdemo_uart_xmit(IbexDemoUARTState *s)
{
    /* drain the fifo when there's no back-end */
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        fifo8_reset(&s->tx_fifo);
        return;
    }

    const uint8_t *buf;
    uint32_t size;
    int ret;

    buf = fifo8_peek_buf(&s->tx_fifo, fifo8_num_used(&s->tx_fifo), &size);
    ret = qemu_chr_fe_write(&s->chr, buf, size);

    if (ret > 0) {
        fifo8_consume_all(&s->tx_fifo, ret);
    }
}

static gboolean ibexdemo_uart_watch_cb(void *do_not_use, GIOCondition cond,
                                       void *opaque)
{
    IbexDemoUARTState *s = opaque;

    s->watch_tag = 0;
    ibexdemo_uart_xmit(s);

    return FALSE;
}

static void ibexdemo_uart_tx_write(IbexDemoUARTState *s, uint8_t val)
{
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }

    if (fifo8_is_full(&s->tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ibexdemo_uart: TX FIFO overflow");
        return;
    }

    fifo8_push(&s->tx_fifo, val);

    ibexdemo_uart_xmit(s);
}

static void ibexdemo_uart_reset(DeviceState *dev)
{
    IbexDemoUARTState *s = IBEXDEMO_UART(dev);

    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->tx_fifo);

    ibexdemo_uart_update_irq(s);

    qemu_chr_fe_accept_input(&s->chr);
}

static uint64_t ibexdemo_uart_read(void *opaque, hwaddr addr, unsigned int size)
{
    IbexDemoUARTState *s = opaque;
    uint32_t val32;
    bool rx_full;

    switch (addr >> 2u) {
    case R_RX:
        rx_full = fifo8_is_full(&s->rx_fifo);
        if (!fifo8_is_empty(&s->rx_fifo)) {
            val32 = fifo8_pop(&s->rx_fifo);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "ibexdemo_uart: RX FIFO underflow");
            val32 = 0;
        }
        if (rx_full) {
            /*
             * RX was full and was not accepting any new input; now that one
             * byte has been popped out, get ready to receive more
             */
            qemu_chr_fe_accept_input(&s->chr);
        }
        ibexdemo_uart_update_irq(s);
        break;
    case R_TX:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: wdata is write only\n", __func__);
        val32 = 0;
        break;
    case R_STATUS:
        val32 = FIELD_DP32(0, STATUS, RX_FIFO_EMPTY,
                           (uint32_t)fifo8_is_empty(&s->rx_fifo));
        val32 = FIELD_DP32(val32, STATUS, TX_FIFO_FULL,
                           (uint32_t)fifo8_is_full(&s->tx_fifo));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    return (uint64_t)val32;
}

static void ibexdemo_uart_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned int size)
{
    IbexDemoUARTState *s = opaque;

    switch (addr >> 2u) {
    case R_RX:
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: reg is read only\n", __func__);
        break;
    case R_TX:
        ibexdemo_uart_tx_write(s, (uint8_t)val64);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ibexdemo_uart_ops = {
    .read = &ibexdemo_uart_read,
    .write = &ibexdemo_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static Property ibexdemo_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", IbexDemoUARTState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static int ibexdemo_uart_be_change(void *opaque)
{
    IbexDemoUARTState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, &ibexdemo_uart_can_receive,
                             &ibexdemo_uart_receive, NULL,
                             &ibexdemo_uart_be_change, s, NULL, true);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             ibexdemo_uart_watch_cb, s);
    }

    return 0;
}

static void ibexdemo_uart_realize(DeviceState *dev, Error **errp)
{
    IbexDemoUARTState *s = IBEXDEMO_UART(dev);

    fifo8_create(&s->tx_fifo, IBEXDEMO_UART_TX_FIFO_SIZE);

    qemu_chr_fe_set_handlers(&s->chr, &ibexdemo_uart_can_receive,
                             &ibexdemo_uart_receive, NULL,
                             &ibexdemo_uart_be_change, s, NULL, true);
}

static void ibexdemo_uart_init(Object *obj)
{
    IbexDemoUARTState *s = IBEXDEMO_UART(obj);

    ibex_sysbus_init_irq(obj, &s->irq);

    memory_region_init_io(&s->mmio, obj, &ibexdemo_uart_ops, s,
                          TYPE_IBEXDEMO_UART, 0x1000u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    fifo8_create(&s->rx_fifo, IBEXDEMO_UART_RX_FIFO_SIZE);
    fifo8_create(&s->tx_fifo, IBEXDEMO_UART_TX_FIFO_SIZE);
}

static void ibexdemo_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ibexdemo_uart_reset;
    dc->realize = &ibexdemo_uart_realize;
    device_class_set_props(dc, ibexdemo_uart_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo ibexdemo_uart_info = {
    .name = TYPE_IBEXDEMO_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoUARTState),
    .instance_init = &ibexdemo_uart_init,
    .class_init = &ibexdemo_uart_class_init,
};

static void ibexdemo_uart_register_types(void)
{
    type_register_static(&ibexdemo_uart_info);
}

type_init(ibexdemo_uart_register_types);
