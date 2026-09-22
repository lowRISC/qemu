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
 * - Target mode only supports TARGET_ID.ADDRESS0 and TARGET_ID.MASK0
 * - Loopback mode.  Need more details about how it works in HW.
 */

#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "block/aio.h"
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

#define OT_I2C_FIFO_SIZE     64u
#define OT_I2C_ACQ_FIFO_SIZE 268u

typedef enum {
    SIGNAL_NONE,
    SIGNAL_START,
    SIGNAL_STOP,
    SIGNAL_RESTART,
    SIGNAL_NACK,
    SIGNAL_NACK_START,
    SIGNAL_NACK_STOP
} OtI2CSignal;

typedef struct {
    int64_t offset_ns;
    bool scl;
    bool sda;
} OtI2cOvrdStep;

typedef enum {
    OT_I2C_BB_IDLE = 0,
    OT_I2C_BB_ADDR,
    OT_I2C_BB_WRITE_DATA,
    OT_I2C_BB_READ_DATA,
    OT_I2C_BB_IGNORE,
} OtI2cBbTargetState;

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

    /* Target mode first address mask */
    uint8_t address_mask_0;

    /* Address that was matched to us */
    uint8_t matched_address;

    /* Target mode transaction and stretching state */
    bool in_bus_xact;
    bool in_target_xact;
    bool in_target_transfer;
    bool restart_pending;
    bool target_read_mode;
    bool expect_stop;
    bool nack_transaction;
    bool ack_ctrl_stretching;
    bool tx_stretching;
    bool cmd_complete_stretching;
    bool cmd_complete_wait_reenable;
    bool host_pending_stop;
    bool host_pending_nakok;
    uint8_t acq_fifo_next_data;
    int last_ack_result;

    /* Last target address used in host START/RESTART */
    uint8_t active_target_addr;

    /* Virtual clock timestamp when FMT FIFO finishes draining */
    uint64_t fmt_finish_ns;
    QEMUTimer *fmt_timer;

    /* Override bitbang waveform recording state */
    OtI2cOvrdStep ovrd_steps[64];
    unsigned ovrd_len;
    int64_t ovrd_start_ns;
    bool ovrd_consumed;

    /* Target bitbang receiver/transmitter state */
    unsigned target_poll_acq_count;
    bool bb_prev_scl;
    bool bb_prev_sda;
    OtI2cBbTargetState bb_state;
    uint8_t bb_shift_reg;
    unsigned bb_bit_count;
    uint8_t bb_cur_tx_byte;
    bool bb_drive_sda_low;
    bool bb_seen_valid_start;

    uint32_t pclk; /* Current input clock */
    const char *clock_src_name; /* IRQ name once connected */

    char *ot_id;
    char *clock_name;
    DeviceState *clock_src;
};

static OtI2CState *ot_i2c_instances[3];

struct OtI2CClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

struct OtI2CTarget {
    I2CSlave i2c;
};

#define TYPE_PMOD_I2C_SENSOR "pmod-i2c-sensor"
OBJECT_DECLARE_SIMPLE_TYPE(PmodI2CSensorState, PMOD_I2C_SENSOR)

struct PmodI2CSensorState {
    I2CSlave parent_obj;
    uint8_t regs[2048];
    uint16_t reg_ptr;
    uint8_t addr_bytes;
    bool busy_nak;
};

static uint8_t ot_i2c_address_abyte(uint8_t address, bool read)
{
    return (address << 1u) | (read ? 1u : 0);
}

static void ot_i2c_update_irqs(OtI2CState *s)
{
    uint32_t state = s->regs[R_INTR_STATE] | s->regs[R_INTR_TEST];
    uint32_t state_masked = state & s->regs[R_INTR_ENABLE];

    if (state || s->regs[R_INTR_ENABLE]) {
        trace_ot_i2c_update_irqs(s->ot_id, state, s->regs[R_INTR_ENABLE],
                                 state_masked);
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
        if (irq == CMD_COMPLETE) {
            s->cmd_complete_stretching = true;
            s->cmd_complete_wait_reenable =
                (s->regs[R_INTR_ENABLE] & INTR_CMD_COMPLETE_MASK) != 0;
        }
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

static uint64_t ot_i2c_get_byte_ns(const OtI2CState *s)
{
    uint32_t cycles = FIELD_EX32(s->regs[R_TIMING0], TIMING0, THIGH) +
                      FIELD_EX32(s->regs[R_TIMING0], TIMING0, TLOW);
    if (!s->pclk || !cycles) {
        return 10000ULL;
    }
    return ((uint64_t)cycles * 9ULL * NANOSECONDS_PER_SECOND) / s->pclk;
}

static uint32_t ot_i2c_get_fmt_depth(const OtI2CState *s)
{
    uint32_t queued = ot_fifo32_num_used(&s->host_tx_fifo);
    int64_t rem_ns =
        (int64_t)s->fmt_finish_ns - qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    if (ot_i2c_host_enabled(s) && rem_ns > 0) {
        uint64_t byte_ns = MAX(ot_i2c_get_byte_ns(s), 1ULL);
        queued += (uint32_t)(((uint64_t)rem_ns + byte_ns - 1ULL) / byte_ns);
    }
    return MIN(queued, OT_I2C_FIFO_SIZE);
}

static bool ot_i2c_fmt_threshold_intr(OtI2CState *s)
{
    uint32_t thresh = ot_i2c_get_fmt_threshold(s);
    return ot_i2c_get_fmt_depth(s) < thresh;
}

static void ot_i2c_update_fmt_threshold(OtI2CState *s)
{
    bool below = ot_i2c_fmt_threshold_intr(s);
    ot_i2c_irq_set_state(s, FMT_THRESHOLD, below);
    if (s->fmt_timer) {
        int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
        if ((int64_t)s->fmt_finish_ns > now) {
            uint64_t next_ns = s->fmt_finish_ns;
            uint32_t thresh = ot_i2c_get_fmt_threshold(s);
            if (!below && thresh > 0) {
                uint64_t drop_ns = s->fmt_finish_ns - (uint64_t)(thresh - 1u) *
                                                          ot_i2c_get_byte_ns(s);
                if ((int64_t)drop_ns > now && drop_ns < next_ns) {
                    next_ns = drop_ns;
                }
            }
            timer_mod(s->fmt_timer, (int64_t)next_ns);
        } else {
            timer_del(s->fmt_timer);
        }
    }
}

static void ot_i2c_fmt_timer_cb(void *opaque)
{
    OtI2CState *s = opaque;
    ot_i2c_update_fmt_threshold(s);
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


static void ot_i2c_target_set_acqdata(OtI2CState *s, uint32_t data,
                                      OtI2CSignal signal);

static bool ot_i2c_should_tx_stretch(OtI2CState *s)
{
    if (!s->target_read_mode) {
        return false;
    }
    return fifo8_is_empty(&s->target_tx_fifo) ||
           (s->regs[R_TARGET_EVENTS] != 0) ||
           (ot_fifo32_num_used(&s->target_rx_fifo) > 1);
}

static void ot_i2c_check_clear_tx_stretch(OtI2CState *s)
{
    if (s->tx_stretching && !ot_i2c_should_tx_stretch(s)) {
        s->tx_stretching = false;
        ot_i2c_irq_set_state(s, TX_STRETCH, false);
    }
}

static void ot_i2c_host_reset_tx_fifo(OtI2CState *s)
{
    ot_fifo32_reset(&s->host_tx_fifo);
    s->fmt_finish_ns = 0;
    s->host_tx_threshold = 0;
    ot_i2c_update_fmt_threshold(s);
}

static void ot_i2c_host_reset_rx_fifo(OtI2CState *s)
{
    fifo8_reset(&s->host_rx_fifo);
    ot_i2c_irq_set_state(s, RX_THRESHOLD, ot_i2c_rx_threshold_intr(s));
}

static void ot_i2c_target_reset_tx_fifo(OtI2CState *s)
{
    fifo8_reset(&s->target_tx_fifo);
    ot_i2c_irq_set_state(s, TX_THRESHOLD, ot_i2c_tx_threshold_intr(s));
}

static void ot_i2c_target_reset_rx_fifo(OtI2CState *s)
{
    ot_fifo32_reset(&s->target_rx_fifo);
    ot_i2c_irq_set_state(s, ACQ_THRESHOLD, ot_i2c_acq_threshold_intr(s));
    ot_i2c_check_clear_tx_stretch(s);
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
    /* Handle a full FIFO. */
    if (fifo8_is_full(&s->target_tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Target TX FIFO overflow\n",
                      __func__, s->ot_id);
    } else {
        /* Add this entry to the FIFO. */
        fifo8_push(&s->target_tx_fifo, val);
    }

    ot_i2c_irq_set_state(s, TX_THRESHOLD, ot_i2c_tx_threshold_intr(s));
    ot_i2c_check_clear_tx_stretch(s);
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

static void ot_i2c_pump_async_bus(OtI2CState *s)
{
    static bool pumping;
    if (pumping || !s || !s->bus) {
        return;
    }
    pumping = true;
    int max_iters = 32;
    while ((s->bus->bh != NULL || !QSIMPLEQ_EMPTY(&s->bus->pending_masters)) &&
           max_iters-- > 0) {
        if (!aio_bh_poll(qemu_get_aio_context())) {
            break;
        }
    }
    pumping = false;
}

static uint64_t ot_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    OtI2CState *s = opaque;
    uint32_t val32 = 0;
    hwaddr reg = R32_OFF(addr);
    (void)size;

    ot_i2c_pump_async_bus(s);

    switch (reg) {
    case R_INTR_STATE:
        ot_i2c_update_fmt_threshold(s);
        val32 = s->regs[R_INTR_STATE] | s->regs[R_INTR_TEST];
        break;
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
    case R_STATUS: {
        ot_i2c_update_fmt_threshold(s);
        uint32_t fmt_depth = ot_i2c_get_fmt_depth(s);
        val32 = FIELD_DP32(val32, STATUS, HOSTIDLE,
                           !i2c_bus_busy(s->bus) &&
                               (!ot_i2c_host_enabled(s) || fmt_depth == 0));
        val32 = FIELD_DP32(val32, STATUS, TARGETIDLE,
                           !s->in_target_transfer && !i2c_bus_busy(s->bus));

        /* Report host TX FIFO status. */
        if (fmt_depth == 0) {
            val32 = FIELD_DP32(val32, STATUS, FMTEMPTY, 1u);
        }
        if (fmt_depth >= OT_I2C_FIFO_SIZE) {
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

        /* Report target RX (ACQ) FIFO status. */
        if (ot_fifo32_is_empty(&s->target_rx_fifo)) {
            val32 = FIELD_DP32(val32, STATUS, ACQEMPTY, 1u);
        }
        if (ot_fifo32_num_used(&s->target_rx_fifo) >=
            (OT_I2C_ACQ_FIFO_SIZE - 2u)) {
            val32 = FIELD_DP32(val32, STATUS, ACQFULL, 1u);
        }
        if (s->ack_ctrl_stretching) {
            val32 = FIELD_DP32(val32, STATUS, ACK_CTRL_STRETCH, 1u);
        }
        break;
    }
    case R_RDATA:
        val32 = (uint32_t)ot_i2c_host_read_rx_fifo(s);
        ot_i2c_irq_set_state(s, RX_THRESHOLD, ot_i2c_rx_threshold_intr(s));
        break;
    case R_ACQDATA:
        val32 = (uint32_t)ot_i2c_target_read_rx_fifo(s);
        /* Deassert level interrupt state if FIFO is no longer above the
         * threshold. */
        ot_i2c_irq_set_state(s, ACQ_THRESHOLD, ot_i2c_acq_threshold_intr(s));
        ot_i2c_check_clear_tx_stretch(s);
        break;
    case R_HOST_FIFO_STATUS:
        val32 = FIELD_DP32(val32, HOST_FIFO_STATUS, FMTLVL,
                           ot_i2c_get_fmt_depth(s));
        val32 = FIELD_DP32(val32, HOST_FIFO_STATUS, RXLVL,
                           fifo8_num_used(&s->host_rx_fifo));
        break;
    case R_TARGET_FIFO_STATUS:
        if (ot_i2c_target_enabled(s)) {
            s->target_poll_acq_count++;
        }
        val32 = FIELD_DP32(val32, TARGET_FIFO_STATUS, TXLVL,
                           fifo8_num_used(&s->target_tx_fifo));
        val32 = FIELD_DP32(val32, TARGET_FIFO_STATUS, ACQLVL,
                           ot_fifo32_num_used(&s->target_rx_fifo));
        break;
    case R_VAL: {
        bool scl = true;
        bool sda = true;
        if (s->regs[R_OVRD] & R_OVRD_TXOVRDEN_MASK) {
            scl = (bool)(s->regs[R_OVRD] & R_OVRD_SCLVAL_MASK);
            sda = (bool)(s->regs[R_OVRD] & R_OVRD_SDAVAL_MASK);
        }
        val32 = FIELD_DP32(0u, VAL, SCL_RX, scl ? 0xffffu : 0u);
        val32 = FIELD_DP32(val32, VAL, SDA_RX, sda ? 0xffffu : 0u);
        break;
    }
    case R_OVRD:
    case R_TIMING0:
    case R_TIMING1:
    case R_TIMING2:
    case R_TIMING3:
    case R_TIMING4:
    case R_TARGET_TIMEOUT_CTRL:
    case R_TARGET_ACK_CTRL:
    case R_HOST_NACK_HANDLER_TIMEOUT:
        val32 = s->regs[reg];
        break;
    case R_TARGET_NACK_COUNT:
        val32 = s->regs[reg] & R_TARGET_NACK_COUNT_TARGET_NACK_COUNT_MASK;
        s->regs[reg] = 0u;
        break;
    case R_ACQ_FIFO_NEXT_DATA:
        val32 = s->acq_fifo_next_data & 0xffu;
        break;
    case R_FIFO_CTRL:
        val32 = 0;
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_FDATA:
    case R_TXDATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: W/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
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

static uint64_t ot_i2c_get_target_stretch_ns(OtI2CState *s, bool is_read)
{
    I2CNode *node;
    QLIST_FOREACH(node, &s->bus->current_devs, next) {
        PmodI2CSensorState *dev = (PmodI2CSensorState *)
            object_dynamic_cast(OBJECT(node->elt), TYPE_PMOD_I2C_SENSOR);
        if (dev && dev->parent_obj.address == 0x22u) {
            return (uint64_t)dev->regs[is_read ? 0xdcu : 0xdbu] * 1000000ULL;
        }
    }
    return 0;
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
        if (!ot_fifo32_is_full(&s->host_tx_fifo)) {
            ot_fifo32_push(&s->host_tx_fifo, fdata);
        }
        ot_i2c_update_fmt_threshold(s);
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
        }

        /* Read bytes from target device into host_rx_fifo. */
        do {
            if (fifo8_is_full(&s->host_rx_fifo)) {
                ot_i2c_irq_set_state(s, RX_OVERFLOW, true);
                while (bytes_to_read--) {
                    (void)i2c_recv(s->bus);
                }
                break;
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
            bool is_restart = i2c_bus_busy(s->bus);
            uint8_t addr = extract32(fbyte, 1, 7);
            if (is_restart && s->active_target_addr != addr) {
                i2c_end_transfer(s->bus);
            }
            s->active_target_addr = addr;
            if (is_restart) {
                ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
            }
            /* START or RESTART I2C transaction to requested address. */
            if (i2c_start_transfer(s->bus, addr, extract32(fbyte, 0, 1)) &&
                !nakok) {
                ARRAY_FIELD_DP32(s->regs, CONTROLLER_EVENTS, NACK, 1);
                ot_i2c_irq_set_state(s, CONTROLLER_HALT, true);
            }
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

            /* Try to send contents of TX FIFO to the target. */
            ot_i2c_host_send(s);
            if (s->ack_ctrl_stretching) {
                s->host_pending_nakok = nakok;
            }
        }
    }

    uint64_t stretch_ns = 0;
    if (readb) {
        stretch_ns = ot_i2c_get_target_stretch_ns(s, true);
    } else if (!start) {
        stretch_ns = ot_i2c_get_target_stretch_ns(s, false);
    }
    if (stretch_ns > 0 &&
        FIELD_EX32(s->regs[R_TIMEOUT_CTRL], TIMEOUT_CTRL, EN)) {
        uint32_t val = FIELD_EX32(s->regs[R_TIMEOUT_CTRL], TIMEOUT_CTRL, VAL);
        uint64_t timeout_ns =
            s->pclk ? ((uint64_t)val * NANOSECONDS_PER_SECOND / s->pclk) : 0;
        if (stretch_ns > timeout_ns &&
            !FIELD_EX32(s->regs[R_TIMEOUT_CTRL], TIMEOUT_CTRL, MODE)) {
            ot_i2c_irq_set_state(s, STRETCH_TIMEOUT, true);
        }
    }

    if (stop) {
        if (s->ack_ctrl_stretching) {
            s->host_pending_stop = true;
        } else {
            /* End the transaction. */
            i2c_end_transfer(s->bus);

            /* Signal command completion. */
            ot_i2c_irq_set_state(s, CMD_COMPLETE, true);

            /* Allow target mode to process data. */
            i2c_schedule_pending_master(s->bus);
            ot_i2c_pump_async_bus(s);
        }
    }

    uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    if (s->fmt_finish_ns < now) {
        s->fmt_finish_ns = now;
    }
    unsigned count = (readb && fbyte > 0) ? fbyte : 1u;
    s->fmt_finish_ns += (uint64_t)count * ot_i2c_get_byte_ns(s);
    ot_i2c_update_fmt_threshold(s);
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
        s->regs[R_INTR_TEST] = val32 & ~INTR_RW1C_MASK;
        s->regs[R_INTR_STATE] |= val32 & INTR_RW1C_MASK;
        ot_i2c_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        s->regs[reg] = val32;
        if (val32) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        break;
    case R_HOST_TIMEOUT_CTRL:
        s->regs[reg] = val32 & R_HOST_TIMEOUT_CTRL_HOST_TIMEOUT_CTRL_MASK;
        break;
    case R_TIMEOUT_CTRL:
    case R_TARGET_TIMEOUT_CTRL:
    case R_HOST_NACK_HANDLER_TIMEOUT:
        s->regs[reg] = val32;
        break;
    case R_TARGET_ID:
        s->regs[R_TARGET_ID] =
            val32 & (R_TARGET_ID_ADDRESS0_MASK | R_TARGET_ID_MASK0_MASK |
                     R_TARGET_ID_ADDRESS1_MASK | R_TARGET_ID_MASK1_MASK);
        address = FIELD_EX32(val32, TARGET_ID, ADDRESS0);
        mask = FIELD_EX32(val32, TARGET_ID, MASK0);
        /* Update the address mask of this target on the bus. */
        s->address_mask_0 = (uint8_t)mask;
        if (address != 0u) {
            /* Update the address of this target on the bus. */
            i2c_slave_set_address(s->target, address);
        }
        break;
    case R_CTRL: {
        bool was_host_enabled = ot_i2c_host_enabled(s);
        if (FIELD_EX32(val32, CTRL, LLPBK)) {
            qemu_log_mask(LOG_UNIMP, "%s: %s: Loopback mode not supported.\n",
                          __func__, s->ot_id);
        }
        /*
         * Allow both ENABLEHOST and ENABLETARGET to be set so the
         * host can decide how to configure and use the controller.
         */
        val32 &=
            R_CTRL_ENABLEHOST_MASK | R_CTRL_ENABLETARGET_MASK |
            R_CTRL_LLPBK_MASK | R_CTRL_NACK_ADDR_AFTER_TIMEOUT_MASK |
            R_CTRL_ACK_CTRL_EN_MASK | R_CTRL_MULTI_CONTROLLER_MONITOR_EN_MASK |
            R_CTRL_TX_STRETCH_CTRL_EN_MASK;
        s->regs[reg] = val32;
        if (!ot_i2c_host_enabled(s)) {
            s->fmt_finish_ns = 0;
            ot_i2c_update_fmt_threshold(s);
            if (i2c_bus_busy(s->bus)) {
                i2c_end_transfer(s->bus);
            }
        } else if (!was_host_enabled && !ot_fifo32_is_empty(&s->host_tx_fifo)) {
            uint32_t queued_fdata[OT_I2C_FIFO_SIZE];
            uint32_t n_queued = 0;
            while (!ot_fifo32_is_empty(&s->host_tx_fifo) &&
                   n_queued < OT_I2C_FIFO_SIZE) {
                queued_fdata[n_queued++] = ot_fifo32_pop(&s->host_tx_fifo);
            }
            for (uint32_t i = 0; i < n_queued; i++) {
                ot_i2c_write_fdata(s, queued_fdata[i]);
            }
        }
        if (s->regs[reg]) {
            /* check timings once, each time one or more timings are updated */
            if (s->check_timings) {
                ot_i2c_check_timings(s);
                s->check_timings = false;
            }
        }
        break;
    }
    case R_FDATA:
        ot_i2c_write_fdata(s, val32);
        break;
    case R_TXDATA:
        s->target_poll_acq_count = 0;
        ot_i2c_target_write_tx_fifo(s, FIELD_EX8(val32, TXDATA, TXDATA));
        break;
    case R_FIFO_CTRL:
        s->target_poll_acq_count = 0;
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
        ot_i2c_update_fmt_threshold(s);
        break;
    case R_TARGET_FIFO_CONFIG:
        ARRAY_FIELD_DP32(s->regs, TARGET_FIFO_CONFIG, TX_THRESH,
                         FIELD_EX32(val32, TARGET_FIFO_CONFIG, TX_THRESH));
        ARRAY_FIELD_DP32(s->regs, TARGET_FIFO_CONFIG, ACQ_THRESH,
                         FIELD_EX32(val32, TARGET_FIFO_CONFIG, ACQ_THRESH));

        ot_i2c_irq_set_state(s, TX_THRESHOLD, ot_i2c_tx_threshold_intr(s));
        ot_i2c_irq_set_state(s, ACQ_THRESHOLD, ot_i2c_acq_threshold_intr(s));
        break;
    case R_OVRD: {
        val32 &= R_OVRD_TXOVRDEN_MASK | R_OVRD_SCLVAL_MASK | R_OVRD_SDAVAL_MASK;
        s->regs[reg] = val32;
        if (val32 & R_OVRD_TXOVRDEN_MASK) {
            bool scl = (bool)(val32 & R_OVRD_SCLVAL_MASK);
            bool sda = (bool)(val32 & R_OVRD_SDAVAL_MASK);
            if (!s->ovrd_consumed) {
                if (s->ovrd_len == 0) {
                    if (scl && sda) {
                        s->ovrd_steps[0] = (OtI2cOvrdStep){ 0, scl, sda };
                        s->ovrd_len = 1u;
                        s->ovrd_start_ns = INT64_MAX;
                    }
                } else if (s->ovrd_len < ARRAY_SIZE(s->ovrd_steps)) {
                    int64_t prev_ns = s->ovrd_steps[s->ovrd_len - 1u].offset_ns;
                    bool prev_scl = s->ovrd_steps[s->ovrd_len - 1u].scl;
                    int64_t delta = (prev_scl && !scl && s->ovrd_len > 3u) ?
                                        40000LL :
                                        20000LL;
                    s->ovrd_steps[s->ovrd_len++] =
                        (OtI2cOvrdStep){ prev_ns + delta, scl, sda };
                }
            }
        } else {
            s->ovrd_len = 0;
            s->ovrd_consumed = false;
            s->ovrd_start_ns = INT64_MAX;
        }
        break;
    }
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
        ot_i2c_check_clear_tx_stretch(s);
        break;
    case R_TARGET_ACK_CTRL: {
        bool nack = FIELD_EX32(val32, TARGET_ACK_CTRL, NACK) != 0;
        uint16_t nbytes = nack ? 0 : FIELD_EX32(val32, TARGET_ACK_CTRL, NBYTES);
        if (nack) {
            s->nack_transaction = true;
        }
        if (s->ack_ctrl_stretching && (nack || nbytes > 0)) {
            s->ack_ctrl_stretching = false;
            ot_i2c_irq_set_state(s, ACQ_STRETCH, false);
            if (nack && ot_i2c_host_enabled(s) && i2c_bus_busy(s->bus)) {
                uint32_t fval =
                    FIELD_DP32(0, FDATA, FBYTE, s->acq_fifo_next_data);
                fval = FIELD_DP32(fval, FDATA, NAKOK, s->host_pending_nakok);
                ot_fifo32_push(&s->host_tx_fifo, fval);
                ot_i2c_host_send(s);
            } else {
                ot_i2c_target_set_acqdata(s, s->acq_fifo_next_data,
                                          nack ? SIGNAL_NACK : SIGNAL_NONE);
                s->last_ack_result = nack ? -1 : 0;
                if (!nack) {
                    nbytes--;
                }
            }
            if (s->host_pending_stop) {
                s->host_pending_stop = false;
                i2c_end_transfer(s->bus);
                ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
                i2c_schedule_pending_master(s->bus);
                ot_i2c_pump_async_bus(s);
            }
        }
        ARRAY_FIELD_DP32(s->regs, TARGET_ACK_CTRL, NBYTES, nbytes);
        break;
    }
    case R_STATUS:
    case R_RDATA:
    case R_HOST_FIFO_STATUS:
    case R_TARGET_FIFO_STATUS:
    case R_VAL:
    case R_ACQDATA:
    case R_TARGET_NACK_COUNT:
    case R_ACQ_FIFO_NEXT_DATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: R/O register 0x%02x (%s)\n",
                      __func__, s->ot_id, (uint32_t)addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Bad offset 0x%02x\n", __func__,
                      s->ot_id, (uint32_t)addr);
        break;
    }
}

static void ot_i2c_target_set_acqdata(OtI2CState *s, uint32_t data,
                                      OtI2CSignal signal)
{
    uint32_t val32 = 0;

    if (signal == SIGNAL_NACK || signal == SIGNAL_NACK_START) {
        if (s->regs[R_TARGET_NACK_COUNT] < 0xffu) {
            s->regs[R_TARGET_NACK_COUNT]++;
        }
    }

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

static bool ot_i2c_check_address_match(const OtI2CState *s, uint8_t address)
{
    uint8_t addr0 = ARRAY_FIELD_EX32(s->regs, TARGET_ID, ADDRESS0);
    uint8_t mask0 = ARRAY_FIELD_EX32(s->regs, TARGET_ID, MASK0);
    uint8_t addr1 = ARRAY_FIELD_EX32(s->regs, TARGET_ID, ADDRESS1);
    uint8_t mask1 = ARRAY_FIELD_EX32(s->regs, TARGET_ID, MASK1);

    bool match0 = mask0 && ((address & mask0) == (addr0 & mask0));
    bool match1 = mask1 && ((address & mask1) == (addr1 & mask1));
    return match0 || match1;
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
    case I2C_START_SEND:
    case I2C_START_SEND_ASYNC:
    case I2C_START_RECV: {
        bool is_restart = s->in_bus_xact || s->restart_pending;
        if (s->in_target_transfer && !s->restart_pending) {
            ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
        }
        s->in_bus_xact = true;
        s->restart_pending = false;
        if (!ot_i2c_check_address_match(s, s->matched_address)) {
            s->in_target_transfer = false;
            s->target_read_mode = false;
            return -1;
        }
        bool is_recv = (event == I2C_START_RECV);
        OtI2CSignal sig = is_restart ? SIGNAL_RESTART : SIGNAL_START;
        s->in_target_xact = true;
        s->in_target_transfer = true;
        s->expect_stop = false;
        s->nack_transaction = false;
        s->ack_ctrl_stretching = false;
        ot_i2c_irq_set_state(s, ACQ_STRETCH, false);
        ARRAY_FIELD_DP32(s->regs, TARGET_ACK_CTRL, NBYTES, 0);

        ot_i2c_target_set_acqdata(s,
                                  ot_i2c_address_abyte(s->matched_address,
                                                       is_recv),
                                  sig);
        if (is_recv) {
            s->target_read_mode = true;
            if (ARRAY_FIELD_EX32(s->regs, CTRL, TX_STRETCH_CTRL_EN)) {
                ARRAY_FIELD_DP32(s->regs, TARGET_EVENTS, TX_PENDING, 1);
            }
            if (ot_i2c_should_tx_stretch(s)) {
                s->tx_stretching = true;
                ot_i2c_irq_set_state(s, TX_STRETCH, true);
            }
            i2c_ack(s->bus);
        } else {
            s->target_read_mode = false;
            s->tx_stretching = false;
            ot_i2c_irq_set_state(s, TX_STRETCH, false);
            if (event == I2C_START_SEND_ASYNC) {
                i2c_ack(s->bus);
            }
        }
        break;
    }
    case I2C_NACK:
        /* Host NACKs the last byte of a target read transfer before STOP. */
        s->expect_stop = true;
        break;
    case I2C_FINISH:
        if (s->in_target_transfer) {
            if (s->target_read_mode && !s->expect_stop) {
                ot_i2c_irq_set_state(s, UNEXP_STOP, true);
            }
            /* Assert command complete interrupt. */
            ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
        }
        if (s->in_target_xact) {
            /* Signal STOP or NACK_STOP as the last entry in the fifo. */
            ot_i2c_target_set_acqdata(s, 0,
                                      s->nack_transaction ? SIGNAL_NACK_STOP :
                                                            SIGNAL_STOP);
        }
        s->in_bus_xact = false;
        s->in_target_xact = false;
        s->in_target_transfer = false;
        s->restart_pending = false;
        s->target_read_mode = false;
        s->expect_stop = false;
        s->ack_ctrl_stretching = false;
        s->tx_stretching = false;
        s->host_pending_stop = false;
        s->host_pending_nakok = false;
        ot_i2c_irq_set_state(s, ACQ_STRETCH, false);
        ot_i2c_irq_set_state(s, TX_STRETCH, false);
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

    /* pop FIFO and check threshold. */
    data = fifo8_pop(&s->target_tx_fifo);
    ot_i2c_irq_set_state(s, TX_THRESHOLD, ot_i2c_tx_threshold_intr(s));

    trace_ot_i2c_target_recv(s->ot_id, fifo8_num_used(&s->target_tx_fifo),
                             data);
    return data;
}

static int ot_i2c_target_send(I2CSlave *target, uint8_t data)
{
    BusState *abus = qdev_get_parent_bus(DEVICE(target));
    OtI2CState *s = OT_I2C(abus->parent);
    if (!ot_i2c_target_enabled(s)) {
        return -1;
    }
    if (s->nack_transaction) {
        ot_i2c_target_set_acqdata(s, data, SIGNAL_NACK);
        s->last_ack_result = -1;
        return -1;
    }
    if (ARRAY_FIELD_EX32(s->regs, CTRL, ACK_CTRL_EN)) {
        uint16_t nbytes = ARRAY_FIELD_EX32(s->regs, TARGET_ACK_CTRL, NBYTES);
        if (nbytes > 0) {
            ARRAY_FIELD_DP32(s->regs, TARGET_ACK_CTRL, NBYTES, nbytes - 1);
            ot_i2c_target_set_acqdata(s, data, SIGNAL_NONE);
            s->last_ack_result = 0;
            return 0;
        }
        s->acq_fifo_next_data = data;
        s->ack_ctrl_stretching = true;
        ot_i2c_irq_set_state(s, ACQ_STRETCH, true);
        return 0;
    }

    ot_i2c_target_set_acqdata(s, data, SIGNAL_NONE);
    s->last_ack_result = 0;
    return 0;
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

bool ot_i2c_bus_target_is_stretching(I2CBus *bus)
{
    OtI2CState *s = OT_I2C(BUS(bus)->parent);
    if (s->cmd_complete_stretching) {
        if (s->cmd_complete_wait_reenable) {
            if (!(s->regs[R_INTR_STATE] & INTR_CMD_COMPLETE_MASK) &&
                (s->regs[R_INTR_ENABLE] & INTR_CMD_COMPLETE_MASK) &&
                (s->in_target_transfer ||
                 ot_fifo32_is_empty(&s->target_rx_fifo))) {
                s->cmd_complete_stretching = false;
            }
        } else if (!(!s->in_target_transfer &&
                     (s->regs[R_INTR_STATE] & INTR_CMD_COMPLETE_MASK) &&
                     !ot_fifo32_is_empty(&s->target_rx_fifo))) {
            s->cmd_complete_stretching = false;
        }
    }
    return s->ack_ctrl_stretching || s->tx_stretching ||
           s->cmd_complete_stretching ||
           (ot_fifo32_num_used(&s->target_rx_fifo) >= 64u);
}

int ot_i2c_bus_target_get_last_ack(I2CBus *bus)
{
    OtI2CState *s = OT_I2C(BUS(bus)->parent);
    return s->last_ack_result;
}

bool ot_i2c_bus_target_check_tx_stretch(I2CBus *bus)
{
    OtI2CState *s = OT_I2C(BUS(bus)->parent);
    if (ot_i2c_should_tx_stretch(s)) {
        s->tx_stretching = true;
        ot_i2c_irq_set_state(s, TX_STRETCH, true);
        return true;
    }
    return false;
}

void ot_i2c_bus_target_repeated_start(I2CBus *bus)
{
    OtI2CState *s = OT_I2C(BUS(bus)->parent);
    if (s->in_target_transfer && !s->restart_pending) {
        ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
        s->restart_pending = true;
    }
}

static bool ot_i2c_target_match_and_add(I2CSlave *candidate, uint8_t address,
                                        bool broadcast,
                                        I2CNodeList *current_devs)
{
    BusState *abus = qdev_get_parent_bus(DEVICE(candidate));
    OtI2CState *s = OT_I2C(abus->parent);

    /* Check address, subject to address masking. */
    if (broadcast || ot_i2c_check_address_match(s, address)) {
        /*
         * Store the address that successfully matched to
         * use the correct address for start condition
         */
        s->matched_address = address;

        I2CNode *node = g_new0(struct I2CNode, 1u);
        node->elt = candidate;
        QLIST_INSERT_HEAD(current_devs, node, next);
        return true;
    }

    /* Not found and not broadcast. */
    return false;
}

static void ot_i2c_target_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    (void)data;

    dc->desc = "OpenTitan I2C Target";
    sc->match_and_add = &ot_i2c_target_match_and_add;
    sc->event = &ot_i2c_target_event;
    sc->send = &ot_i2c_target_send;
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

static const uint8_t I2C_PERMIT[REGS_COUNT] = {
    [R_INTR_STATE] = 0x3u,
    [R_INTR_ENABLE] = 0x3u,
    [R_INTR_TEST] = 0x3u,
    [R_ALERT_TEST] = 0x1u,
    [R_CTRL] = 0x1u,
    [R_STATUS] = 0x3u,
    [R_RDATA] = 0x1u,
    [R_FDATA] = 0x3u,
    [R_FIFO_CTRL] = 0x3u,
    [R_HOST_FIFO_CONFIG] = 0xfu,
    [R_TARGET_FIFO_CONFIG] = 0xfu,
    [R_HOST_FIFO_STATUS] = 0xfu,
    [R_TARGET_FIFO_STATUS] = 0xfu,
    [R_OVRD] = 0x1u,
    [R_VAL] = 0xfu,
    [R_TIMING0] = 0xfu,
    [R_TIMING1] = 0xfu,
    [R_TIMING2] = 0xfu,
    [R_TIMING3] = 0xfu,
    [R_TIMING4] = 0xfu,
    [R_TIMEOUT_CTRL] = 0xfu,
    [R_TARGET_ID] = 0xfu,
    [R_ACQDATA] = 0x3u,
    [R_TXDATA] = 0x1u,
    [R_HOST_TIMEOUT_CTRL] = 0x7u,
    [R_TARGET_TIMEOUT_CTRL] = 0xfu,
    [R_TARGET_NACK_COUNT] = 0x1u,
    [R_TARGET_ACK_CTRL] = 0xfu,
    [R_ACQ_FIFO_NEXT_DATA] = 0x1u,
    [R_HOST_NACK_HANDLER_TIMEOUT] = 0xfu,
    [R_CONTROLLER_EVENTS] = 0x1u,
    [R_TARGET_EVENTS] = 0x1u,
};

static bool ot_i2c_accepts(void *opaque, hwaddr addr, unsigned size,
                           bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    uint32_t reg = (uint32_t)(addr >> 2u);
    uint8_t reg_be = (uint8_t)(((1u << size) - 1u) << (addr & 3u));
    return reg < REGS_COUNT && (!is_write || (I2C_PERMIT[reg] & ~reg_be) == 0u);
}

static const MemoryRegionOps ot_i2c_ops = {
    .read = &ot_i2c_read,
    .write = &ot_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.accepts = &ot_i2c_accepts,
};

static const Property ot_i2c_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtI2CState, ot_id),
    DEFINE_PROP_STRING("clock-name", OtI2CState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtI2CState, clock_src, TYPE_DEVICE,
                     DeviceState *),
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
    s->address_mask_0 = 0x0u;
    s->matched_address = 0x0u;
    s->in_bus_xact = false;
    s->in_target_xact = false;
    s->in_target_transfer = false;
    s->restart_pending = false;
    s->target_read_mode = false;
    s->expect_stop = false;
    s->nack_transaction = false;
    s->ack_ctrl_stretching = false;
    s->tx_stretching = false;
    s->cmd_complete_stretching = false;
    s->cmd_complete_wait_reenable = false;
    s->host_pending_stop = false;
    s->host_pending_nakok = false;
    s->acq_fifo_next_data = 0;
    s->last_ack_result = 0;
    s->active_target_addr = 0x0u;
    s->ovrd_len = 0;
    s->ovrd_start_ns = INT64_MAX;
    s->ovrd_consumed = false;
    ot_i2c_reset_bitbang_target(s);
}

I2CBus *ot_i2c_get_active_target_bus(I2CBus *default_bus, uint8_t address)
{
    I2CBus *bus = default_bus;
    OtI2CState *def = bus ? OT_I2C(BUS(bus)->parent) : NULL;
    if (!(def && ot_i2c_target_enabled(def) &&
          ot_i2c_check_address_match(def, address))) {
        for (unsigned i = 0; i < ARRAY_SIZE(ot_i2c_instances); i++) {
            OtI2CState *inst = ot_i2c_instances[i];
            if (inst && ot_i2c_target_enabled(inst) &&
                ot_i2c_check_address_match(inst, address)) {
                bus = inst->bus;
                break;
            }
        }
    }
    if (bus) {
        OT_I2C(BUS(bus)->parent)->matched_address = address;
    }
    return bus;
}

bool ot_i2c_is_target_enabled(OtI2CState *s)
{
    return s && ot_i2c_target_enabled(s);
}

bool ot_i2c_is_target_polling_acq(OtI2CState *s)
{
    return s && s->target_poll_acq_count >= 2u;
}

void ot_i2c_reset_bitbang_target(OtI2CState *s)
{
    if (!s) {
        return;
    }
    s->target_poll_acq_count = 0;
    s->bb_prev_scl = true;
    s->bb_prev_sda = true;
    s->bb_state = OT_I2C_BB_IDLE;
    s->bb_shift_reg = 0;
    s->bb_bit_count = 0;
    s->bb_cur_tx_byte = 0xffu;
    s->bb_drive_sda_low = false;
    s->bb_seen_valid_start = false;
}

bool ot_i2c_bitbang_target_step(OtI2CState *s, bool scl, bool sda_in)
{
    if (!s || !ot_i2c_target_enabled(s)) {
        return sda_in;
    }

    /* 1. Check START / STOP conditions while SCL is high */
    if (s->bb_prev_scl && scl) {
        if (s->bb_prev_sda && !sda_in) {
            /* START or REPEATED START condition */
            s->bb_state = OT_I2C_BB_ADDR;
            s->bb_bit_count = 0;
            s->bb_shift_reg = 0;
            s->bb_drive_sda_low = false;
        } else if (!s->bb_prev_sda && sda_in) {
            /* STOP condition */
            if (s->bb_seen_valid_start) {
                ot_i2c_target_set_acqdata(s, 0, SIGNAL_STOP);
                ot_i2c_irq_set_state(s, CMD_COMPLETE, true);
            }
            s->bb_state = OT_I2C_BB_IDLE;
            s->bb_seen_valid_start = false;
            s->bb_drive_sda_low = false;
        }
    }

    /* 2. Check SCL Rising Edge (!prev_scl -> scl) */
    if (!s->bb_prev_scl && scl) {
        switch (s->bb_state) {
        case OT_I2C_BB_ADDR:
        case OT_I2C_BB_WRITE_DATA:
            if (s->bb_bit_count < 8u) {
                s->bb_shift_reg =
                    (uint8_t)((s->bb_shift_reg << 1u) | (sda_in ? 1u : 0u));
                s->bb_bit_count++;
            } else if (s->bb_bit_count == 8u) {
                s->bb_bit_count = 9u;
            }
            break;
        case OT_I2C_BB_READ_DATA:
            if (s->bb_bit_count < 8u) {
                s->bb_bit_count++;
            } else if (s->bb_bit_count == 8u) {
                if (sda_in) {
                    s->bb_state = OT_I2C_BB_IGNORE;
                }
                s->bb_bit_count = 9u;
            }
            break;
        default:
            break;
        }
    }

    /* 3. Check SCL Falling Edge (prev_scl -> !scl) */
    if (s->bb_prev_scl && !scl) {
        switch (s->bb_state) {
        case OT_I2C_BB_ADDR:
            if (s->bb_bit_count == 8u) {
                uint8_t addr = s->bb_shift_reg >> 1u;
                bool match = ot_i2c_check_address_match(s, addr);
                s->bb_drive_sda_low = match;
                s->bb_state = match ? OT_I2C_BB_ADDR : OT_I2C_BB_IGNORE;
                if (match) {
                    OtI2CSignal sig =
                        s->bb_seen_valid_start ? SIGNAL_RESTART : SIGNAL_START;
                    ot_i2c_target_set_acqdata(s, s->bb_shift_reg, sig);
                    s->bb_seen_valid_start = true;
                }
            } else if (s->bb_bit_count == 9u) {
                bool is_read = (s->bb_shift_reg & 1u) != 0;
                s->bb_bit_count = 0;
                s->bb_shift_reg = 0;
                if (!is_read) {
                    s->bb_state = OT_I2C_BB_WRITE_DATA;
                    s->bb_drive_sda_low = false;
                } else {
                    s->bb_state = OT_I2C_BB_READ_DATA;
                    s->bb_cur_tx_byte = fifo8_is_empty(&s->target_tx_fifo) ?
                                            0xffu :
                                            fifo8_pop(&s->target_tx_fifo);
                    ot_i2c_irq_set_state(s, TX_THRESHOLD,
                                         ot_i2c_tx_threshold_intr(s));
                    s->bb_drive_sda_low =
                        (((s->bb_cur_tx_byte >> 7u) & 1u) == 0);
                }
            }
            break;
        case OT_I2C_BB_WRITE_DATA:
            if (s->bb_bit_count == 8u) {
                s->bb_drive_sda_low = true;
                ot_i2c_target_set_acqdata(s, s->bb_shift_reg, SIGNAL_NONE);
            } else if (s->bb_bit_count == 9u) {
                s->bb_bit_count = 0;
                s->bb_shift_reg = 0;
                s->bb_drive_sda_low = false;
            }
            break;
        case OT_I2C_BB_READ_DATA:
            if (s->bb_bit_count < 8u) {
                s->bb_drive_sda_low =
                    (((s->bb_cur_tx_byte >> (7u - s->bb_bit_count)) & 1u) == 0);
            } else if (s->bb_bit_count == 8u) {
                s->bb_drive_sda_low = false;
            } else if (s->bb_bit_count == 9u) {
                s->bb_bit_count = 0;
                s->bb_cur_tx_byte = fifo8_is_empty(&s->target_tx_fifo) ?
                                        0xffu :
                                        fifo8_pop(&s->target_tx_fifo);
                ot_i2c_irq_set_state(s, TX_THRESHOLD,
                                     ot_i2c_tx_threshold_intr(s));
                s->bb_drive_sda_low = (((s->bb_cur_tx_byte >> 7u) & 1u) == 0);
            }
            break;
        default:
            s->bb_drive_sda_low = false;
            break;
        }
    }

    s->bb_prev_scl = scl;
    s->bb_prev_sda = sda_in;

    return sda_in && !s->bb_drive_sda_low;
}

bool ot_i2c_is_override_waveform_ready(OtI2CState *s)
{
    return s && !s->ovrd_consumed && s->ovrd_len >= 31u;
}

bool ot_i2c_is_override_enabled(OtI2CState *s)
{
    return s && ((s->regs[R_OVRD] & R_OVRD_TXOVRDEN_MASK) != 0) &&
           !s->ovrd_consumed;
}

void ot_i2c_set_waveform_start_ns(OtI2CState *s, int64_t val_ns, bool relative)
{
    if (!relative) {
        s->ovrd_start_ns = val_ns;
    } else if (s->ovrd_start_ns != INT64_MAX) {
        s->ovrd_start_ns += val_ns;
    }
}

bool ot_i2c_get_override_pin_level(OtI2CState *s, bool is_sda,
                                   int64_t sample_ns)
{
    if (!s || s->ovrd_len == 0 || sample_ns < s->ovrd_start_ns) {
        return true;
    }
    int64_t rel_ns = sample_ns - s->ovrd_start_ns;
    unsigned idx = 0;
    for (unsigned i = 0; i < s->ovrd_len; i++) {
        if (s->ovrd_steps[i].offset_ns <= rel_ns) {
            idx = i;
        } else {
            break;
        }
    }
    return is_sda ? s->ovrd_steps[idx].sda : s->ovrd_steps[idx].scl;
}

void ot_i2c_consume_override_waveform(OtI2CState *s)
{
    if (s) {
        s->ovrd_consumed = true;
    }
}

static void ot_i2c_realize(DeviceState *dev, Error **errp)
{
    OtI2CState *s = OT_I2C(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    if (!strcmp(s->ot_id, "i2c0")) {
        ot_i2c_instances[0] = s;
    } else if (!strcmp(s->ot_id, "i2c1")) {
        ot_i2c_instances[1] = s;
    } else if (!strcmp(s->ot_id, "i2c2")) {
        ot_i2c_instances[2] = s;
    }

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
    ot_fifo32_create(&s->target_rx_fifo, OT_I2C_ACQ_FIFO_SIZE);

    s->fmt_timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_i2c_fmt_timer_cb, s);
}

static void ot_i2c_class_init(ObjectClass *klass, const void *data)
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
