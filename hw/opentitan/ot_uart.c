/*
 * QEMU OpenTitan UART device
 *
 * Copyright (c) 2022-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_TX_WATERMARK, 0u, 1u)
    SHARED_FIELD(INTR_RX_WATERMARK, 1u, 1u)
    SHARED_FIELD(INTR_TX_DONE, 2u, 1u)
    SHARED_FIELD(INTR_RX_OVERFLOW, 3u, 1u)
    SHARED_FIELD(INTR_RX_FRAME_ERR, 4u, 1u)
    SHARED_FIELD(INTR_RX_BREAK_ERR, 5u, 1u)
    SHARED_FIELD(INTR_RX_TIMEOUT, 6u, 1u)
    SHARED_FIELD(INTR_RX_PARITY_ERR, 7u, 1u)
    SHARED_FIELD(INTR_TX_EMPTY, 8u, 1u)
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
    (INTR_TX_WATERMARK_MASK | INTR_RX_WATERMARK_MASK | INTR_TX_DONE_MASK | \
     INTR_RX_OVERFLOW_MASK | INTR_RX_FRAME_ERR_MASK | INTR_RX_BREAK_ERR_MASK | \
     INTR_RX_TIMEOUT_MASK | INTR_RX_PARITY_ERR_MASK | INTR_TX_EMPTY_MASK)

#define CTRL_MASK \
    (R_CTRL_TX_MASK | R_CTRL_RX_MASK | R_CTRL_NF_MASK | R_CTRL_SLPBK_MASK | \
     R_CTRL_LLPBK_MASK | R_CTRL_PARITY_EN_MASK | R_CTRL_PARITY_ODD_MASK | \
     R_CTRL_RXBLVL_MASK | R_CTRL_NCO_MASK)

#define CTRL_SUP_MASK \
    (R_CTRL_RX_MASK | R_CTRL_TX_MASK | R_CTRL_SLPBK_MASK | R_CTRL_NCO_MASK)

#define OT_UART_NCO_BITS     16u
#define OT_UART_TX_FIFO_SIZE 128u
#define OT_UART_RX_FIFO_SIZE 128u
#define OT_UART_IRQ_NUM      9u

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_TIMEOUT_CTRL)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

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
    IbexIRQ irqs[OT_UART_IRQ_NUM];
    IbexIRQ alert;

    uint32_t regs[REGS_COUNT];

    Fifo8 tx_fifo;
    Fifo8 rx_fifo;
    uint32_t tx_watermark_level;
    bool in_break;
    guint watch_tag;
    unsigned pclk; /* Current input clock */
    const char *clock_src_name; /* IRQ name once connected */

    char *ot_id;
    char *clock_name;
    DeviceState *clock_src;
    CharFrontend chr;
    bool oversample_break; /* Should mock break in the oversampled VAL reg? */
    bool toggle_break; /* Are incoming breaks temporary or toggled? */
};

struct OtUARTClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static uint32_t ot_uart_get_tx_watermark_level(const OtUARTState *s)
{
    uint32_t tx_ilvl = (s->regs[R_FIFO_CTRL] & R_FIFO_CTRL_TXILVL_MASK) >>
                       R_FIFO_CTRL_TXILVL_SHIFT;

    return tx_ilvl < 7u ? (1u << tx_ilvl) : 64u;
}

static uint32_t ot_uart_get_rx_watermark_level(const OtUARTState *s)
{
    uint32_t rx_ilvl = (s->regs[R_FIFO_CTRL] & R_FIFO_CTRL_RXILVL_MASK) >>
                       R_FIFO_CTRL_RXILVL_SHIFT;

    return rx_ilvl < 7u ? (1u << rx_ilvl) : 126u;
}

static void ot_uart_update_irqs(OtUARTState *s)
{
    uint32_t state_masked = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    trace_ot_uart_irqs(s->ot_id, s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                       state_masked);

    for (int index = 0; index < OT_UART_IRQ_NUM; index++) {
        bool level = (state_masked & (1U << index)) != 0;
        ibex_irq_set(&s->irqs[index], level);
    }
}

static bool ot_uart_is_sys_loopack_enabled(const OtUARTState *s)
{
    return (bool)FIELD_EX32(s->regs[R_CTRL], CTRL, SLPBK);
}

static bool ot_uart_is_tx_enabled(const OtUARTState *s)
{
    return (bool)FIELD_EX32(s->regs[R_CTRL], CTRL, TX);
}

static bool ot_uart_is_rx_enabled(const OtUARTState *s)
{
    return (bool)FIELD_EX32(s->regs[R_CTRL], CTRL, RX);
}

static void ot_uart_check_baudrate(const OtUARTState *s)
{
    uint32_t nco = FIELD_EX32(s->regs[R_CTRL], CTRL, NCO);

    unsigned baudrate = (unsigned)(((uint64_t)nco * (uint64_t)s->pclk) >>
                                   (R_CTRL_NCO_LENGTH + 4));

    if (baudrate) {
        trace_ot_uart_check_baudrate(s->ot_id, s->pclk, baudrate);
    }
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
        return (int)fifo8_num_free(&s->rx_fifo);
    }

    return 0;
}

static void ot_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    OtUARTState *s = opaque;
    uint32_t rx_watermark_level;
    size_t count = MIN(fifo8_num_free(&s->rx_fifo), (size_t)size);

    if (size && !s->toggle_break) {
        /* no longer breaking, so emulate idle in oversampled VAL register */
        s->in_break = false;
    }

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

static void ot_uart_event_handler(void *opaque, QEMUChrEvent event)
{
    OtUARTState *s = opaque;

    if (event == CHR_EVENT_BREAK) {
        if (!s->in_break || !s->oversample_break) {
            /* ignore CTRL.RXBLVL as we have no notion of break "time" */
            s->regs[R_INTR_STATE] |= INTR_RX_BREAK_ERR_MASK;
            ot_uart_update_irqs(s);
            /* emulate break in the oversampled VAL register */
            s->in_break = true;
        } else if (s->toggle_break) {
            /* emulate toggling break off in the oversampled VAL register */
            s->in_break = false;
        }
    }
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
    s->regs[R_INTR_STATE] |= INTR_TX_DONE_MASK;
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
        buf = fifo8_pop_bufptr(&s->tx_fifo, count, &size);
        ot_uart_receive(s, buf, (int)size);
        count -= size;
        /*
         * there may be more data to send if data wraps around the end of TX
         * FIFO
         */
        if (count) {
            buf = fifo8_pop_bufptr(&s->tx_fifo, count, &size);
            ot_uart_receive(s, buf, (int)size);
        }
    } else {
        /* instant drain the fifo when there's no back-end */
        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            ot_uart_reset_tx_fifo(s);
            ot_uart_update_irqs(s);
            return;
        }

        /* get a continuous buffer from the FIFO */
        buf =
            fifo8_peek_bufptr(&s->tx_fifo, fifo8_num_used(&s->tx_fifo), &size);
        /* send as much as possible */
        ret = qemu_chr_fe_write(&s->chr, buf, (int)size);
        /* if some characters where sent, remove them from the FIFO */
        if (ret >= 0) {
            fifo8_drop(&s->tx_fifo, ret);
        }
    }

    /* update INTR_STATE */
    if (fifo8_is_empty(&s->tx_fifo)) {
        s->regs[R_INTR_STATE] |= INTR_TX_EMPTY_MASK;
        s->regs[R_INTR_STATE] |= INTR_TX_DONE_MASK;
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
    (void)do_not_use;
    (void)cond;

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

static void ot_uart_clock_input(void *opaque, int irq, int level)
{
    OtUARTState *s = opaque;

    g_assert(irq == 0);

    s->pclk = (unsigned)level;

    /* TODO: disable UART transfer when PCLK is 0 */
    ot_uart_check_baudrate(s);
}

static uint64_t ot_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    OtUARTState *s = opaque;
    (void)size;
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
        default:
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
        default:
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
    case R_VAL:
        /*
         * This is not trivially implemented due to the QEMU UART
         * interface. There is no way to reliably sample or oversample
         * given our emulated interface, but some software might poll the
         * value of this register to determine break conditions.
         *
         * As such, default to reporting 16 of the last sample received
         * instead. This defaults to 16 idle high samples (as a stop bit is
         * always the last received), except for when the `oversample-break`
         * property is set and a break condition is received over UART RX,
         * where we then show 16 low samples until the next valid UART
         * transmission is received (or break is toggled off with the
         * `toggle-break` property enabled). This will not be accurate, but
         * should be sufficient to support basic software flows that
         * essentially use UART break as a strapping mechanism.
         */
        val32 = (s->in_break && s->oversample_break) ? 0u : UINT16_MAX;
        qemu_log_mask(LOG_UNIMP, "%s: VAL only shows idle%s\n", __func__,
                      (s->oversample_break ? "/break" : ""));
        break;
    case R_OVRD:
    case R_TIMEOUT_CTRL:
        val32 = s->regs[reg];
        qemu_log_mask(LOG_UNIMP, "%s: %s is not supported\n", __func__,
                      REG_NAME(reg));
        break;
    case R_ALERT_TEST:
    case R_INTR_TEST:
    case R_WDATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_uart_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                              pc);

    return (uint64_t)val32;
}

static void ot_uart_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned size)
{
    OtUARTState *s = opaque;
    (void)size;
    uint32_t val32 = val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_uart_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

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
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        s->regs[reg] = val32;
        ibex_irq_set(&s->alert, (int)(bool)val32);
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
        if (change & R_CTRL_NCO_MASK) {
            ot_uart_check_baudrate(s);
        }
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
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
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

static const Property ot_uart_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtUARTState, ot_id),
    DEFINE_PROP_CHR("chardev", OtUARTState, chr),
    DEFINE_PROP_STRING("clock-name", OtUARTState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtUARTState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_BOOL("oversample-break", OtUARTState, oversample_break, false),
    DEFINE_PROP_BOOL("toggle-break", OtUARTState, toggle_break, false),
};

static int ot_uart_be_change(void *opaque)
{
    OtUARTState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, ot_uart_can_receive, ot_uart_receive,
                             ot_uart_event_handler, ot_uart_be_change, s, NULL,
                             true);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             ot_uart_watch_cb, s);
    }

    return 0;
}

static void ot_uart_reset_enter(Object *obj, ResetType type)
{
    OtUARTClass *c = OT_UART_GET_CLASS(obj);
    OtUARTState *s = OT_UART(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    memset(&s->regs[0], 0, sizeof(s->regs));

    s->tx_watermark_level = 0;
    for (unsigned index = 0; index < ARRAY_SIZE(s->irqs); index++) {
        ibex_irq_set(&s->irqs[index], 0);
    }
    ot_uart_reset_tx_fifo(s);
    ot_uart_reset_rx_fifo(s);

    /*
     * do not reset `s->in_break`, as that tracks whether we are currently
     * receiving a break condition over UART RX from some device talking
     * to OpenTitan, which should survive resets. The QEMU CharDev only
     * supports transient break events and not the notion of holding the
     * UART in break, so remembering breaks like this is required to
     * support mocking of break conditions in the oversampled `VAL` reg.
     */
    if (s->in_break) {
        /* ignore CTRL.RXBLVL as we have no notion of break "time" */
        s->regs[R_INTR_STATE] |= INTR_RX_BREAK_ERR_MASK;
    }

    ot_uart_update_irqs(s);
    ibex_irq_set(&s->alert, 0);

    if (!s->clock_src_name) {
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_name =
            ic->get_clock_source(ii, s->clock_name, DEVICE(s), &error_fatal);
        qemu_irq in_irq = qdev_get_gpio_in_named(DEVICE(s), "clock-in", 0);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_name, 0, in_irq);
        trace_ot_uart_connect_input_clock(s->ot_id, s->clock_src_name);
    }
}

static void ot_uart_realize(DeviceState *dev, Error **errp)
{
    OtUARTState *s = OT_UART(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    qdev_init_gpio_in_named(DEVICE(s), &ot_uart_clock_input, "clock-in", 1);

    fifo8_create(&s->tx_fifo, OT_UART_TX_FIFO_SIZE);
    fifo8_create(&s->rx_fifo, OT_UART_RX_FIFO_SIZE);

    qemu_chr_fe_set_handlers(&s->chr, ot_uart_can_receive, ot_uart_receive,
                             ot_uart_event_handler, ot_uart_be_change, s, NULL,
                             true);
}

static void ot_uart_init(Object *obj)
{
    OtUARTState *s = OT_UART(obj);

    for (unsigned index = 0; index < OT_UART_IRQ_NUM; index++) {
        ibex_sysbus_init_irq(obj, &s->irqs[index]);
    }
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    memory_region_init_io(&s->mmio, obj, &ot_uart_ops, s, TYPE_OT_UART,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void ot_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = ot_uart_realize;
    device_class_set_props(dc, ot_uart_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtUARTClass *uc = OT_UART_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_uart_reset_enter, NULL, NULL,
                                       &uc->parent_phases);
}

static const TypeInfo ot_uart_info = {
    .name = TYPE_OT_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtUARTState),
    .instance_init = ot_uart_init,
    .class_size = sizeof(OtUARTClass),
    .class_init = ot_uart_class_init,
};

static void ot_uart_register_types(void)
{
    type_register_static(&ot_uart_info);
}

type_init(ot_uart_register_types);
