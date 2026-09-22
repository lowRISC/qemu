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
#include <termios.h>
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "exec/icount.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "io/channel-file.h"
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

/*
 * Status-type interrupts (tx_watermark, rx_watermark, tx_empty per uart.hjson).
 * Unlike event-type interrupts, their INTR_STATE bits are read-only (ro): the
 * hardware drives them from the live FIFO condition every cycle rather than
 * latching, so INTR_STATE writes do not affect them and they de-assert
 * automatically once the condition clears (prim_intr_hw.sv, IntrT="Status").
 */
#define INTR_STATUS_MASK \
    (INTR_TX_WATERMARK_MASK | INTR_RX_WATERMARK_MASK | INTR_TX_EMPTY_MASK)

#define CTRL_MASK \
    (R_CTRL_TX_MASK | R_CTRL_RX_MASK | R_CTRL_NF_MASK | R_CTRL_SLPBK_MASK | \
     R_CTRL_LLPBK_MASK | R_CTRL_PARITY_EN_MASK | R_CTRL_PARITY_ODD_MASK | \
     R_CTRL_RXBLVL_MASK | R_CTRL_NCO_MASK)

#define CTRL_SUP_MASK \
    (R_CTRL_RX_MASK | R_CTRL_TX_MASK | R_CTRL_SLPBK_MASK | R_CTRL_LLPBK_MASK | \
     R_CTRL_PARITY_EN_MASK | R_CTRL_PARITY_ODD_MASK | R_CTRL_RXBLVL_MASK | \
     R_CTRL_NCO_MASK)

/* FIFO depths match uart_reg_pkg.sv (TxFifoDepth=32, RxFifoDepth=64). */
#define OT_UART_NCO_BITS             16u
#define OT_UART_TX_FIFO_SIZE         32u
#define OT_UART_RX_FIFO_SIZE         64u
#define OT_UART_RX_STAGING_FIFO_SIZE 2048u
#define OT_UART_IRQ_NUM              9u

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

    OtUARTState *tx_chr_owner;
    OtUARTState *rx_target_uart;
    QEMUTimer *tx_timer;
    bool tx_busy;
    QEMUTimer *rx_timer;
    Fifo8 rx_staging_fifo;
    QEMUTimer *rx_pace_timer;
    QEMUTimer *rx_timeout_timer;
    int64_t last_rx_ns;
    uint32_t rx_poll_count;
};

typedef struct {
    Chardev parent;
    QIOChannel *ioc;
} OtPtyChardev;

static OtUARTState *ot_uart_instances[4];

struct OtUARTClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static uint32_t ot_uart_get_tx_watermark_level(const OtUARTState *s)
{
    uint32_t tx_ilvl = (s->regs[R_FIFO_CTRL] & R_FIFO_CTRL_TXILVL_MASK) >>
                       R_FIFO_CTRL_TXILVL_SHIFT;

    /*
     * Power-of-two thresholds, matching uart_core.sv. TxFifoDepthW == 6 for a
     * 32-entry FIFO, so the threshold saturates at half the FIFO depth (16)
     * once txilvl >= TxFifoDepthW - 2 == 4.
     */
    if (tx_ilvl >= 4u) {
        return OT_UART_TX_FIFO_SIZE / 2u;
    }
    return 1u << tx_ilvl;
}

static uint32_t ot_uart_get_rx_watermark_level(const OtUARTState *s)
{
    uint32_t rx_ilvl = (s->regs[R_FIFO_CTRL] & R_FIFO_CTRL_RXILVL_MASK) >>
                       R_FIFO_CTRL_RXILVL_SHIFT;

    /*
     * Power-of-two thresholds, matching uart_core.sv. RxFifoDepthW == 7 for a
     * 64-entry FIFO: rxilvl 6 saturates at RxFifoDepth - 2 (62), and rxilvl 7
     * selects a threshold the FIFO can never reach (2*RxFifoDepth - 1), which
     * disables the interrupt.
     */
    if (rx_ilvl > 6u) {
        return OT_UART_RX_FIFO_SIZE * 2u - 1u;
    }
    if (rx_ilvl == 6u) {
        return OT_UART_RX_FIFO_SIZE - 2u;
    }
    return 1u << rx_ilvl;
}

/*
 * Compute the live values of the status-type interrupt conditions from the
 * current FIFO state. These bits are not stored in regs[R_INTR_STATE]; they are
 * recomputed on every INTR_STATE read and IRQ update so that the interrupt
 * tracks the hardware condition (matching prim_intr_hw IntrT="Status").
 */
static uint32_t ot_uart_status_intr_bits(OtUARTState *s)
{
    uint32_t bits = 0;

    /* rx_watermark: asserted while RX FIFO fill level >= threshold. */
    if ((uint32_t)fifo8_num_used(&s->rx_fifo) >=
        ot_uart_get_rx_watermark_level(s)) {
        bits |= INTR_RX_WATERMARK_MASK;
    }
    /* tx_watermark: asserted while TX FIFO has drained below the threshold. */
    if ((uint32_t)fifo8_num_used(&s->tx_fifo) <
        ot_uart_get_tx_watermark_level(s)) {
        bits |= INTR_TX_WATERMARK_MASK;
    }
    /* tx_empty: asserted while the TX FIFO is empty. */
    if (fifo8_is_empty(&s->tx_fifo)) {
        bits |= INTR_TX_EMPTY_MASK;
    }

    return bits;
}

/*
 * The architectural INTR_STATE value: latched event-type bits (held in
 * regs[R_INTR_STATE]) combined with the live status-type bits.
 */
static uint32_t ot_uart_intr_state(OtUARTState *s)
{
    return (s->regs[R_INTR_STATE] & ~INTR_STATUS_MASK) |
           ot_uart_status_intr_bits(s) |
           (s->regs[R_INTR_TEST] & INTR_STATUS_MASK);
}

static void ot_uart_update_irqs(OtUARTState *s)
{
    uint32_t state = ot_uart_intr_state(s);
    uint32_t state_masked = state & s->regs[R_INTR_ENABLE];

    trace_ot_uart_irqs(s->ot_id, state, s->regs[R_INTR_ENABLE], state_masked);

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

static CharFrontend *ot_uart_get_tx_chr(OtUARTState *s)
{
    if (s->tx_chr_owner) {
        return &s->tx_chr_owner->chr;
    }
    return &s->chr;
}

static CharFrontend *ot_uart_get_rx_chr(OtUARTState *s)
{
    for (int i = 0; i < 4; i++) {
        if (ot_uart_instances[i] && ot_uart_instances[i]->rx_target_uart == s) {
            return &ot_uart_instances[i]->chr;
        }
    }
    return &s->chr;
}

void ot_uart_update_pinmux(const uint32_t *mio_outsel,
                           const uint32_t *mio_periph_insel)
{
    static const unsigned out_pads[4] = { 26u, 14u, 5u, 1u };
    static const unsigned in_sels[4] = { 27u, 15u, 6u, 2u };

    for (int u = 0; u < 4; u++) {
        OtUARTState *s = ot_uart_instances[u];
        if (s) {
            OtUARTState *owner = NULL;
            for (int ch = 0; ch < 4; ch++) {
                if (mio_outsel[out_pads[ch]] == (uint32_t)(45 + u)) {
                    owner = ot_uart_instances[ch];
                    break;
                }
            }
            if (!owner && mio_outsel[out_pads[u]] <= 2u) {
                owner = s;
            }
            s->tx_chr_owner = owner;
        }
    }

    for (int ch = 0; ch < 4; ch++) {
        OtUARTState *ch_dev = ot_uart_instances[ch];
        if (ch_dev) {
            OtUARTState *target = NULL;
            for (int u = 0; u < 4; u++) {
                if (mio_periph_insel[42 + u] == in_sels[ch]) {
                    target = ot_uart_instances[u];
                    break;
                }
            }
            if (!target && mio_periph_insel[42 + ch] <= 1u &&
                mio_outsel[out_pads[ch]] <= 2u) {
                target = ch_dev;
            }
            ch_dev->rx_target_uart = target;
        }
    }

    for (int ch = 0; ch < 4; ch++) {
        OtUARTState *ch_dev = ot_uart_instances[ch];
        if (ch_dev && ch_dev->rx_target_uart) {
            OtUARTState *target = ch_dev->rx_target_uart;
            if ((ot_uart_is_rx_enabled(target) ||
                 (target->regs[R_CTRL] & R_CTRL_LLPBK_MASK)) &&
                !ot_uart_is_sys_loopack_enabled(target)) {
                qemu_chr_fe_accept_input(&ch_dev->chr);
            }
        }
    }
}

static bool ot_uart_check_pty_parity(OtUARTState *s, OtUARTState *rx_owner)
{
    if (!(s->regs[R_CTRL] & R_CTRL_PARITY_EN_MASK)) {
        return true;
    }
    if (!rx_owner || !rx_owner->chr.chr || !rx_owner->chr.chr->filename) {
        return true;
    }
    if (!g_str_has_prefix(rx_owner->chr.chr->filename, "pty:")) {
        return true;
    }
    OtPtyChardev *pty = (OtPtyChardev *)rx_owner->chr.chr;
    int master_fd =
        (pty->ioc &&
         object_dynamic_cast(OBJECT(pty->ioc), TYPE_QIO_CHANNEL_FILE)) ?
            QIO_CHANNEL_FILE(pty->ioc)->fd :
            -1;
    if (master_fd >= 0) {
        struct termios tio;
        if (tcgetattr(master_fd, &tio) == 0) {
            if ((tio.c_cflag & PARENB) || (tio.c_iflag & INPCK)) {
                bool host_odd = (tio.c_cflag & PARODD) != 0;
                bool dev_odd = (s->regs[R_CTRL] & R_CTRL_PARITY_ODD_MASK) != 0;
                if (host_odd != dev_odd) {
                    return false;
                }
            }
        }
    }
    return true;
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

static int64_t ot_uart_char_ns(const OtUARTState *s);

static void ot_uart_update_val(OtUARTState *s)
{
    if ((ot_uart_is_tx_enabled(s) || ot_uart_is_rx_enabled(s)) &&
        FIELD_EX32(s->regs[R_CTRL], CTRL, NCO) != 0) {
        s->regs[R_VAL] = UINT16_MAX;
    }
}

static void ot_uart_update_rx_timeout(OtUARTState *s)
{
    if (!s->rx_timeout_timer) {
        return;
    }
    if (s->regs[R_TIMEOUT_CTRL] & R_TIMEOUT_CTRL_EN_MASK) {
        uint32_t val = FIELD_EX32(s->regs[R_TIMEOUT_CTRL], TIMEOUT_CTRL, VAL);
        if (val == 0u) {
            /*
             * In uart_core.sv, event_rx_timeout = (rx_timeout_count_q ==
             * uart_rxto_val) & uart_rxto_en. Since rx_timeout_count_q is 0 when
             * idle/empty, enabling TIMEOUT_CTRL with VAL == 0 immediately
             * asserts INTR_STATE.RX_TIMEOUT.
             */
            s->regs[R_INTR_STATE] |= INTR_RX_TIMEOUT_MASK;
            ot_uart_update_irqs(s);
            timer_del(s->rx_timeout_timer);
            return;
        }
        if (ot_uart_is_rx_enabled(s) && !fifo8_is_empty(&s->rx_fifo)) {
            int64_t bit_ns = MAX(ot_uart_char_ns(s) / 10LL, 100LL);
            int64_t delay_ns = bit_ns * (int64_t)val;
            timer_mod(s->rx_timeout_timer,
                      qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + delay_ns);
            return;
        }
    }
    timer_del(s->rx_timeout_timer);
}

static void ot_uart_rx_timeout_timer_cb(void *opaque)
{
    OtUARTState *s = opaque;

    if ((s->regs[R_TIMEOUT_CTRL] & R_TIMEOUT_CTRL_EN_MASK) &&
        ot_uart_is_rx_enabled(s) && !fifo8_is_empty(&s->rx_fifo)) {
        s->regs[R_INTR_STATE] |= INTR_RX_TIMEOUT_MASK;
        ot_uart_update_irqs(s);
        ot_uart_update_rx_timeout(s);
    }
}

static OtUARTState *ot_uart_get_rx_owner(OtUARTState *s)
{
    for (int i = 0; i < 4; i++) {
        if (ot_uart_instances[i] && ot_uart_instances[i]->rx_target_uart == s) {
            return ot_uart_instances[i];
        }
    }
    return s;
}

static void ot_uart_receive_bytes(OtUARTState *s, OtUARTState *rx_owner,
                                  const uint8_t *buf, int size)
{
    if (size && !s->toggle_break) {
        /* no longer breaking, so emulate idle in oversampled VAL register */
        s->in_break = false;
    }

    if (rx_owner && (s->regs[R_CTRL] & R_CTRL_LLPBK_MASK)) {
        if (ot_uart_is_tx_enabled(s)) {
            CharFrontend *tx_chr = ot_uart_get_tx_chr(s);
            if (qemu_chr_fe_backend_connected(tx_chr)) {
                qemu_chr_fe_write(tx_chr, buf, size);
            }
        }
        return;
    }

    if (rx_owner && !ot_uart_check_pty_parity(s, rx_owner)) {
        s->regs[R_INTR_STATE] |= INTR_RX_PARITY_ERR_MASK;
        ot_uart_update_irqs(s);
        return;
    }

    if (!(s->regs[R_CTRL] & R_CTRL_RX_MASK)) {
        return;
    }

    size_t count = MIN(fifo8_num_free(&s->rx_fifo), (size_t)size);

    for (size_t index = 0; index < count; index++) {
        fifo8_push(&s->rx_fifo, buf[index]);
    }

    /* rx_overflow is event-type: latch it if the FIFO could not absorb all. */
    if (count != (size_t)size) {
        s->regs[R_INTR_STATE] |= INTR_RX_OVERFLOW_MASK;
    }

    ot_uart_update_rx_timeout(s);

    /* rx_watermark is status-type: computed live in ot_uart_update_irqs(). */
    ot_uart_update_irqs(s);
}

static void ot_uart_step_rx(OtUARTState *s)
{
    if (!(s->regs[R_CTRL] & R_CTRL_RX_MASK) ||
        fifo8_is_empty(&s->rx_staging_fifo) || timer_pending(s->rx_timer)) {
        return;
    }

    bool can_push = !fifo8_is_full(&s->rx_fifo) ||
                    ((s->regs[R_INTR_ENABLE] & INTR_RX_OVERFLOW_MASK) &&
                     !(s->regs[R_INTR_ENABLE] & INTR_RX_WATERMARK_MASK) &&
                     !(s->regs[R_INTR_STATE] & INTR_RX_OVERFLOW_MASK));
    if (can_push) {
        uint8_t ch = fifo8_pop(&s->rx_staging_fifo);
        ot_uart_receive_bytes(s, ot_uart_get_rx_owner(s), &ch, 1);
        if (!fifo8_is_empty(&s->rx_staging_fifo)) {
            timer_mod(s->rx_timer,
                      qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + ot_uart_char_ns(s));
        } else if (!ot_uart_is_sys_loopack_enabled(s)) {
            qemu_chr_fe_accept_input(ot_uart_get_rx_chr(s));
        }
    }
}

static void ot_uart_rx_timer_cb(void *opaque)
{
    ot_uart_step_rx(opaque);
}

static void ot_uart_reset_rx_fifo(OtUARTState *s)
{
    if (s->rx_timer) {
        timer_del(s->rx_timer);
    }
    fifo8_reset(&s->rx_staging_fifo);
    fifo8_reset(&s->rx_fifo);
    ot_uart_update_rx_timeout(s);
    if ((ot_uart_is_rx_enabled(s) || (s->regs[R_CTRL] & R_CTRL_LLPBK_MASK)) &&
        !ot_uart_is_sys_loopack_enabled(s)) {
        qemu_chr_fe_accept_input(ot_uart_get_rx_chr(s));
    }
}

static int ot_uart_can_receive(void *opaque)
{
    OtUARTState *rx_owner = opaque;
    OtUARTState *s = rx_owner->rx_target_uart;
    if (!s) {
        return 0;
    }

    if (s->regs[R_CTRL] & R_CTRL_LLPBK_MASK) {
        return (int)OT_UART_RX_FIFO_SIZE;
    }

    if (s->regs[R_CTRL] & R_CTRL_RX_MASK) {
        return (int)fifo8_num_free(&s->rx_staging_fifo);
    }

    return 0;
}

static void ot_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    OtUARTState *rx_owner = opaque;
    OtUARTState *s = rx_owner->rx_target_uart;
    if (!s || size <= 0) {
        return;
    }

    if (s->regs[R_CTRL] & R_CTRL_LLPBK_MASK) {
        ot_uart_receive_bytes(s, rx_owner, buf, size);
        return;
    }

    if (!(s->regs[R_CTRL] & R_CTRL_RX_MASK)) {
        return;
    }

    s->last_rx_ns = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    for (int i = 0; i < size; i++) {
        if (!fifo8_is_full(&s->rx_staging_fifo)) {
            fifo8_push(&s->rx_staging_fifo, buf[i]);
        }
    }

    if (!timer_pending(s->rx_timer)) {
        int64_t mult =
            fifo8_is_empty(&s->rx_fifo) ? (int64_t)OT_UART_TX_FIFO_SIZE : 1LL;
        timer_mod(s->rx_timer, s->last_rx_ns + mult * ot_uart_char_ns(s));
    }
}

static void ot_uart_event_handler(void *opaque, QEMUChrEvent event)
{
    OtUARTState *rx_owner = opaque;
    OtUARTState *s = rx_owner->rx_target_uart;
    if (!s) {
        return;
    }

    if (event == CHR_EVENT_BREAK) {
        if (!s->in_break) {
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

    if (fifo8_is_empty(&s->rx_fifo)) {
        return 0;
    }

    val = fifo8_pop(&s->rx_fifo);
    ot_uart_update_rx_timeout(s);

    /*
     * rx_watermark is status-type: draining the FIFO may drop it below the
     * threshold, which must de-assert the interrupt line immediately.
     */
    ot_uart_update_irqs(s);

    if (ot_uart_is_rx_enabled(s) && !ot_uart_is_sys_loopack_enabled(s)) {
        ot_uart_step_rx(s);
        qemu_chr_fe_accept_input(ot_uart_get_rx_chr(s));
    }

    return val;
}

static void ot_uart_reset_tx_fifo(OtUARTState *s)
{
    if (s->tx_timer) {
        timer_del(s->tx_timer);
    }
    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        s->watch_tag = 0;
    }
    s->tx_busy = false;
    fifo8_reset(&s->tx_fifo);
}

static void ot_uart_send_byte(OtUARTState *s, uint8_t ch)
{
    if (ot_uart_is_sys_loopack_enabled(s)) {
        ot_uart_receive_bytes(s, NULL, &ch, 1);
    } else {
        CharFrontend *tx_chr = ot_uart_get_tx_chr(s);
        if (qemu_chr_fe_backend_connected(tx_chr)) {
            qemu_chr_fe_write(tx_chr, &ch, 1);
        }
    }
}

static int64_t ot_uart_char_ns(const OtUARTState *s)
{
    uint32_t nco = FIELD_EX32(s->regs[R_CTRL], CTRL, NCO);
    if (nco && s->pclk) {
        return (int64_t)((10ULL * NANOSECONDS_PER_SECOND
                          << (R_CTRL_NCO_LENGTH + 4)) /
                         ((uint64_t)nco * (uint64_t)s->pclk));
    }
    return 86805LL;
}

static void ot_uart_tx_timer_cb(void *opaque)
{
    OtUARTState *s = opaque;

    if (ot_uart_is_tx_enabled(s) && !fifo8_is_empty(&s->tx_fifo)) {
        uint8_t ch = fifo8_pop(&s->tx_fifo);
        s->tx_busy = true;
        ot_uart_send_byte(s, ch);
        timer_mod(s->tx_timer,
                  qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + ot_uart_char_ns(s));
    } else {
        s->tx_busy = false;
        if (ot_uart_is_tx_enabled(s)) {
            s->regs[R_INTR_STATE] |= INTR_TX_DONE_MASK;
        }
    }
    ot_uart_update_irqs(s);
}

static gboolean ot_uart_watch_cb(void *do_not_use, GIOCondition cond,
                                 void *opaque);

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
        CharFrontend *tx_chr = ot_uart_get_tx_chr(s);
        while (!fifo8_is_empty(&s->tx_fifo)) {
            /* get a continuous buffer from the FIFO */
            buf = fifo8_peek_bufptr(&s->tx_fifo, fifo8_num_used(&s->tx_fifo),
                                    &size);
            /* send as much as possible */
            ret = qemu_chr_fe_write(tx_chr, buf, (int)size);
            /* if some characters were sent, remove them from the FIFO */
            if (ret > 0) {
                fifo8_drop(&s->tx_fifo, ret);
            }
            if (ret < (int)size) {
                if (s->watch_tag == 0) {
                    /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
                     */
                    s->watch_tag =
                        qemu_chr_fe_add_watch(tx_chr, G_IO_OUT | G_IO_HUP,
                                              ot_uart_watch_cb, s);
                    /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
                     */
                }
                break;
            }
        }
    }

    /* update INTR_STATE: tx_done is event-type and latched on drain.
     * tx_empty/tx_watermark are status-type and tracked live in update_irqs. */
    if (fifo8_is_empty(&s->tx_fifo)) {
        s->regs[R_INTR_STATE] |= INTR_TX_DONE_MASK;
    }

    ot_uart_update_irqs(s);
}

static void ot_uart_tx_schedule(OtUARTState *s)
{
    if (!ot_uart_is_tx_enabled(s)) {
        return;
    }
    bool use_timer =
        (s->regs[R_INTR_ENABLE] & (INTR_TX_DONE_MASK | INTR_TX_WATERMARK_MASK |
                                   INTR_TX_EMPTY_MASK)) != 0 ||
        s->tx_busy || ot_uart_is_sys_loopack_enabled(s) ||
        !qemu_chr_fe_backend_connected(ot_uart_get_tx_chr(s)) ||
        ot_uart_char_ns(s) > 100000LL;
    if (!use_timer) {
        ot_uart_xmit(s);
        return;
    }
    if (!s->tx_busy && !fifo8_is_empty(&s->tx_fifo)) {
        uint8_t ch = fifo8_pop(&s->tx_fifo);
        s->tx_busy = true;
        ot_uart_send_byte(s, ch);
        timer_mod(s->tx_timer,
                  qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + ot_uart_char_ns(s));
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
    ot_uart_tx_schedule(s);

    return FALSE;
}

static void uart_write_tx_fifo(OtUARTState *s, uint8_t val)
{
    s->rx_poll_count = 0u;
    if (fifo8_is_full(&s->tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ot_uart: TX FIFO overflow");
        return;
    }

    fifo8_push(&s->tx_fifo, val);

    if (ot_uart_is_tx_enabled(s)) {
        ot_uart_tx_schedule(s);
    } else {
        /* tx_watermark/tx_empty are status-type: refresh from the new level. */
        ot_uart_update_irqs(s);
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

static void ot_uart_rx_pace_timer_cb(void *opaque)
{
    OtUARTState *s = opaque;

    if (ot_uart_is_rx_enabled(s) && fifo8_is_empty(&s->rx_fifo) &&
        fifo8_is_empty(&s->rx_staging_fifo) && s->rx_poll_count >= 2u) {
        int64_t now_ns = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
        int64_t t0_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool recent =
            s->last_rx_ns > 0 && (now_ns - s->last_rx_ns) < 1500000000LL;
        s->rx_poll_count = 1u;
        g_usleep(recent ? 200u : 10u);
        qemu_chr_fe_accept_input(ot_uart_get_rx_chr(s));
        if (fifo8_is_empty(&s->rx_fifo) &&
            fifo8_is_empty(&s->rx_staging_fifo)) {
            int64_t slept_ns =
                CLAMP(qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0_ns, 0LL,
                      2000000LL);
            if (!recent) {
                slept_ns = MAX(slept_ns, 1800000LL);
            }
            icount_advance_bias_ns(slept_ns);
            now_ns += slept_ns;
        }
        timer_mod(s->rx_pace_timer, now_ns + (recent ? 150000LL : 200000LL));
    } else {
        s->rx_poll_count = 0u;
    }
}

static uint64_t ot_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    OtUARTState *s = opaque;
    (void)size;
    uint32_t val32;

    if (!s->pclk) {
        ot_common_stall_cpu_on_unclocked_mmio(DEVICE(s), addr);
        return 0;
    }

    if (s->rx_poll_count >= 4u && !s->regs[R_INTR_ENABLE] &&
        fifo8_is_empty(&s->rx_fifo) &&
        fifo8_num_used(&s->rx_staging_fifo) > OT_UART_RX_FIFO_SIZE) {
        timer_del(s->rx_timer);
        icount_advance_bias_ns(ot_uart_char_ns(s));
    }
    ot_uart_step_rx(s);

    if (!fifo8_is_empty(&s->tx_fifo) && !s->tx_busy &&
        ot_uart_is_tx_enabled(s)) {
        ot_uart_tx_schedule(s);
    }

    hwaddr reg = R32_OFF(addr);
    if (reg == R_STATUS && ot_uart_is_rx_enabled(s) &&
        fifo8_is_empty(&s->rx_fifo) &&
        (fifo8_is_empty(&s->rx_staging_fifo) ||
         (!s->regs[R_INTR_ENABLE] &&
          fifo8_num_used(&s->rx_staging_fifo) > OT_UART_RX_FIFO_SIZE))) {
        if (++s->rx_poll_count >= 4u && fifo8_is_empty(&s->rx_staging_fifo) &&
            !timer_pending(s->rx_pace_timer)) {
            int64_t now_ns = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
            bool recent =
                s->last_rx_ns > 0 && (now_ns - s->last_rx_ns) < 1500000000LL;
            timer_mod(s->rx_pace_timer,
                      now_ns + (recent ? 150000LL : 200000LL));
        }
    } else if (!fifo8_is_empty(&s->rx_fifo) ||
               !fifo8_is_empty(&s->rx_staging_fifo)) {
        s->rx_poll_count = 0u;
    }

    switch (reg) {
    case R_INTR_STATE:
        /* Status-type bits reflect the live FIFO condition, not a latch. */
        val32 = ot_uart_intr_state(s);
        break;
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
            val32 |= R_STATUS_TXEMPTY_MASK;
            if (!s->tx_busy) {
                val32 |= R_STATUS_TXIDLE_MASK;
            }
            break;
        case OT_UART_TX_FIFO_SIZE:
            val32 |= R_STATUS_TXFULL_MASK;
            break;
        default:
            break;
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
         * rx_val_q resets to 0x0000 and shifts in rx_in on tick_baud_x16 when
         * (tx_enable || rx_enable) and CTRL.NCO != 0. When oversample-break
         * is active and in_break is set, report 0x0000.
         */
        val32 = (s->in_break && s->oversample_break) ? 0u : s->regs[R_VAL];
        break;
    case R_OVRD:
    case R_TIMEOUT_CTRL:
        val32 = s->regs[reg];
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

    if (!s->pclk) {
        ot_common_stall_cpu_on_unclocked_mmio(DEVICE(s), addr);
        return;
    }

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_uart_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        /*
         * Only the event-type bits are RW1C; the status-type bits are ro and
         * are never stored in regs[R_INTR_STATE] (they are recomputed live), so
         * mask them out here and leave regs[R_INTR_STATE] holding event bits
         * only.
         */
        val32 &= INTR_MASK & ~INTR_STATUS_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C (event-type bits only) */
        ot_uart_update_rx_timeout(s);
        ot_uart_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_uart_update_irqs(s);
        if (ot_uart_is_rx_enabled(s) && !ot_uart_is_sys_loopack_enabled(s)) {
            qemu_chr_fe_accept_input(ot_uart_get_rx_chr(s));
        }
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32 & ~INTR_STATUS_MASK;
        s->regs[R_INTR_TEST] = val32 & INTR_STATUS_MASK;
        ot_uart_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        break;
    case R_CTRL:
        if (val32 & ~CTRL_SUP_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: UART_CTRL feature not supported: 0x%08x\n",
                          __func__, val32 & ~CTRL_SUP_MASK);
        }
        uint32_t prev = s->regs[R_CTRL];
        s->regs[R_CTRL] = val32 & CTRL_MASK;
        ot_uart_update_val(s);
        ot_uart_update_rx_timeout(s);
        uint32_t change = prev ^ s->regs[R_CTRL];
        if (change & R_CTRL_NCO_MASK) {
            ot_uart_check_baudrate(s);
        }
        if ((change & (R_CTRL_RX_MASK | R_CTRL_LLPBK_MASK)) &&
            (ot_uart_is_rx_enabled(s) ||
             (s->regs[R_CTRL] & R_CTRL_LLPBK_MASK)) &&
            !ot_uart_is_sys_loopack_enabled(s)) {
            qemu_chr_fe_accept_input(ot_uart_get_rx_chr(s));
        }
        if (change & R_CTRL_TX_MASK) {
            if (ot_uart_is_tx_enabled(s)) {
                /* try sending pending data from TX FIFO if any */
                ot_uart_tx_schedule(s);
            } else if (s->tx_busy) {
                if (s->tx_timer) {
                    timer_del(s->tx_timer);
                }
                s->tx_busy = false;
                if (fifo8_is_empty(&s->tx_fifo)) {
                    s->regs[R_INTR_STATE] |= INTR_TX_DONE_MASK;
                }
                ot_uart_update_irqs(s);
            }
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
        }
        if (val32 & R_FIFO_CTRL_TXRST_MASK) {
            ot_uart_reset_tx_fifo(s);
        }
        /*
         * Changing RXILVL/TXILVL moves the status-type watermark thresholds, so
         * re-evaluate the interrupt lines unconditionally.
         */
        ot_uart_update_irqs(s);
        break;
    case R_OVRD:
        s->regs[R_OVRD] = val32 & (R_OVRD_TXEN_MASK | R_OVRD_TXVAL_MASK);
        ot_uart_update_val(s);
        break;
    case R_TIMEOUT_CTRL:
        s->regs[R_TIMEOUT_CTRL] =
            val32 & (R_TIMEOUT_CTRL_EN_MASK | R_TIMEOUT_CTRL_VAL_MASK);
        ot_uart_update_rx_timeout(s);
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

static const uint8_t UART_PERMIT[REGS_COUNT] = {
    [R_INTR_STATE] = 0x3u,   [R_INTR_ENABLE] = 0x3u, [R_INTR_TEST] = 0x3u,
    [R_ALERT_TEST] = 0x1u,   [R_CTRL] = 0xfu,        [R_STATUS] = 0x1u,
    [R_RDATA] = 0x1u,        [R_WDATA] = 0x1u,       [R_FIFO_CTRL] = 0x1u,
    [R_FIFO_STATUS] = 0x7u,  [R_OVRD] = 0x1u,        [R_VAL] = 0x3u,
    [R_TIMEOUT_CTRL] = 0xfu,
};

static bool ot_uart_accepts(void *opaque, hwaddr addr, unsigned size,
                            bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    uint8_t reg_be = (uint8_t)(((1u << size) - 1u) << (addr & 3u));
    return reg < REGS_COUNT &&
           (!is_write || (UART_PERMIT[reg] & ~reg_be) == 0u);
}

static const MemoryRegionOps ot_uart_ops = {
    .read = ot_uart_read,
    .write = ot_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.accepts = &ot_uart_accepts,
};

static const Property ot_uart_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtUARTState, ot_id),
    DEFINE_PROP_CHR("chardev", OtUARTState, chr),
    DEFINE_PROP_STRING("clock-name", OtUARTState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtUARTState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_BOOL("oversample-break", OtUARTState, oversample_break, false),
    DEFINE_PROP_BOOL("toggle-break", OtUARTState, toggle_break, true),
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

    for (unsigned index = 0; index < ARRAY_SIZE(s->irqs); index++) {
        ibex_irq_set(&s->irqs[index], 0);
    }
    if (s->rx_pace_timer) {
        timer_del(s->rx_pace_timer);
    }
    if (s->rx_timeout_timer) {
        timer_del(s->rx_timeout_timer);
    }
    s->rx_poll_count = 0;
    s->last_rx_ns = 0;
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
    fifo8_create(&s->rx_staging_fifo, OT_UART_RX_STAGING_FIFO_SIZE);

    s->tx_chr_owner = s;
    s->rx_target_uart = s;
    s->tx_timer = timer_new_ns(OT_VIRTUAL_CLOCK, ot_uart_tx_timer_cb, s);
    s->rx_timer = timer_new_ns(OT_VIRTUAL_CLOCK, ot_uart_rx_timer_cb, s);
    s->rx_pace_timer =
        timer_new_ns(OT_VIRTUAL_CLOCK, ot_uart_rx_pace_timer_cb, s);
    s->rx_timeout_timer =
        timer_new_ns(OT_VIRTUAL_CLOCK, ot_uart_rx_timeout_timer_cb, s);
    if (s->ot_id[0] == 'u' && s->ot_id[1] >= '0' && s->ot_id[1] <= '3' &&
        s->ot_id[2] == '\0') {
        ot_uart_instances[s->ot_id[1] - '0'] = s;
    }

    qemu_chr_fe_set_handlers(&s->chr, ot_uart_can_receive, ot_uart_receive,
                             ot_uart_event_handler, ot_uart_be_change, s, NULL,
                             true);
}

static void ot_uart_init(Object *obj)
{
    OtUARTState *s = OT_UART(obj);

    s->pclk = 1u;
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
