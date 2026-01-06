/*
 * I2C Host Proxy Device
 *
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Alice Ziuziakowska <a.ziuziakowska@lowrisc.org>
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
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "chardev/char-fe.h"
#include "hw/i2c/i2c.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_i2c_host_proxy.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "trace.h"

typedef enum {
    /* Initial state, wait for 'i' followed by version byte */
    CMD_I2C_INIT,
    /* Waiting for version byte */
    CMD_I2C_VERSION,
    /* bus idle state before transaction start */
    CMD_I2C_BUS_IDLE,
    /* Waiting for address + r/w byte */
    CMD_I2C_START,
    CMD_I2C_REPEATED_START,
    /* read transaction - waiting to read byte */
    CMD_I2C_READ,
    /* write transaction - waiting to write byte */
    CMD_I2C_WRITE,
    /* write transaction - waiting for byte to write */
    CMD_I2C_WRITE_PAYLOAD,
    /* parse error state */
    CMD_I2C_ERR,
} CmdParserState;

#define I2C_PROXY_PROTO_VERSION 0x01u

#define I2C_PROXY_STALL_NS 100000u /* 100us */

struct OtI2CHostProxyState {
    I2CSlave parent_obj;
    I2CBus *bus;

    /* chardev i2c command parser state */
    CmdParserState parser_state;

    /* Saved target address of transaction for repeated start conditions */
    uint8_t address;

    /*
     * Timer to stall chardev I/O to return control back to the vCPU, so
     * that OT can process the I2C transaction
     */
    QEMUTimer *stall_timer;

    /* device change tracker */
    guint watch_tag;

    CharFrontend chr;
};

struct OtI2CHostProxyClass {
    I2CSlaveClass parent_class;
};

static void ot_i2c_host_proxy_put_byte(OtI2CHostProxyState *s, uint8_t data)
{
    qemu_chr_fe_write(&s->chr, &data, 1u);
}

static void ot_i2c_host_proxy_put_nack(OtI2CHostProxyState *s)
{
    ot_i2c_host_proxy_put_byte(s, (uint8_t)'!');
}

static void ot_i2c_host_proxy_put_ack(OtI2CHostProxyState *s)
{
    ot_i2c_host_proxy_put_byte(s, (uint8_t)'.');
}

/*
 * Stall the chardev to slow down I2C transaction processing, so that QEMU can
 * return control to the vCPU to process and prepare response data for OT I2C.
 */
static void ot_i2c_host_proxy_stall(OtI2CHostProxyState *s)
{
    uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    timer_mod(s->stall_timer, (int64_t)(now + I2C_PROXY_STALL_NS));
}

static void
ot_i2c_host_proxy_start_transfer(OtI2CHostProxyState *s, bool read_transfer)
{
    if (i2c_start_transfer(s->bus, s->address, read_transfer) != 0) {
        ot_i2c_host_proxy_put_nack(s);
    } else {
        ot_i2c_host_proxy_put_ack(s);
    }
    ot_i2c_host_proxy_stall(s);
}

static void ot_i2c_host_proxy_do_read(OtI2CHostProxyState *s)
{
    ot_i2c_host_proxy_put_byte(s, i2c_recv(s->bus));
    ot_i2c_host_proxy_stall(s);
}

static void ot_i2c_host_proxy_do_write(OtI2CHostProxyState *s, uint8_t data)
{
    if (i2c_send(s->bus, data) != 0) {
        ot_i2c_host_proxy_put_nack(s);
    } else {
        ot_i2c_host_proxy_put_ack(s);
    }
    ot_i2c_host_proxy_stall(s);
}

static void ot_i2c_host_proxy_parse_error(OtI2CHostProxyState *s)
{
    s->parser_state = CMD_I2C_ERR;
    ot_i2c_host_proxy_put_byte(s, (uint8_t)'x');
}

static void ot_i2c_host_proxy_command_byte(OtI2CHostProxyState *s, uint8_t byte)
{
    switch (s->parser_state) {
    case CMD_I2C_INIT:
        if (byte == 'i') {
            s->parser_state = CMD_I2C_VERSION;
            break;
        }
        s->parser_state = CMD_I2C_ERR;
        break;
    case CMD_I2C_VERSION:
        if (byte == I2C_PROXY_PROTO_VERSION) {
            ot_i2c_host_proxy_put_ack(s);
            s->parser_state = CMD_I2C_BUS_IDLE;
            break;
        }
        ot_i2c_host_proxy_put_nack(s);
        s->parser_state = CMD_I2C_ERR;
        break;
    case CMD_I2C_BUS_IDLE:
        if (byte == 'S') {
            s->parser_state = CMD_I2C_START;
            break;
        }
        s->parser_state = CMD_I2C_ERR;
        break;
    case CMD_I2C_START:
    case CMD_I2C_REPEATED_START:
        /*
         * If a transaction is already in progress (repeated start), QEMU does
         * not re-scan the bus to find devices even if the provided address
         * is different, so we do not bother storing it here as the address
         * that the transaction was started with will be used anyway.
         */
        if (s->parser_state == CMD_I2C_START) {
            s->address = (byte >> 1u);
        }
        bool read_transfer = (byte & 1u) == 1u;
        ot_i2c_host_proxy_start_transfer(s, read_transfer);
        s->parser_state = read_transfer ? CMD_I2C_READ : CMD_I2C_WRITE;
        break;
    case CMD_I2C_READ:
        if (byte == 'R') {
            ot_i2c_host_proxy_do_read(s);
            break;
        }
        if (byte == 'P') {
            s->parser_state = CMD_I2C_INIT;
            i2c_end_transfer(s->bus);
            break;
        }
        if (byte == 'S') {
            s->parser_state = CMD_I2C_REPEATED_START;
            break;
        }
        s->parser_state = CMD_I2C_ERR;
        i2c_end_transfer(s->bus);
        ot_i2c_host_proxy_parse_error(s);
        break;
    case CMD_I2C_WRITE:
        if (byte == 'W') {
            s->parser_state = CMD_I2C_WRITE_PAYLOAD;
            break;
        }
        if (byte == 'P') {
            s->parser_state = CMD_I2C_INIT;
            i2c_end_transfer(s->bus);
            break;
        }
        if (byte == 'S') {
            s->parser_state = CMD_I2C_REPEATED_START;
            break;
        }
        s->parser_state = CMD_I2C_ERR;
        i2c_end_transfer(s->bus);
        ot_i2c_host_proxy_parse_error(s);
        break;
    case CMD_I2C_WRITE_PAYLOAD:
        ot_i2c_host_proxy_do_write(s, byte);
        s->parser_state = CMD_I2C_WRITE;
        break;
    case CMD_I2C_ERR:
        break;
    default:
        g_assert_not_reached();
    }
}

static void ot_i2c_host_proxy_timer(void *opaque)
{
    (void)opaque;
}

static int ot_i2c_host_proxy_can_receive(void *opaque)
{
    OtI2CHostProxyState *s = OT_I2C_HOST_PROXY(opaque);
    if (s->parser_state == CMD_I2C_ERR) {
        return 0;
    }
    return timer_pending(s->stall_timer) ? 0 : 1;
}

static void ot_i2c_host_proxy_receive(void *opaque, const uint8_t *buf,
                                      int size)
{
    OtI2CHostProxyState *s = OT_I2C_HOST_PROXY(opaque);
    g_assert(size == 1);
    ot_i2c_host_proxy_command_byte(s, *buf);
}

static void ot_i2c_host_proxy_event_handler(void *opaque, QEMUChrEvent event)
{
    OtI2CHostProxyState *s = OT_I2C_HOST_PROXY(opaque);

    if (event == CHR_EVENT_CLOSED) {
        switch (s->parser_state) {
        case CMD_I2C_READ:
        case CMD_I2C_WRITE:
        case CMD_I2C_WRITE_PAYLOAD:
            i2c_end_transfer(s->bus);
            break;
        default:
            break;
        }
        s->parser_state = CMD_I2C_INIT;
        timer_del(s->stall_timer);
    }
}

static int ot_i2c_host_proxy_chr_watch_cb(void *do_not_use, GIOCondition cond,
                                          void *opaque)
{
    OtI2CHostProxyState *s = OT_I2C_HOST_PROXY(opaque);
    (void)do_not_use;
    (void)cond;
    s->watch_tag = 0;
    return 0;
}

static int ot_i2c_host_proxy_be_change(void *opaque)
{
    OtI2CHostProxyState *s = OT_I2C_HOST_PROXY(opaque);

    qemu_chr_fe_set_handlers(&s->chr, ot_i2c_host_proxy_can_receive,
                             ot_i2c_host_proxy_receive,
                             ot_i2c_host_proxy_event_handler,
                             ot_i2c_host_proxy_be_change, s, NULL, true);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag =
            /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
            qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                  &ot_i2c_host_proxy_chr_watch_cb, s);
    }

    return 0;
}

static void ot_i2c_host_proxy_realize(DeviceState *dev, Error **errp)
{
    (void)errp;
    OtI2CHostProxyState *state = OT_I2C_HOST_PROXY(dev);
    BusState *bus = qdev_get_parent_bus(dev);
    state->bus = I2C_BUS(bus);
    qemu_chr_fe_set_handlers(&state->chr, ot_i2c_host_proxy_can_receive,
                             ot_i2c_host_proxy_receive, NULL,
                             ot_i2c_host_proxy_be_change, state, NULL, true);
}

static void ot_i2c_host_proxy_init(Object *obj)
{
    OtI2CHostProxyState *s = OT_I2C_HOST_PROXY(obj);
    s->parser_state = CMD_I2C_INIT;
    s->stall_timer =
        timer_new_ns(OT_VIRTUAL_CLOCK, &ot_i2c_host_proxy_timer, (void *)s);
}

static const Property ot_i2c_host_proxy_properties[] = {
    DEFINE_PROP_CHR("chardev", OtI2CHostProxyState, chr),
};

/*
 * This device does not support being a target device, and does not respond to
 * transactions. Overwrite the default `match_and_add` implementation to
 * always not match any address, even for a broadcast.
 */
static bool ot_i2c_host_proxy_match_and_add(
    I2CSlave *s, uint8_t address, bool broadcast, I2CNodeList *current_devs)
{
    (void)s;
    (void)address;
    (void)broadcast;
    (void)current_devs;
    return false;
}

static void ot_i2c_host_proxy_class_init(ObjectClass *klass, const void *data)
{
    (void)data;
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "I2C Host Proxy";
    dc->realize = ot_i2c_host_proxy_realize;
    device_class_set_props(dc, ot_i2c_host_proxy_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    sc->match_and_add = &ot_i2c_host_proxy_match_and_add;
}

static const TypeInfo ot_i2c_host_proxy = {
    .name = TYPE_OT_I2C_HOST_PROXY,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(OtI2CHostProxyState),
    .instance_init = ot_i2c_host_proxy_init,
    .class_init = ot_i2c_host_proxy_class_init,
    .class_size = sizeof(OtI2CHostProxyClass),
};

static void register_types(void)
{
    type_register_static(&ot_i2c_host_proxy);
}

type_init(register_types);
