/*
 * QEMU OpenTitan I2C device
 *
 * Copyright (c) 2024-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Duncan Laurie <duncan@rivosinc.com>
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

/*
 * The OpenTitan I2C Controller supports both host and target mode.
 *
 * The datasheet indicates that this controller should be able to support host
 * and target mode enabled at the same time but notes it may not be validated
 * in hardware.  The register, FIFO, and interrupt interfaces are separate so
 * enabling host and target mode at the same time is supported in QEMU.
 */

/*
 * Features not handled:
 * - This controller does not support 10 bit addressing.
 * - Anything that requires raw SCL/SDA:
 *      bus recover/override
 *      some interrupts will never be generated (except via INTR_TEST)
 *      bus timing registers are ignored
 * - Target mode only supports TARGET_ID.ADDRESS0 with TARGET_ID.MASK0=0x7F.
 * - Loopback mode.  Need more details about how it works in HW.
 */

#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/i2c/i2c.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_i2c.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "trace.h"

typedef enum {
    FMT_THRESHOLD,
    RX_THRESHOLD,
    ACQ_THRESHOLD,
    RX_OVERFLOW,
    CONTROLLER_HALT,
    SCL_INTERFERENCE,
    SDA_INTERFERENCE,
    STRETCH_TIMEOUT,
    SDA_UNSTABLE,
    CMD_COMPLETE,
    TX_STRETCH,
    TX_THRESHOLD,
    ACQ_STRETCH,
    UNEXP_STOP,
    HOST_TIMEOUT,
    OT_I2C_IRQ_NUM
} OtI2CInterrupt;

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_FMT_THRESHOLD, FMT_THRESHOLD, 1u)
    SHARED_FIELD(INTR_RX_THRESHOLD, RX_THRESHOLD, 1u)
    SHARED_FIELD(INTR_ACQ_THRESHOLD, ACQ_THRESHOLD, 1u)
    SHARED_FIELD(INTR_RX_OVERFLOW, RX_OVERFLOW, 1u)
    SHARED_FIELD(INTR_CONTROLLER_HALT, CONTROLLER_HALT, 1u)
    SHARED_FIELD(INTR_SCL_INTERFERENCE, SCL_INTERFERENCE, 1u)
    SHARED_FIELD(INTR_SDA_INTERFERENCE, SDA_INTERFERENCE, 1u)
    SHARED_FIELD(INTR_STRETCH_TIMEOUT, STRETCH_TIMEOUT, 1u)
    SHARED_FIELD(INTR_SDA_UNSTABLE, SDA_UNSTABLE, 1u)
    SHARED_FIELD(INTR_CMD_COMPLETE, CMD_COMPLETE, 1u)
    SHARED_FIELD(INTR_TX_STRETCH, TX_STRETCH, 1u)
    SHARED_FIELD(INTR_TX_THRESHOLD, TX_THRESHOLD, 1u)
    SHARED_FIELD(INTR_ACQ_STRETCH, ACQ_STRETCH, 1u)
    SHARED_FIELD(INTR_UNEXP_STOP, UNEXP_STOP, 1u)
    SHARED_FIELD(INTR_HOST_TIMEOUT, HOST_TIMEOUT, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CTRL, 0x10u)
    FIELD(CTRL, ENABLEHOST, 0u, 1u)
    FIELD(CTRL, ENABLETARGET, 1u, 1u)
    FIELD(CTRL, LLPBK, 2u, 1u)
    FIELD(CTRL, NACK_ADDR_AFTER_TIMEOUT, 3u, 1u)
    FIELD(CTRL, ACK_CTRL_EN, 4u, 1u)
    FIELD(CTRL, MULTI_CONTROLLER_MONITOR_EN, 5u, 1u)
    FIELD(CTRL, TX_STRETCH_CTRL_EN, 6u, 1u)
REG32(STATUS, 0x14u)
    FIELD(STATUS, FMTFULL, 0u, 1u)
    FIELD(STATUS, RXFULL, 1u, 1u)
    FIELD(STATUS, FMTEMPTY, 2u, 1u)
    FIELD(STATUS, HOSTIDLE, 3u, 1u)
    FIELD(STATUS, TARGETIDLE, 4u, 1u)
    FIELD(STATUS, RXEMPTY, 5u, 1u)
    FIELD(STATUS, TXFULL, 6u, 1u)
    FIELD(STATUS, ACQFULL, 7u, 1u)
    FIELD(STATUS, TXEMPTY, 8u, 1u)
    FIELD(STATUS, ACQEMPTY, 9u, 1u)
    FIELD(STATUS, ACK_CTRL_STRETCH, 10u, 1u)
REG32(RDATA, 0x18u)
    FIELD(RDATA, RDATA, 0u, 8u)
REG32(FDATA, 0x1cu)
    FIELD(FDATA, FBYTE, 0u, 8u)
    FIELD(FDATA, START, 8u, 1u)
    FIELD(FDATA, STOP, 9u, 1u)
    FIELD(FDATA, READB, 10u, 1u)
    FIELD(FDATA, RCONT, 11u, 1u)
    FIELD(FDATA, NAKOK, 12u, 1u)
REG32(FIFO_CTRL, 0x20u)
    FIELD(FIFO_CTRL, RXRST, 0u, 1u)
    FIELD(FIFO_CTRL, FMTRST, 1u, 1u)
    FIELD(FIFO_CTRL, ACQRST, 7u, 1u)
    FIELD(FIFO_CTRL, TXRST, 8u, 1u)
REG32(HOST_FIFO_CONFIG, 0x24u)
    FIELD(HOST_FIFO_CONFIG, RX_THRESH, 0u, 12u)
    FIELD(HOST_FIFO_CONFIG, FMT_THRESH, 16u, 12u)
REG32(TARGET_FIFO_CONFIG, 0x28u)
    FIELD(TARGET_FIFO_CONFIG, TX_THRESH, 0u, 12u)
    FIELD(TARGET_FIFO_CONFIG, ACQ_THRESH, 16u, 12u)
REG32(HOST_FIFO_STATUS, 0x2cu)
    FIELD(HOST_FIFO_STATUS, FMTLVL, 0u, 12u)
    FIELD(HOST_FIFO_STATUS, RXLVL, 16u, 12u)
REG32(TARGET_FIFO_STATUS, 0x30u)
    FIELD(TARGET_FIFO_STATUS, TXLVL, 0u, 12u)
    FIELD(TARGET_FIFO_STATUS, ACQLVL, 16u, 12u)
REG32(OVRD, 0x34u)
    FIELD(OVRD, TXOVRDEN, 0u, 1u)
    FIELD(OVRD, SCLVAL, 1u, 1u)
    FIELD(OVRD, SDAVAL, 2u, 1u)
REG32(VAL, 0x38u)
    FIELD(VAL, SCL_RX, 0u, 16u)
    FIELD(VAL, SDA_RX, 16u, 16u)
REG32(TIMING0, 0x3cu)
    FIELD(TIMING0, THIGH, 0u, 13u)
    FIELD(TIMING0, TLOW, 16u, 13u)
REG32(TIMING1, 0x40u)
    FIELD(TIMING1, T_R, 0u, 10u)
    FIELD(TIMING1, T_F, 16u, 9u)
REG32(TIMING2, 0x44u)
    FIELD(TIMING2, TSU_STA, 0u, 13u)
    FIELD(TIMING2, THD_STA, 16u, 13u)
REG32(TIMING3, 0x48u)
    FIELD(TIMING3, TSU_DAT, 0u, 9u)
    FIELD(TIMING3, THD_DAT, 16u, 13u)
REG32(TIMING4, 0x4cu)
    FIELD(TIMING4, TSU_STO, 0u, 13u)
    FIELD(TIMING4, T_BUF, 16u, 13u)
REG32(TIMEOUT_CTRL, 0x50u)
    FIELD(TIMEOUT_CTRL, VAL, 0u, 30u)
    FIELD(TIMEOUT_CTRL, MODE, 30u, 1u)
    FIELD(TIMEOUT_CTRL, EN, 31u, 1u)
REG32(TARGET_ID, 0x54u)
    FIELD(TARGET_ID, ADDRESS0, 0u, 7u)
    FIELD(TARGET_ID, MASK0, 7u, 7u)
    FIELD(TARGET_ID, ADDRESS1, 14u, 7u)
    FIELD(TARGET_ID, MASK1, 21u, 7u)
REG32(ACQDATA, 0x58u)
    FIELD(ACQDATA, ABYTE, 0u, 8u)
    FIELD(ACQDATA, SIGNAL, 8u, 3u)
REG32(TXDATA, 0x5cu)
    FIELD(TXDATA, TXDATA, 0u, 8u)
REG32(HOST_TIMEOUT_CTRL, 0x60u)
    FIELD(HOST_TIMEOUT_CTRL, HOST_TIMEOUT_CTRL, 0u, 20u)
REG32(TARGET_TIMEOUT_CTRL, 0x64u)
    FIELD(TARGET_TIMEOUT_CTRL, VAL, 0u, 31u)
    FIELD(TARGET_TIMEOUT_CTRL, EN, 31u, 1u)
REG32(TARGET_NACK_COUNT, 0x68u)
    FIELD(TARGET_NACK_COUNT, TARGET_NACK_COUNT, 0u, 8u)
REG32(TARGET_ACK_CTRL, 0x6cu)
    FIELD(TARGET_ACK_CTRL, NBYTES, 0u, 9u)
    FIELD(TARGET_ACK_CTRL, NACK, 31u, 1u)
REG32(ACQ_FIFO_NEXT_DATA, 0x70u)
    FIELD(ACQ_FIFO_NEXT_DATA, ACQ_FIFO_NEXT_DATA, 0u, 8u)
REG32(HOST_NACK_HANDLER_TIMEOUT, 0x74u)
    FIELD(HOST_NACK_HANDLER_TIMEOUT, VAL, 0u, 31u)
    FIELD(HOST_NACK_HANDLER_TIMEOUT, EN, 31u, 1u)
REG32(CONTROLLER_EVENTS, 0x78u)
    FIELD(CONTROLLER_EVENTS, NACK, 0u, 1u)
    FIELD(CONTROLLER_EVENTS, UNHANDLED_NACK_TIMEOUT, 1u, 1u)
    FIELD(CONTROLLER_EVENTS, BUS_TIMEOUT, 2u, 1u)
    FIELD(CONTROLLER_EVENTS, ARBITRATION_LOST, 3u, 1u)
REG32(TARGET_EVENTS, 0x7cu)
    FIELD(TARGET_EVENTS, TX_PENDING, 0u, 1u)
    FIELD(TARGET_EVENTS, BUS_TIMEOUT, 1u, 1u)
    FIELD(TARGET_EVENTS, ARBITRATION_LOST, 2u, 1u)
/* clang-format on */

#define INTR_RW1C_MASK \
    (INTR_RX_OVERFLOW_MASK | INTR_SCL_INTERFERENCE_MASK | \
     INTR_SDA_INTERFERENCE_MASK | INTR_STRETCH_TIMEOUT_MASK | \
     INTR_SDA_UNSTABLE_MASK | INTR_CMD_COMPLETE_MASK | INTR_UNEXP_STOP_MASK | \
     INTR_HOST_TIMEOUT_MASK)

#define INTR_MASK \
    (INTR_RW1C_MASK | INTR_FMT_THRESHOLD_MASK | INTR_RX_THRESHOLD_MASK | \
     INTR_ACQ_THRESHOLD_MASK | INTR_CONTROLLER_HALT_MASK | \
     INTR_TX_STRETCH_MASK | INTR_TX_THRESHOLD_MASK | INTR_ACQ_STRETCH_MASK)

#define CONTROLLER_EVENTS_RW1C_MASK \
    (R_CONTROLLER_EVENTS_NACK_MASK | \
     R_CONTROLLER_EVENTS_UNHANDLED_NACK_TIMEOUT_MASK | \
     R_CONTROLLER_EVENTS_BUS_TIMEOUT_MASK | \
     R_CONTROLLER_EVENTS_ARBITRATION_LOST_MASK)

#define TARGET_EVENTS_RW1C_MASK \
    (R_TARGET_EVENTS_TX_PENDING_MASK | R_TARGET_EVENTS_BUS_TIMEOUT_MASK | \
     R_TARGET_EVENTS_ARBITRATION_LOST_MASK)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_TARGET_EVENTS)
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
    REG_NAME_ENTRY(FDATA),
    REG_NAME_ENTRY(FIFO_CTRL),
    REG_NAME_ENTRY(HOST_FIFO_CONFIG),
    REG_NAME_ENTRY(TARGET_FIFO_CONFIG),
    REG_NAME_ENTRY(HOST_FIFO_STATUS),
    REG_NAME_ENTRY(TARGET_FIFO_STATUS),
    REG_NAME_ENTRY(OVRD),
    REG_NAME_ENTRY(VAL),
    REG_NAME_ENTRY(TIMING0),
    REG_NAME_ENTRY(TIMING1),
    REG_NAME_ENTRY(TIMING2),
    REG_NAME_ENTRY(TIMING3),
    REG_NAME_ENTRY(TIMING4),
    REG_NAME_ENTRY(TIMEOUT_CTRL),
    REG_NAME_ENTRY(TARGET_ID),
    REG_NAME_ENTRY(ACQDATA),
    REG_NAME_ENTRY(TXDATA),
    REG_NAME_ENTRY(HOST_TIMEOUT_CTRL),
    REG_NAME_ENTRY(TARGET_TIMEOUT_CTRL),
    REG_NAME_ENTRY(TARGET_NACK_COUNT),
    REG_NAME_ENTRY(TARGET_ACK_CTRL),
    REG_NAME_ENTRY(ACQ_FIFO_NEXT_DATA),
    REG_NAME_ENTRY(HOST_NACK_HANDLER_TIMEOUT),
    REG_NAME_ENTRY(CONTROLLER_EVENTS),
    REG_NAME_ENTRY(TARGET_EVENTS),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

#define IRQ_NAME_ENTRY(_irq_) [_irq_] = stringify(_irq_)
static const char *IRQ_NAMES[OT_I2C_IRQ_NUM] = {
    /* clang-format off */
    IRQ_NAME_ENTRY(FMT_THRESHOLD),
    IRQ_NAME_ENTRY(RX_THRESHOLD),
    IRQ_NAME_ENTRY(ACQ_THRESHOLD),
    IRQ_NAME_ENTRY(RX_OVERFLOW),
    IRQ_NAME_ENTRY(CONTROLLER_HALT),
    IRQ_NAME_ENTRY(SCL_INTERFERENCE),
    IRQ_NAME_ENTRY(SDA_INTERFERENCE),
    IRQ_NAME_ENTRY(STRETCH_TIMEOUT),
    IRQ_NAME_ENTRY(SDA_UNSTABLE),
    IRQ_NAME_ENTRY(CMD_COMPLETE),
    IRQ_NAME_ENTRY(TX_STRETCH),
    IRQ_NAME_ENTRY(TX_THRESHOLD),
    IRQ_NAME_ENTRY(ACQ_STRETCH),
    IRQ_NAME_ENTRY(UNEXP_STOP),
    IRQ_NAME_ENTRY(HOST_TIMEOUT),
    /* clang-format on */
};
#undef IRQ_NAME_ENTRY

#define OT_I2C_FIFO_SIZE 64u

typedef enum {
    SIGNAL_NONE,
    SIGNAL_START,
    SIGNAL_STOP,
    SIGNAL_RESTART,
    SIGNAL_NACK,
    SIGNAL_NACK_START,
    SIGNAL_NACK_STOP
} OtI2CSignal;

struct OtI2CState {
    SysBusDevice parent_obj;

    I2CBus *bus;
    I2CSlave *target;

    MemoryRegion mmio;

    uint32_t regs[REGS_COUNT];
    IbexIRQ irqs[OT_I2C_IRQ_NUM];
    IbexIRQ alert;

    /*
     * FMT: Scheduled operations for host mode.
     * [7:0] = Data byte
     * [12] = NAKOK
     */
    OtFifo32 host_tx_fifo;
    uint32_t host_tx_threshold;

    /* RX: Received bytes for host mode. */
    Fifo8 host_rx_fifo;

    /*
     * ACQ: Received bytes + signals for target mode.
     * [7:0] = Data byte
     * [10:8] = Signal (OtI2CSignal)
     */
    OtFifo32 target_rx_fifo;

    /* Whether I2C timings should be checked before comm. over the bus */
    bool check_timings;

    /* TX: Scheduled responses for target mode. */
    Fifo8 target_tx_fifo;

    uint32_t pclk; /* Current input clock */
    const char *clock_src_name; /* IRQ name once connected */

    char *ot_id;
    char *clock_name;
    DeviceState *clock_src;
};

struct OtI2CClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

struct OtI2CTarget {
    I2CSlave i2c;
};

static void ot_i2c_update_irqs(OtI2CState *s)
{
    uint32_t state_masked = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    if (s->regs[R_INTR_STATE] || s->regs[R_INTR_ENABLE]) {
        trace_ot_i2c_update_irqs(s->ot_id, s->regs[R_INTR_STATE],
                                 s->regs[R_INTR_ENABLE], state_masked);
    }

    for (unsigned index = 0; index < ARRAY_SIZE(s->irqs); index++) {
        bool level = (state_masked & (1U << index)) != 0;
        ibex_irq_set(&s->irqs[index], level);
    }
}

static void ot_i2c_irq_set_state(OtI2CState *s, OtI2CInterrupt irq, bool en)
{
    unsigned long *addr = (unsigned long *)&s->regs[R_INTR_STATE];

    if (irq > ARRAY_SIZE(s->irqs)) {
        return;
    }
    if (test_bit(irq, addr) == en) {
        return;
    }

    trace_ot_i2c_irq(s->ot_id, IRQ_NAMES[irq], en);

    if (en) {
        set_bit(irq, addr);
    } else {
        clear_bit(irq, addr);
    }

    ot_i2c_update_irqs(s);
}

static bool ot_i2c_host_enabled(const OtI2CState *s)
{
    return (bool)ARRAY_FIELD_EX32(s->regs, CTRL, ENABLEHOST);
}

static bool ot_i2c_target_enabled(const OtI2CState *s)
{
    return (bool)ARRAY_FIELD_EX32(s->regs, CTRL, ENABLETARGET);
}

static uint32_t ot_i2c_get_fmt_threshold(const OtI2CState *s)
{
    return ARRAY_FIELD_EX32(s->regs, HOST_FIFO_CONFIG, FMT_THRESH);
}

static uint32_t ot_i2c_get_rx_threshold(const OtI2CState *s)
{
    return ARRAY_FIELD_EX32(s->regs, HOST_FIFO_CONFIG, RX_THRESH);
}

static uint32_t ot_i2c_get_acq_threshold(const OtI2CState *s)
{
    return ARRAY_FIELD_EX32(s->regs, TARGET_FIFO_CONFIG, ACQ_THRESH);
}

static uint32_t ot_i2c_get_tx_threshold(const OtI2CState *s)
{
    return ARRAY_FIELD_EX32(s->regs, TARGET_FIFO_CONFIG, TX_THRESH);
}

static bool ot_i2c_fmt_threshold_intr(OtI2CState *s)
{
    return ot_fifo32_num_used(&s->host_tx_fifo) < ot_i2c_get_fmt_threshold(s);
}

static bool ot_i2c_rx_threshold_intr(OtI2CState *s)
{
    return fifo8_num_used(&s->host_rx_fifo) > ot_i2c_get_rx_threshold(s);
}

static bool ot_i2c_acq_threshold_intr(OtI2CState *s)
{
    return ot_fifo32_num_used(&s->target_rx_fifo) > ot_i2c_get_acq_threshold(s);
}

static bool ot_i2c_tx_threshold_intr(OtI2CState *s)
{
    return fifo8_num_used(&s->target_tx_fifo) < ot_i2c_get_tx_threshold(s);
}


static void ot_i2c_host_reset_tx_fifo(OtI2CState *s)
{
    SHARED_ARRAY_FIELD_DP32(s->regs, R_INTR_STATE, INTR_FMT_THRESHOLD, 0);
    ot_fifo32_reset(&s->host_tx_fifo);
    s->host_tx_threshold = 0;
}

static void ot_i2c_host_reset_rx_fifo(OtI2CState *s)
{
    SHARED_ARRAY_FIELD_DP32(s->regs, R_INTR_STATE, INTR_RX_THRESHOLD, 0);
    SHARED_ARRAY_FIELD_DP32(s->regs, R_INTR_STATE, INTR_RX_OVERFLOW, 0);
    fifo8_reset(&s->host_rx_fifo);
}

static void ot_i2c_target_reset_tx_fifo(OtI2CState *s)
{
    SHARED_ARRAY_FIELD_DP32(s->regs, R_INTR_STATE, INTR_TX_THRESHOLD, 0);
    fifo8_reset(&s->target_tx_fifo);
}

static void ot_i2c_target_reset_rx_fifo(OtI2CState *s)
{
    SHARED_ARRAY_FIELD_DP32(s->regs, R_INTR_STATE, INTR_ACQ_THRESHOLD, 0);
    ot_fifo32_reset(&s->target_rx_fifo);
}

static uint8_t ot_i2c_host_read_rx_fifo(OtI2CState *s)
{
    if (!ot_i2c_host_enabled(s)) {
        return 0;
    }
    if (fifo8_is_empty(&s->host_rx_fifo)) {
        return 0;
    }

    return fifo8_pop(&s->host_rx_fifo);
}

static void ot_i2c_host_send(OtI2CState *s)
{
    trace_ot_i2c_host_send(s->ot_id, ot_fifo32_num_used(&s->host_tx_fifo),
                           s->host_tx_threshold);

    /* Send all the data in the TX FIFO to the target. */
    while (!ot_fifo32_is_empty(&s->host_tx_fifo)) {
        uint32_t val = ot_fifo32_pop(&s->host_tx_fifo);
        uint8_t fbyte = (uint8_t)FIELD_EX32(val, FDATA, FBYTE);
        bool nakok = (bool)FIELD_EX32(val, FDATA, NAKOK);
        if (i2c_send(s->bus, fbyte) && !nakok) {
            /*
             * Error while sending byte and NAKOK unset,
             * raise controller halt interrupt.
             */
            ARRAY_FIELD_DP32(s->regs, CONTROLLER_EVENTS, NACK, 1);
            ot_i2c_irq_set_state(s, CONTROLLER_HALT, true);
            break;
        }
    }

    /*
     * Threshold interrupt is raised when FIFO depth goes from above
     * threshold to below. If we haven't reached the threshold, reset the
     * cached threshold level.
     */
    if (s->host_tx_threshold &&
        ot_fifo32_num_used(&s->host_tx_fifo) < s->host_tx_threshold) {
        ot_i2c_irq_set_state(s, FMT_THRESHOLD, true);
        s->host_tx_threshold = 0;
    }
}

static uint32_t ot_i2c_target_read_rx_fifo(OtI2CState *s)
{
    if (!ot_i2c_target_enabled(s)) {
        return 0;
    }
    if (ot_fifo32_is_empty(&s->target_rx_fifo)) {
        return 0;
    }

    return ot_fifo32_pop(&s->target_rx_fifo);
}

static void ot_i2c_target_write_tx_fifo(OtI2CState *s, uint8_t val)
{
    if (!ot_i2c_target_enabled(s)) {
        return;
    }

    /* Handle a full FIFO. */
    if (fifo8_is_full(&s->target_tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Target TX FIFO overflow\n",
                      __func__, s->ot_id);
    } else {
        /* Add this entry to the FIFO. */
        fifo8_push(&s->target_tx_fifo, val);
    }

    ot_i2c_irq_set_state(s, TX_THRESHOLD, ot_i2c_tx_threshold_intr(s));
}

static bool ot_i2c_check_timings(OtI2CState *s)
{
    if (!s->pclk) {
        return 0;
    }

    uint32_t thigh = FIELD_EX32(s->regs[R_TIMING0], TIMING0, THIGH);
    uint32_t tlow = FIELD_EX32(s->regs[R_TIMING0], TIMING0, TLOW);
    uint32_t tr = FIELD_EX32(s->regs[R_TIMING1], TIMING1, T_R);
    uint32_t tf = FIELD_EX32(s->regs[R_TIMING1], TIMING1, T_F);
    uint32_t tsusta = FIELD_EX32(s->regs[R_TIMING2], TIMING2, TSU_STA);
    uint32_t thdsta = FIELD_EX32(s->regs[R_TIMING2], TIMING2, THD_STA);
    uint32_t tsudat = FIELD_EX32(s->regs[R_TIMING3], TIMING3, TSU_DAT);
    uint32_t thddat = FIELD_EX32(s->regs[R_TIMING3], TIMING3, THD_DAT);
    uint32_t tsusto = FIELD_EX32(s->regs[R_TIMING4], TIMING4, TSU_STO);
    uint32_t tbuf = FIELD_EX32(s->regs[R_TIMING4], TIMING4, T_BUF);

    bool res = true;

    /* Check I2C HW limits (I2C input clock cycles) */

    if (thddat == 0u || (thdsta < thddat + 2u)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: invalid THD settings\n",
                      __func__, s->ot_id);
        res = false;
    }
    if (tlow < 3u + tr) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: invalid Tlow settings\n",
                      __func__, s->ot_id);
        res = false;
    }
    if (thigh < 4u) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: invalid Thigh settings\n",
                      __func__, s->ot_id);
        res = false;
    }

    /* Convert clock cycles into nanoseconds based on input clock */

    thigh = (uint32_t)((((uint64_t)thigh) * NANOSECONDS_PER_SECOND) / s->pclk);
    tlow = (uint32_t)((((uint64_t)tlow) * NANOSECONDS_PER_SECOND) / s->pclk);

    /* Check I2C limits (from I2C specification rev. 6, table 10) */

    if (((thigh >= 4000u) && (tlow < 4700u)) ||
        ((tlow >= 4700u) && (thigh < 4000u)) ||
        ((thigh >= 600) && (tlow < 1300u)) ||
        ((tlow >= 1300u) && (thigh < 600u)) ||
        ((thigh < 260u) || (tlow < 500u))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: invalid Thigh/Tlow settings\n",
                      __func__, s->ot_id);
        res = false;
        return res; /* subsequent checks would lead to more errors */
    }

    uint32_t tsusta_min;
    uint32_t tsudat_min;
    uint32_t tsusto_min;
    uint32_t tbuf_min;
    uint32_t tr_max;
    uint32_t tf_max;
    if (thigh >= 4000u) {
        /* standard mode */
        tsusta_min = 4700u;
        tsudat_min = 250u;
        tsusto_min = 4000u;
        tbuf_min = 4700u;
        tr_max = 1000u;
        tf_max = 300u;
    } else if (thigh > 600u) {
        /* fast mode */
        tsusta_min = 600u;
        tsudat_min = 100u;
        tsusto_min = 600u;
        tbuf_min = 1300u;
        tr_max = 300u;
        tf_max = 300u;
    } else {
        /* fast mode plus */
        tsusta_min = 260u;
        tsudat_min = 50u;
        tsusto_min = 260u;
        tbuf_min = 500u;
        tr_max = 120u;
        tf_max = 1230u;
    }

    tsusta =
        (uint32_t)((((uint64_t)tsusta) * NANOSECONDS_PER_SECOND) / s->pclk);
    tsudat =
        (uint32_t)((((uint64_t)tsudat) * NANOSECONDS_PER_SECOND) / s->pclk);
    tsusto =
        (uint32_t)((((uint64_t)tsusto) * NANOSECONDS_PER_SECOND) / s->pclk);
    tbuf = (uint32_t)((((uint64_t)tbuf) * NANOSECONDS_PER_SECOND) / s->pclk);
    tr = (uint32_t)((((uint64_t)tr) * NANOSECONDS_PER_SECOND) / s->pclk);
    tf = (uint32_t)((((uint64_t)tf) * NANOSECONDS_PER_SECOND) / s->pclk);

    if (tsusta < tsusta_min) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Tsu;sta too low\n", __func__,
                      s->ot_id);
        res = false;
    }
    if (tsudat < tsudat_min) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Tsu;dat too low\n", __func__,
                      s->ot_id);
        res = false;
    }
    if (tsusto < tsusto_min) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Tsu;sto too low\n", __func__,
                      s->ot_id);
        res = false;
    }
    if (tbuf < tbuf_min) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Tbuf too low\n", __func__,
                      s->ot_id);
        res = false;
    }
    if (tr > tr_max) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Tr too high\n", __func__,
                      s->ot_id);
        res = false;
    }
    if (tf > tf_max) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Tf too high\n", __func__,
                      s->ot_id);
        res = false;
    }

    return res;
}

static void ot_i2c_clock_input(void *opaque, int irq, int level)
{
    OtI2CState *s = opaque;

    g_assert(irq == 0);

    if (level && ((uint32_t)level != s->pclk)) {
        s->check_timings = true;
    }

    s->pclk = (uint32_t)level;
    /* TODO: disable I2C transfers when PCLK is 0 */
}

static uint64_t ot_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    OtI2CState *s = opaque;
    uint32_t val32 = 0;
    hwaddr reg = R32_OFF(addr);
    (void)size;

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CTRL:
    case R_HOST_FIFO_CONFIG:
    case R_TARGET_FIFO_CONFIG:
    case R_TARGET_ID:
    case R_TIMEOUT_CTRL:
    case R_HOST_TIMEOUT_CTRL:
    case R_CONTROLLER_EVENTS:
    case R_TARGET_EVENTS:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        val32 = FIELD_DP32(val32, STATUS, HOSTIDLE, !i2c_bus_busy(s->bus));
        val32 = FIELD_DP32(val32, STATUS, TARGETIDLE, !i2c_bus_busy(s->bus));

        /* Report host TX FIFO status. */
        if (ot_fifo32_is_empty(&s->host_tx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, FMTEMPTY, 1u);
        }
        if (ot_fifo32_is_full(&s->host_tx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, FMTFULL, 1u);
        }

        /* Report host RX FIFO status. */
        if (fifo8_is_empty(&s->host_rx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, RXEMPTY, 1u);
        }
        if (fifo8_is_full(&s->host_rx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, RXFULL, 1u);
        }

        /* Report target TX FIFO status. */
        if (fifo8_is_empty(&s->target_tx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, TXEMPTY, 1u);
        }
        if (fifo8_is_full(&s->target_tx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, TXFULL, 1u);
        }

        /* Report target TX FIFO status. */
        if (ot_fifo32_is_empty(&s->target_rx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, ACQEMPTY, 1u);
        }
        if (ot_fifo32_is_full(&s->target_rx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, ACQFULL, 1u);
        }
        break;
    case R_RDATA:
        val32 = (uint32_t)ot_i2c_host_read_rx_fifo(s);
        ot_i2c_irq_set_state(s, RX_THRESHOLD, ot_i2c_rx_threshold_intr(s));
        break;
    case R_ACQDATA:
        val32 = (uint32_t)ot_i2c_target_read_rx_fifo(s);
        /* Deassert level interrupt state if FIFO is no longer above the
         * threshold. */
        ot_i2c_irq_set_state(s, ACQ_THRESHOLD, ot_i2c_acq_threshold_intr(s));
        break;
    case R_HOST_FIFO_STATUS:
        val32 = FIELD_DP32(val32, HOST_FIFO_STATUS, FMTLVL,
                           ot_fifo32_num_used(&s->host_tx_fifo));
        val32 = FIELD_DP32(val32, HOST_FIFO_STATUS, RXLVL,
                           fifo8_num_used(&s->host_rx_fifo));
        break;
    case R_TARGET_FIFO_STATUS:
        val32 = FIELD_DP32(val32, TARGET_FIFO_STATUS, TXLVL,
                           fifo8_num_used(&s->target_tx_fifo));
        val32 = FIELD_DP32(val32, TARGET_FIFO_STATUS, ACQLVL,
                           ot_fifo32_num_used(&s->target_rx_fifo));
        break;
    case R_OVRD:
    case R_VAL:
    case R_TIMING0:
    case R_TIMING1:
    case R_TIMING2:
    case R_TIMING3:
    case R_TIMING4:
        val32 = s->regs[reg];
        break;
    case R_TARGET_TIMEOUT_CTRL:
    case R_TARGET_NACK_COUNT:
    case R_TARGET_ACK_CTRL:
    case R_ACQ_FIFO_NEXT_DATA:
    case R_HOST_NACK_HANDLER_TIMEOUT:
        qemu_log_mask(LOG_UNIMP, "%s: %s: register %s is not implemented\n",
                      __func__, s->ot_id, REG_NAME(reg));
        break;
    case R_FIFO_CTRL:
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_FDATA:
    case R_TXDATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
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

    uint64_t pc = ibex_get_current_pc();
    trace_ot_i2c_io_read(s->ot_id, (unsigned)addr, REG_NAME(reg),
                         (uint64_t)val32, pc);

    return (uint64_t)val32;
}

static unsigned ot_i2c_host_recv_fill_fifo(OtI2CState *s, unsigned chunk)
{
    unsigned index = 0;

    trace_ot_i2c_host_recv(s->ot_id, fifo8_num_used(&s->host_rx_fifo), chunk);

    /* Check if read is larger than room in the FIFO. */
    if (fifo8_num_free(&s->host_rx_fifo) < chunk) {
        chunk = fifo8_num_free(&s->host_rx_fifo);
    }

    /* Read expected number of bytes from target. */
    for (index = 0; index < chunk; index++) {
        fifo8_push(&s->host_rx_fifo, i2c_recv(s->bus));
    }

    /* Check if rx_threshold interrupt should be asserted. */
    ot_i2c_irq_set_state(s, RX_THRESHOLD, ot_i2c_rx_threshold_intr(s));

    /* Return number of bytes read. */
    return index;
}

static void ot_i2c_write_fdata(OtI2CState *s, uint32_t fdata)
{
    uint8_t fbyte = FIELD_EX32(fdata, FDATA, FBYTE);
    bool readb = FIELD_EX32(fdata, FDATA, READB);
    bool start = FIELD_EX32(fdata, FDATA, START);
    bool stop = FIELD_EX32(fdata, FDATA, STOP);
    bool rcont = FIELD_EX32(fdata, FDATA, RCONT);
    bool nakok = FIELD_EX32(fdata, FDATA, NAKOK);

    if (!ot_i2c_host_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: I2C host not enabled, no write issued\n",
                      __func__, s->ot_id);
        return;
    }

    if (readb) {
        /* Number of bytes to read is in FDATA.FBYTE, 0 means 256 bytes. */
        unsigned bytes_to_read = fbyte ?: 256;
        unsigned index;

        if (nakok) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Invalid FDATA flags READB+NAKOK\n", __func__,
                          s->ot_id);
        }
        if (rcont && stop) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Invalid FDATA flags READB+RCONT+STOP\n",
                          __func__, s->ot_id);
            return;
        }

        /* Read bytes from target device into host_rx_fifo. */
        do {
            if (fifo8_is_full(&s->host_rx_fifo)) {
                /* End the transfer and exit. */
                ot_i2c_irq_set_state(s, HOST_TIMEOUT, true);
                i2c_end_transfer(s->bus);
                return;
            }
            index = ot_i2c_host_recv_fill_fifo(s, bytes_to_read);
            if (index == 0 || index >= bytes_to_read) {
                break;
            }
            bytes_to_read -= index;
        } while (bytes_to_read);

        /* NACK the last byte read if indicated to allow reads >256 bytes. */
        if (!rcont) {
            i2c_nack(s->bus);
        }
    } else { /* !READB */
        if (start) {
            /* START or RESTART I2C transaction to requested address. */
            i2c_start_transfer(s->bus, extract32(fbyte, 1, 7),
                               extract32(fbyte, 0, 1));
        } else {
            /* Check for overflow. */
            if (ot_fifo32_is_full(&s->host_tx_fifo)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: TX FIFO overflow\n",
                              __func__, s->ot_id);
                return;
            }

            uint32_t val = 0;
            val = FIELD_DP32(val, FDATA, FBYTE, fbyte);
            val = FIELD_DP32(val, FDATA, NAKOK, nakok);
            /* Add this byte to the TX FIFO. */
            ot_fifo32_push(&s->host_tx_fifo, val);

            /* Check if threshold has been reached. */
            s->host_tx_threshold = ot_i2c_get_fmt_threshold(s);
            if (ot_fifo32_num_used(&s->host_tx_fifo) < s->host_tx_threshold) {
                ot_i2c_irq_set_state(s, FMT_THRESHOLD, true);
            } else {
                /* Reset the cached threshold level. */
                s->host_tx_threshold = 0;
            }

            /* Try to send contents of TX FIFO to the target. */
            ot_i2c_host_send(s);
        }
    }

    if (stop) {
        /* End the transaction. */
        i2c_end_transfer(s->bus);

        /* Signal command completion. */
        ot_i2c_irq_set_state(s, CMD_COMPLETE, true);

        /* Allow target mode to process data. */
        i2c_schedule_pending_master(s->bus);
    }
}

static void ot_i2c_write(void *opaque, hwaddr addr, uint64_t val64,
                         unsigned size)
{
    OtI2CState *s = opaque;
    uint32_t val32 = val64;
    hwaddr reg = R32_OFF(addr);
    uint64_t pc = ibex_get_current_pc();
    uint8_t address, mask;
    (void)size;

    trace_ot_i2c_io_write(s->ot_id, (unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_RW1C_MASK;
        s->regs[reg] &= ~val32;
        ot_i2c_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_i2c_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_i2c_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        s->regs[reg] = val32;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case R_TIMEOUT_CTRL:
    case R_HOST_TIMEOUT_CTRL:
        s->regs[reg] = val32;
        break;
    case R_TARGET_ID:
        if (FIELD_EX32(val32, TARGET_ID, ADDRESS1)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Target address 1 not supported.\n", __func__,
                          s->ot_id);
        }
        address = FIELD_EX32(val32, TARGET_ID, ADDRESS0);
        if ((val32 & R_TARGET_ID_MASK0_MASK) != R_TARGET_ID_MASK0_MASK) {
            qemu_log_mask(
                LOG_UNIMP,
                "%s: %s: Address Mask with any bits unset is not supported.\n",
                __func__, s->ot_id);
            break;
        }
        if (address != 0) {
            ARRAY_FIELD_DP32(s->regs, TARGET_ID, ADDRESS0, address);
            mask = FIELD_EX32(val32, TARGET_ID, MASK0);
            ARRAY_FIELD_DP32(s->regs, TARGET_ID, MASK0, mask);
            /* Update the address of this target on the bus. */
            i2c_slave_set_address(s->target, address);
        }
        break;
    case R_CTRL:
        if (FIELD_EX32(val32, CTRL, LLPBK)) {
            qemu_log_mask(LOG_UNIMP, "%s: %s: Loopback mode not supported.\n",
                          __func__, s->ot_id);
        }
        /*
         * Allow both ENABLEHOST and ENABLETARGET to be set so the
         * host can decide how to configure and use the controller.
         */
        val32 &= R_CTRL_LLPBK_MASK | R_CTRL_ENABLEHOST_MASK |
                 R_CTRL_ENABLETARGET_MASK;
        s->regs[reg] = val32;
        if (s->regs[reg]) {
            /* check timings once, each time one or more timings are updated */
            if (s->check_timings) {
                ot_i2c_check_timings(s);
                s->check_timings = false;
            }
        }
        break;
    case R_FDATA:
        ot_i2c_write_fdata(s, val32);
        break;
    case R_TXDATA:
        ot_i2c_target_write_tx_fifo(s, FIELD_EX8(val32, TXDATA, TXDATA));
        break;
    case R_FIFO_CTRL:
        if (FIELD_EX32(val32, FIFO_CTRL, RXRST)) {
            ot_i2c_host_reset_rx_fifo(s);
        }
        if (FIELD_EX32(val32, FIFO_CTRL, TXRST)) {
            ot_i2c_target_reset_tx_fifo(s);
        }
        if (FIELD_EX32(val32, FIFO_CTRL, FMTRST)) {
            ot_i2c_host_reset_tx_fifo(s);
        }
        if (FIELD_EX32(val32, FIFO_CTRL, ACQRST)) {
            ot_i2c_target_reset_rx_fifo(s);
        }
        break;
    case R_HOST_FIFO_CONFIG:
        ARRAY_FIELD_DP32(s->regs, HOST_FIFO_CONFIG, RX_THRESH,
                         FIELD_EX32(val32, HOST_FIFO_CONFIG, RX_THRESH));
        ARRAY_FIELD_DP32(s->regs, HOST_FIFO_CONFIG, FMT_THRESH,
                         FIELD_EX32(val32, HOST_FIFO_CONFIG, FMT_THRESH));

        ot_i2c_irq_set_state(s, RX_THRESHOLD, ot_i2c_rx_threshold_intr(s));
        ot_i2c_irq_set_state(s, FMT_THRESHOLD, ot_i2c_fmt_threshold_intr(s));
        break;
    case R_TARGET_FIFO_CONFIG:
        ARRAY_FIELD_DP32(s->regs, TARGET_FIFO_CONFIG, TX_THRESH,
                         FIELD_EX32(val32, TARGET_FIFO_CONFIG, TX_THRESH));
        ARRAY_FIELD_DP32(s->regs, TARGET_FIFO_CONFIG, ACQ_THRESH,
                         FIELD_EX32(val32, TARGET_FIFO_CONFIG, ACQ_THRESH));

        ot_i2c_irq_set_state(s, TX_THRESHOLD, ot_i2c_tx_threshold_intr(s));
        ot_i2c_irq_set_state(s, ACQ_THRESHOLD, ot_i2c_acq_threshold_intr(s));
        break;
    case R_OVRD:
        qemu_log_mask(LOG_UNIMP, "%s: %s: register %s is not implemented\n",
                      __func__, s->ot_id, REG_NAME(reg));
        val32 &= R_OVRD_TXOVRDEN_MASK | R_OVRD_SCLVAL_MASK | R_OVRD_SDAVAL_MASK;
        s->regs[reg] = val32;
        break;
    case R_TIMING0:
        val32 &= R_TIMING0_THIGH_MASK | R_TIMING0_TLOW_MASK;
        s->regs[reg] = val32;
        s->check_timings = true;
        break;
    case R_TIMING1:
        val32 &= R_TIMING1_T_R_MASK | R_TIMING1_T_F_MASK;
        s->regs[reg] = val32;
        s->check_timings = true;
        break;
    case R_TIMING2:
        val32 &= R_TIMING2_TSU_STA_MASK | R_TIMING2_THD_STA_MASK;
        s->regs[reg] = val32;
        s->check_timings = true;
        break;
    case R_TIMING3:
        val32 &= R_TIMING3_TSU_DAT_MASK | R_TIMING3_THD_DAT_MASK;
        s->regs[reg] = val32;
        s->check_timings = true;
        break;
    case R_TIMING4:
        val32 &= R_TIMING4_TSU_STO_MASK | R_TIMING4_T_BUF_MASK;
        s->regs[reg] = val32;
        s->check_timings = true;
        break;
    case R_CONTROLLER_EVENTS:
        val32 &= CONTROLLER_EVENTS_RW1C_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_i2c_irq_set_state(s, CONTROLLER_HALT, s->regs[reg] != 0);
        break;
    case R_TARGET_EVENTS:
        val32 &= TARGET_EVENTS_RW1C_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_i2c_irq_set_state(s, TX_STRETCH, s->regs[reg] != 0);
        break;
    case R_TARGET_NACK_COUNT:
    case R_TARGET_ACK_CTRL:
    case R_ACQ_FIFO_NEXT_DATA:
    case R_HOST_NACK_HANDLER_TIMEOUT:
        qemu_log_mask(LOG_UNIMP, "%s: %s: register %s is not implemented\n",
                      __func__, s->ot_id, REG_NAME(reg));
        break;
    case R_STATUS:
    case R_RDATA:
    case R_HOST_FIFO_STATUS:
    case R_TARGET_FIFO_STATUS:
    case R_VAL:
    case R_ACQDATA:
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
}

static void ot_i2c_target_set_acqdata(OtI2CState *s, uint32_t data,
                                      OtI2CSignal signal)
{
    uint32_t val32 = 0;

    if (ot_fifo32_is_full(&s->target_rx_fifo)) {
        i2c_end_transfer(s->bus);
        return;
    }

    /* Set the first byte to the target address + RW bit as 0. */
    val32 = FIELD_DP32(val32, ACQDATA, ABYTE, data);
    /* Indicate that this should send requested signal to the host. */
    val32 = FIELD_DP32(val32, ACQDATA, SIGNAL, signal);
    /* Add this entry to the target receive FIFO. */
    ot_fifo32_push(&s->target_rx_fifo, val32);

    /* See if adding this entry exceeded the threshold. */
    ot_i2c_irq_set_state(s, ACQ_THRESHOLD, ot_i2c_acq_threshold_intr(s));

    trace_ot_i2c_target_set_acqdata(s->ot_id,
                                    ot_fifo32_num_used(&s->target_rx_fifo),
                                    data, signal);
}

static int ot_i2c_target_event(I2CSlave *target, enum i2c_event event)
{
    BusState *abus = qdev_get_parent_bus(DEVICE(target));
    OtI2CState *s = OT_I2C(abus->parent);
    int ret = 0;

    if (!ot_i2c_target_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: I2C target mode not enabled, no event issued\n",
                      __func__, s->ot_id);
        return -1;
    }

    switch (event) {
    case I2C_START_SEND_ASYNC:
        /* Set the first byte to the target address + RW bit as 0. */
        ot_i2c_target_set_acqdata(s, target->address << 1u, SIGNAL_START);
        i2c_ack(s->bus);
        break;
    case I2C_START_RECV:
        /* Set the first byte to the target address + RW bit as 1. */
        ot_i2c_target_set_acqdata(s, target->address << 1u | 1u, SIGNAL_START);
        if (ot_fifo32_num_used(&s->target_rx_fifo) > 1) {
            /*
             * Potentially an unhandled condition in the ACQ fifo. Datasheet
             * says to stretch the clock in this situation so assert that
             * interrupt and let the driver decide what to do.
             */
            ot_i2c_irq_set_state(s, TX_STRETCH, true);
        }
        i2c_ack(s->bus);
        break;
    case I2C_NACK:
        g_assert_not_reached();
        break;
    case I2C_FINISH:
        /* Signal STOP as the last entry in the fifo. */
        ot_i2c_target_set_acqdata(s, 0, SIGNAL_STOP);

        /* Assert command complete interrupt. */
        ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: %s: I2C event %d unimplemented\n",
                      __func__, s->ot_id, event);
        ret = -1;
    }

    return ret;
}

static uint8_t ot_i2c_target_recv(I2CSlave *target)
{
    BusState *abus = qdev_get_parent_bus(DEVICE(target));
    OtI2CState *s = OT_I2C(abus->parent);
    uint8_t data;

    if (!ot_i2c_target_enabled(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: I2C target mode not enabled, no event issued\n",
                      __func__, s->ot_id);
        return 0;
    }

    /* If the FIFO is empty then there is nothing to return. */
    if (fifo8_is_empty(&s->target_tx_fifo)) {
        return 0;
    }

    data = fifo8_pop(&s->target_tx_fifo);
    trace_ot_i2c_target_recv(s->ot_id, fifo8_num_used(&s->target_tx_fifo),
                             data);
    return data;
}

static void ot_i2c_target_send_async(I2CSlave *target, uint8_t data)
{
    BusState *abus = qdev_get_parent_bus(DEVICE(target));
    OtI2CState *s = OT_I2C(abus->parent);

    if (ot_i2c_target_enabled(s)) {
        /* Send data byte with no signal flags. */
        ot_i2c_target_set_acqdata(s, data, SIGNAL_NONE);
        i2c_ack(s->bus);
    }
}

static void ot_i2c_target_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    (void)data;

    dc->desc = "OpenTitan I2C Target";
    sc->event = &ot_i2c_target_event;
    sc->send_async = &ot_i2c_target_send_async;
    sc->recv = &ot_i2c_target_recv;
}

static const TypeInfo ot_i2c_target_info = {
    .name = TYPE_OT_I2C_TARGET,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(OtI2CState),
    .class_init = &ot_i2c_target_class_init,
    .class_size = sizeof(I2CSlaveClass),
};

static const MemoryRegionOps ot_i2c_ops = {
    .read = &ot_i2c_read,
    .write = &ot_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static Property ot_i2c_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtI2CState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtI2CState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtI2CState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_i2c_reset_enter(Object *obj, ResetType type)
{
    OtI2CClass *c = OT_I2C_GET_CLASS(obj);
    OtI2CState *s = OT_I2C(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    i2c_end_transfer(s->bus);

    for (unsigned index = 0; index < ARRAY_SIZE(s->irqs); index++) {
        ibex_irq_set(&s->irqs[index], 0);
    }
    ibex_irq_set(&s->alert, 0);

    memset(s->regs, 0, sizeof(s->regs));

    ot_i2c_host_reset_tx_fifo(s);
    ot_i2c_host_reset_rx_fifo(s);
    ot_i2c_target_reset_tx_fifo(s);
    ot_i2c_target_reset_rx_fifo(s);

    if (!s->clock_src_name) {
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_name =
            ic->get_clock_source(ii, s->clock_name, DEVICE(s), &error_fatal);
        qemu_irq in_irq = qdev_get_gpio_in_named(DEVICE(s), "clock-in", 0);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_name, 0, in_irq);
    }

    s->check_timings = true;
}

static void ot_i2c_realize(DeviceState *dev, Error **errp)
{
    OtI2CState *s = OT_I2C(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    qdev_init_gpio_in_named(DEVICE(s), &ot_i2c_clock_input, "clock-in", 1);

    /* TODO: check if the following can be moved to ot_i2c_init */
    char *bus_name = g_strdup_printf("ot-%s", s->ot_id);
    s->bus = i2c_init_bus(dev, bus_name);
    g_free(bus_name);
    s->target = i2c_slave_create_simple(s->bus, TYPE_OT_I2C_TARGET, 0xff);
}

static void ot_i2c_init(Object *obj)
{
    OtI2CState *s = OT_I2C(obj);

    for (unsigned index = 0; index < ARRAY_SIZE(s->irqs); index++) {
        ibex_sysbus_init_irq(obj, &s->irqs[index]);
    }
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    memory_region_init_io(&s->mmio, obj, &ot_i2c_ops, s, TYPE_OT_I2C,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ot_fifo32_create(&s->host_tx_fifo, OT_I2C_FIFO_SIZE);
    fifo8_create(&s->host_rx_fifo, OT_I2C_FIFO_SIZE);
    fifo8_create(&s->target_tx_fifo, OT_I2C_FIFO_SIZE);
    ot_fifo32_create(&s->target_rx_fifo, OT_I2C_FIFO_SIZE);
}

static void ot_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->desc = "OpenTitan I2C Host";
    dc->realize = ot_i2c_realize;
    device_class_set_props(dc, ot_i2c_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtI2CClass *ic = OT_I2C_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_i2c_reset_enter, NULL, NULL,
                                       &ic->parent_phases);
}

static const TypeInfo ot_i2c_info = {
    .name = TYPE_OT_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtI2CState),
    .instance_init = &ot_i2c_init,
    .class_size = sizeof(OtI2CClass),
    .class_init = &ot_i2c_class_init,
};

static void ot_i2c_register_types(void)
{
    type_register_static(&ot_i2c_info);
    type_register_static(&ot_i2c_target_info);
}

type_init(ot_i2c_register_types);
