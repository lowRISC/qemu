/*
 * QEMU OpenTitan SPI Host controller
 *
 * Copyright (C) 2022 Western Digital
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Wilfred Mallawa <wilfred.mallawa@wdc.com>
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
 * Known limitations:
 *  - BigEndian devices are not supported
 *  - TX FIFO TXWD/TXEMPTY behavior documented in
 *    https://github.com/lowRISC/opentitan/issues/17644 is not emulated: there
 *    is no special case for first write: TXEMPTY is reset whenever one packet
 *    has been pushed and not yet sent over the SPI bus.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/ssi/ssi.h"
#include "trace.h"

/* ------------------------------------------------------------------------ */
/* Configuration */
/* ------------------------------------------------------------------------ */

/* undef to get all the repeated, identical status query traces */
#define DISCARD_REPEATED_STATUS_TRACES

/* fake delayed completion of HW commands */
#define FSM_TRIGGER_DELAY_NS 100U /* nanoseconds */

#define TXFIFO_LEN  288U /* bytes */
#define RXFIFO_LEN  256U /* bytes */
#define CMDFIFO_LEN 4U /* slots */

/* ------------------------------------------------------------------------ */
/* Register definitions */
/* ------------------------------------------------------------------------ */

/* clang-format off */
REG32(INTR_STATE, 0x00u)
    SHARED_FIELD(INTR_ERROR, 0u, 1u)
    SHARED_FIELD(INTR_SPI_EVENT, 1u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CONTROL, 0x10u)
    FIELD(CONTROL, RX_WATERMARK, 0u, 8u)
    FIELD(CONTROL, TX_WATERMARK, 8u, 8u)
    FIELD(CONTROL, OUTPUT_EN, 29u, 1u)
    FIELD(CONTROL, SW_RST, 30u, 1u)
    FIELD(CONTROL, SPIEN, 31u, 1u)
REG32(STATUS, 0x14u)
    FIELD(STATUS, TXQD, 0u, 8u)
    FIELD(STATUS, RXQD, 8u, 8u)
    FIELD(STATUS, CMDQD, 16u, 4u)
    FIELD(STATUS, RXWM, 20u, 1u)
    FIELD(STATUS, BYTEORDER, 22u, 1u)
    FIELD(STATUS, RXSTALL, 23u, 1u)
    FIELD(STATUS, RXEMPTY, 24u, 1u)
    FIELD(STATUS, RXFULL, 25u, 1u)
    FIELD(STATUS, TXWM, 26u, 1u)
    FIELD(STATUS, TXSTALL, 27u, 1u)
    FIELD(STATUS, TXEMPTY, 28u, 1u)
    FIELD(STATUS, TXFULL, 29u, 1u)
    FIELD(STATUS, ACTIVE, 30u, 1u)
    FIELD(STATUS, READY, 31u, 1u)
REG32(CONFIGOPTS, 0x18u)
    FIELD(CONFIGOPTS, CLKDIV_0, 0u, 16u)
    FIELD(CONFIGOPTS, CSNIDLE_0, 16u, 4u)
    FIELD(CONFIGOPTS, CSNTRAIL_0, 20u, 4u)
    FIELD(CONFIGOPTS, CSNLEAD_0, 24u, 4u)
    FIELD(CONFIGOPTS, FULLCYC_0, 29u, 1u)
    FIELD(CONFIGOPTS, CPHA_0, 30u, 1u)
    FIELD(CONFIGOPTS, CPOL_0, 31u, 1u)
REG32(CSID, 0x1cu)
    FIELD(CSID, CSID, 0u, 32u)
REG32(COMMAND, 0x20u)
    FIELD(COMMAND, LEN, 0u, 9u)
    FIELD(COMMAND, CSAAT, 9u, 1u)
    FIELD(COMMAND, SPEED, 10u, 2u)
    FIELD(COMMAND, DIRECTION, 12u, 2u)
REG32(RXDATA, 0x24u)
REG32(TXDATA, 0x28u)
REG32(ERROR_ENABLE, 0x2cu)
    FIELD(ERROR_ENABLE, CMDBUSY, 0u, 1u)
    FIELD(ERROR_ENABLE, OVERFLOW, 1u, 1u)
    FIELD(ERROR_ENABLE, UNDERFLOW, 2u, 1u)
    FIELD(ERROR_ENABLE, CMDINVAL, 3u, 1u)
    FIELD(ERROR_ENABLE, CSIDINVAL, 4u, 1u)
REG32(ERROR_STATUS, 0x30u)
    FIELD(ERROR_STATUS, CMDBUSY, 0u, 1u)
    FIELD(ERROR_STATUS, OVERFLOW, 1u, 1u)
    FIELD(ERROR_STATUS, UNDERFLOW, 2u, 1u)
    FIELD(ERROR_STATUS, CMDINVAL, 3u, 1u)
    FIELD(ERROR_STATUS, CSIDINVAL, 4u, 1u)
    FIELD(ERROR_STATUS, ACCESSINVAL, 5u, 1u)
REG32(EVENT_ENABLE, 0x34u)
    FIELD(EVENT_ENABLE, RXFULL, 0u, 1u)
    FIELD(EVENT_ENABLE, TXEMPTY, 1u, 1u)
    FIELD(EVENT_ENABLE, RXWM, 2u, 1u)
    FIELD(EVENT_ENABLE, TXWM, 3u, 1u)
    FIELD(EVENT_ENABLE, READY, 4u, 1u)
    FIELD(EVENT_ENABLE, IDLE, 5u, 1u)

#define INTR_MASK \
    (INTR_ERROR_MASK | INTR_SPI_EVENT_MASK)

#define R_CONTROL_MASK \
    (R_CONTROL_RX_WATERMARK_MASK | \
     R_CONTROL_TX_WATERMARK_MASK | \
     R_CONTROL_OUTPUT_EN_MASK    | \
     R_CONTROL_SW_RST_MASK       | \
     R_CONTROL_SPIEN_MASK)

#define R_COMMAND_MASK \
    (R_COMMAND_LEN_MASK       | \
     R_COMMAND_CSAAT_MASK     | \
     R_COMMAND_SPEED_MASK     | \
     R_COMMAND_DIRECTION_MASK)

#define R_ERROR_ENABLE_MASK \
    (R_ERROR_ENABLE_CMDBUSY_MASK   | \
     R_ERROR_ENABLE_OVERFLOW_MASK  | \
     R_ERROR_ENABLE_UNDERFLOW_MASK | \
     R_ERROR_ENABLE_CMDINVAL_MASK  | \
     R_ERROR_ENABLE_CSIDINVAL_MASK)

#define R_ERROR_STATUS_MASK \
    (R_ERROR_STATUS_CMDBUSY_MASK   | \
     R_ERROR_STATUS_OVERFLOW_MASK  | \
     R_ERROR_STATUS_UNDERFLOW_MASK | \
     R_ERROR_STATUS_CMDINVAL_MASK  | \
     R_ERROR_STATUS_CSIDINVAL_MASK | \
     R_ERROR_STATUS_ACCESSINVAL_MASK)

#define R_CONFIGOPTS_MASK \
    (R_CONFIGOPTS_CLKDIV_0_MASK    | \
     R_CONFIGOPTS_CSNIDLE_0_MASK   | \
     R_CONFIGOPTS_CSNTRAIL_0_MASK  | \
     R_CONFIGOPTS_CSNLEAD_0_MASK   | \
     R_CONFIGOPTS_FULLCYC_0_MASK   | \
     R_CONFIGOPTS_CPHA_0_MASK      | \
     R_CONFIGOPTS_CPOL_0_MASK)

#define R_EVENT_ENABLE_MASK \
    (R_EVENT_ENABLE_RXFULL_MASK  | \
     R_EVENT_ENABLE_TXEMPTY_MASK | \
     R_EVENT_ENABLE_RXWM_MASK    | \
     R_EVENT_ENABLE_TXWM_MASK    | \
     R_EVENT_ENABLE_READY_MASK   | \
     R_EVENT_ENABLE_IDLE_MASK)
/* clang-format on */

#define REG_UPDATE(_s_, _r_, _f_, _v_) \
    do { \
        (_s_)->regs[R_##_r_] = \
            FIELD_DP32((_s_)->regs[R_##_r_], _r_, _f_, _v_); \
    } while (0)

#define REG_GET(_s_, _r_, _f_) FIELD_EX32((_s_)->regs[R_##_r_], _r_, _f_)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

/* ------------------------------------------------------------------------ */
/* Debug */
/* ------------------------------------------------------------------------ */

#define R_LAST_REG (R_EVENT_ENABLE)
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
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(CONFIGOPTS),
    REG_NAME_ENTRY(CSID),
    REG_NAME_ENTRY(COMMAND),
    REG_NAME_ENTRY(RXDATA),
    REG_NAME_ENTRY(TXDATA),
    REG_NAME_ENTRY(ERROR_ENABLE),
    REG_NAME_ENTRY(ERROR_STATUS),
    REG_NAME_ENTRY(EVENT_ENABLE),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

static const char *F_COMMAND_DIRECTION[4u] = {
    "DUMMY",
    "RX",
    "TX",
    "TX|RX",
};

static const char *F_COMMAND_SPEED[4u] = {
    "STD",
    "DUAL",
    "QUAD",
    "ERROR",
};

static void ot_spi_host_trace_status(const char *msg, uint32_t status)
{
    unsigned cmd = FIELD_EX32(status, STATUS, CMDQD);
    unsigned rxd = FIELD_EX32(status, STATUS, RXQD);
    unsigned txd = FIELD_EX32(status, STATUS, TXQD);
    char str[64u];
    snprintf(str, sizeof(str), "%s%s%s%s%s%s%s%s%s%s",
             FIELD_EX32(status, STATUS, RXWM) ? "RXM|" : "",
             FIELD_EX32(status, STATUS, RXSTALL) ? "RXS|" : "",
             FIELD_EX32(status, STATUS, RXEMPTY) ? "RXE|" : "",
             FIELD_EX32(status, STATUS, RXFULL) ? "RXF|" : "",
             FIELD_EX32(status, STATUS, TXWM) ? "TXM|" : "",
             FIELD_EX32(status, STATUS, TXSTALL) ? "TXS|" : "",
             FIELD_EX32(status, STATUS, TXEMPTY) ? "TXE|" : "",
             FIELD_EX32(status, STATUS, TXFULL) ? "TXF|" : "",
             FIELD_EX32(status, STATUS, ACTIVE) ? "ACT|" : "",
             FIELD_EX32(status, STATUS, READY) ? "RDY|" : "");
    trace_ot_spi_host_status(msg, status, str, cmd, rxd, txd);
}

#ifdef DISCARD_REPEATED_STATUS_TRACES
typedef struct {
    uint64_t pc;
    uint32_t addr;
    uint32_t value;
    size_t count;
} TraceCache;
#endif /* DISCARD_REPEATED_STATUS_TRACES */

/* ------------------------------------------------------------------------ */
/* Types */
/* ------------------------------------------------------------------------ */

/** RX FIFO is byte-written and word-read */
typedef Fifo8 RxFifo;

/** TX FIFO contains TX data and tracks meaningful bytes in TX words */
struct TxFifo;
typedef struct TxFifo TxFifo;

/** Command FIFO contains commands and SPI device configuration */
struct CmdFifo;
typedef struct CmdFifo CmdFifo;

typedef struct {
    bool active; /**< a command is being handled */
    bool transaction; /**< SPI transation (CS is active) */
    bool rx_stall; /**< RX FIFO is full while processing a command */
    bool tx_stall; /**< TX FIFO is empty while processing a command */
    bool output_en; /**< SPI host output pins are enabled */
} OtSPIHostFsm;

/* this class is only required to manage on-hold reset */
struct OtSPIHostClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

struct OtSPIHostState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;

    qemu_irq *cs_lines; /**< CS output lines */
    SSIBus *ssi; /**< SPI bus */

    uint32_t *regs; /**< Registers (except. banked and fifos) */
    uint32_t *config_opts; /**< Banked configopts registers */

    RxFifo *rx_fifo;
    TxFifo *tx_fifo;
    CmdFifo *cmd_fifo;

    QEMUBH *fsm_bh; /**< Run queued commands */
    QEMUTimer *fsm_delay; /**< Simulate delayed SPI transfer completion */

    IbexIRQ irqs[2u]; /**< System bus IRQs */
    IbexIRQ alert; /**< OpenTitan alert */
    uint32_t events; /**< Active events */
    uint32_t last_events; /**< Last detected events */

    OtSPIHostFsm fsm;
    bool on_reset;

    /* properties */
    uint32_t bus_num; /**< SPI host port number */
    uint8_t num_cs; /**< Supported CS line count */
    bool initbug; /**< Whether to ignore first TX request */
};

/* ------------------------------------------------------------------------ */
/* Declarations */
/* ------------------------------------------------------------------------ */

static void ot_spi_host_post_fsm(void *opaque);

/* ------------------------------------------------------------------------ */
/* FIFOs */
/* ------------------------------------------------------------------------ */

/**
 * TX FIFO needs 36 bits of storage (32-bit word + 4-bit tracking)
 */
typedef struct {
    uint32_t bits; /* which bytes are meaningful in data (4 bits) */
    uint32_t data; /* 1..4 bytes */
} TxFifoSlot;

static_assert(sizeof(TxFifoSlot) == sizeof(uint64_t),
              "Invalid TxFifoSlot size");

struct TxFifo {
    TxFifoSlot *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
};

/**
 * Command FIFO stores commands alongs with SPI device configuration.
 * To fit into 64-bit word, limit supported CS lines down to 64K rather than 4G.
 */
typedef struct {
    uint32_t opts; /* configopts */
    uint16_t command; /* command[15:0] */
    uint8_t csid; /* csid[7:0] */
    bool ongoing; /* command is being processed */
} CmdFifoSlot;

static_assert(sizeof(TxFifoSlot) == sizeof(uint64_t),
              "Invalid CmdFifoSlot size");

struct CmdFifo {
    CmdFifoSlot *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
};

static void txfifo_create(TxFifo *fifo, uint32_t capacity)
{
    capacity /= sizeof(uint32_t); /* capacity is specified in bytes */
    fifo->data = g_new(TxFifoSlot, capacity);
    fifo->capacity = capacity;
    fifo->head = 0u;
    fifo->num = 0u;
}

static void txfifo_push(TxFifo *fifo, uint32_t data, uint32_t size)
{
    g_assert(fifo->num < fifo->capacity);

    switch (size) {
    case sizeof(uint32_t):
    case sizeof(uint16_t):
    case sizeof(uint8_t):
        break;
    default:
        g_assert_not_reached();
        break;
    }
    TxFifoSlot slot = {
        .bits = (1u << size) - 1u,
        .data = data,
    };
    fifo->data[(fifo->head + fifo->num) % fifo->capacity] = slot;
    fifo->num++;
}

static uint8_t txfifo_pop(TxFifo *fifo, bool last)
{
    g_assert(fifo->num > 0);
    TxFifoSlot slot = fifo->data[fifo->head];
    uint8_t ret = (uint8_t)slot.data;
    if (slot.bits > 1u && !last) {
        slot.data >>= 8u;
        slot.bits >>= 1u;
        fifo->data[fifo->head] = slot;
    } else {
        fifo->head++;
        fifo->head %= fifo->capacity;
        fifo->num--;
    }
    return ret;
}

static void txfifo_reset(TxFifo *fifo)
{
    fifo->num = 0u;
    fifo->head = 0u;
}

static bool txfifo_is_empty(TxFifo *fifo)
{
    return (fifo->num == 0u);
}

static bool txfifo_is_full(TxFifo *fifo)
{
    return (fifo->num == fifo->capacity);
}

static uint32_t txfifo_slot_used(TxFifo *fifo)
{
    return fifo->num;
}

static void cmdfifo_create(CmdFifo *fifo, uint32_t capacity)
{
    fifo->data = g_new(CmdFifoSlot, capacity);
    fifo->capacity = capacity;
    fifo->head = 0u;
    fifo->num = 0u;
}

static void cmdfifo_push(CmdFifo *fifo, CmdFifoSlot cmd)
{
    g_assert(fifo->num < fifo->capacity);
    fifo->data[(fifo->head + fifo->num) % fifo->capacity] = cmd;
    fifo->num++;
}

static CmdFifoSlot cmdfifo_pop(CmdFifo *fifo)
{
    g_assert(fifo->num > 0u);
    CmdFifoSlot ret = fifo->data[fifo->head++];
    fifo->head %= fifo->capacity;
    fifo->num--;
    return ret;
}

static CmdFifoSlot *cmdfifo_peek(CmdFifo *fifo)
{
    g_assert(fifo->num > 0u);
    return &fifo->data[fifo->head];
}

static void cmdfifo_reset(CmdFifo *fifo)
{
    fifo->num = 0u;
    fifo->head = 0u;
}

static bool cmdfifo_is_empty(CmdFifo *fifo)
{
    return (fifo->num == 0);
}

static bool cmdfifo_is_full(CmdFifo *fifo)
{
    return (fifo->num == fifo->capacity);
}

static uint32_t cmdfifo_num_used(CmdFifo *fifo)
{
    return fifo->num;
}

/* ------------------------------------------------------------------------ */
/* Helpers */
/* ------------------------------------------------------------------------ */

static bool ot_spi_host_is_rx(uint32_t command)
{
    return (bool)(FIELD_EX32(command, COMMAND, DIRECTION) & 0x1u);
}

static bool ot_spi_host_is_tx(uint32_t command)
{
    return (bool)(FIELD_EX32(command, COMMAND, DIRECTION) & 0x2u);
}

static bool ot_spi_host_is_ready(OtSPIHostState *s)
{
    return !cmdfifo_is_full(s->cmd_fifo);
}

static bool ot_spi_host_is_on_error(OtSPIHostState *s)
{
    return (bool)s->regs[R_ERROR_STATUS];
}

static void ot_spi_host_chip_select(OtSPIHostState *s, uint32_t csid,
                                    bool activate)
{
    trace_ot_spi_host_cs(csid, activate ? "" : "de");
    qemu_set_irq(s->cs_lines[csid], !activate);
}

static uint32_t ot_spi_host_get_status(OtSPIHostState *s)
{
    uint32_t status = R_STATUS_BYTEORDER_MASK; /* always little-endian */

    /* RX */

    /* round down, RXD should be seen as empty till it is padded */
    uint32_t rxqd = fifo8_num_used(s->rx_fifo) / sizeof(uint32_t);
    bool rxwm = rxqd >= REG_GET(s, CONTROL, RX_WATERMARK);
    status = FIELD_DP32(status, STATUS, RXQD, rxqd);
    status = FIELD_DP32(status, STATUS, RXWM, (uint32_t)rxwm);
    /*
     * the RX FIFO should be considered as empty as long as less than a full
     * slot (4 bytes) has been filled in. Otherwise the RXE bit may be set as
     * soon as a single byte is received from the slave, whereas the RX slot
     * padding has not yet been performed
     */
    bool rxe = fifo8_num_used(s->rx_fifo) < sizeof(uint32_t);
    status = FIELD_DP32(status, STATUS, RXEMPTY, (uint32_t)rxe);
    status =
        FIELD_DP32(status, STATUS, RXFULL, (uint32_t)fifo8_is_full(s->rx_fifo));
    status = FIELD_DP32(status, STATUS, RXSTALL, s->fsm.rx_stall);

    /* TX */
    uint32_t txqd = txfifo_slot_used(s->tx_fifo);
    bool txwm = txqd < REG_GET(s, CONTROL, TX_WATERMARK);
    status = FIELD_DP32(status, STATUS, TXQD, txqd);
    status = FIELD_DP32(status, STATUS, TXWM, (uint32_t)txwm);
    status = FIELD_DP32(status, STATUS, TXEMPTY,
                        (uint32_t)txfifo_is_empty(s->tx_fifo));
    status = FIELD_DP32(status, STATUS, TXFULL,
                        (uint32_t)txfifo_is_full(s->tx_fifo));
    status = FIELD_DP32(status, STATUS, TXSTALL, s->fsm.tx_stall);

    /* CMD */
    status = FIELD_DP32(status, STATUS, CMDQD, cmdfifo_num_used(s->cmd_fifo));

    /* State */
    status =
        FIELD_DP32(status, STATUS, READY, (uint32_t)ot_spi_host_is_ready(s));
    status = FIELD_DP32(status, STATUS, ACTIVE, s->fsm.active);

    return status;
}

static uint32_t ot_spi_host_build_event_bits(OtSPIHostState *s)
{
    /* round down, RXD should be seen as empty till it is padded */
    uint32_t rxqd = fifo8_num_used(s->rx_fifo) / sizeof(uint32_t);
    bool rxwm = rxqd >= REG_GET(s, CONTROL, RX_WATERMARK);
    uint32_t txqd = txfifo_slot_used(s->tx_fifo);
    bool txwm = txqd < REG_GET(s, CONTROL, TX_WATERMARK);

    uint32_t events;
    events = FIELD_DP32(0, EVENT_ENABLE, RXFULL,
                        (uint32_t)fifo8_is_full(s->rx_fifo));
    events = FIELD_DP32(events, EVENT_ENABLE, TXEMPTY,
                        (uint32_t)txfifo_is_empty(s->tx_fifo));
    events = FIELD_DP32(events, EVENT_ENABLE, RXWM, rxwm);
    events = FIELD_DP32(events, EVENT_ENABLE, TXWM, txwm);
    events = FIELD_DP32(events, EVENT_ENABLE, READY,
                        (uint32_t)ot_spi_host_is_ready(s));
    events = FIELD_DP32(events, EVENT_ENABLE, IDLE, (uint32_t)!s->fsm.active);
    return events;
}

/* ------------------------------------------------------------------------ */
/* IRQ and alert management */
/* ------------------------------------------------------------------------ */

/** IRQ lines */
enum OtSPIHostIrq {
    IRQ_ERROR,
    IRQ_SPI_EVENT,
    _IRQ_COUNT,
};

static bool ot_spi_host_update_event(OtSPIHostState *s)
{
    /* new events' state */
    uint32_t events = ot_spi_host_build_event_bits(s);

    /* events that have changed since last call (detect rising/falling edges) */
    uint32_t changes = s->last_events ^ events;
    /* RXWM/TXWM are not edge events, but level ones */
    changes |= R_EVENT_ENABLE_RXWM_MASK | R_EVENT_ENABLE_TXWM_MASK;
    s->last_events = events;

    /* pick up changes */
    events &= changes;

    /* accumulate events */
    s->events |= events;

    /* mask disabled events to get the spi event state */
    bool event = (bool)(s->events & s->regs[R_EVENT_ENABLE]);
    trace_ot_spi_host_debug1("event", event);

    /*
     * if the spi event test has been enabled, force event and clear its bit
     * right away
     */
    event |= (bool)(s->regs[R_INTR_TEST] & INTR_SPI_EVENT_MASK);
    s->regs[R_INTR_TEST] &= ~INTR_SPI_EVENT_MASK;
    if (event) {
        s->regs[R_INTR_STATE] |= INTR_SPI_EVENT_MASK;
    } else {
        s->regs[R_INTR_STATE] &= ~INTR_SPI_EVENT_MASK;
    }

    /* now update the IRQ signal (event could have been already signalled) */
    bool event_level = (bool)(s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE] &
                              INTR_SPI_EVENT_MASK);
    ibex_irq_set(&s->irqs[IRQ_SPI_EVENT], event_level);

    return event;
}

static bool ot_spi_host_update_error(OtSPIHostState *s)
{
    if (s->regs[R_ERROR_STATUS] & s->regs[R_ERROR_ENABLE]) {
        s->regs[R_INTR_STATE] |= INTR_ERROR_MASK;
    }

    if (s->regs[R_INTR_TEST] & INTR_ERROR_MASK) {
        s->regs[R_INTR_TEST] &= ~INTR_ERROR_MASK;
        s->regs[R_INTR_STATE] |= INTR_ERROR_MASK;
    }

    bool error = (bool)(s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE] &
                        INTR_ERROR_MASK);
    ibex_irq_set(&s->irqs[IRQ_ERROR], error);

    return error;
}

static void ot_spi_host_update_regs(OtSPIHostState *s)
{
    ot_spi_host_update_error(s);
    ot_spi_host_update_event(s);
}

static void ot_spi_host_update_alert(OtSPIHostState *s)
{
    /*
     * note: there is no other way to trigger a fatal error but the alert test
     * register in QEMU
     */
    bool alert = (bool)s->regs[R_ALERT_TEST];
    s->regs[R_ALERT_TEST] = 0u;
    ibex_irq_set(&s->alert, alert);
}

/* ------------------------------------------------------------------------ */
/* State machine and I/O */
/* ------------------------------------------------------------------------ */

static void ot_spi_host_reset(DeviceState *dev)
{
    OtSPIHostState *s = OT_SPI_HOST(dev);
    trace_ot_spi_host_reset("Resetting OpenTitan SPI");

    timer_del(s->fsm_delay);

    s->regs[R_INTR_STATE] = 0x00u;
    s->regs[R_INTR_ENABLE] = 0x00u;
    s->regs[R_INTR_TEST] = 0x00u;
    s->regs[R_ALERT_TEST] = 0x00u;
    s->regs[R_CONTROL] = 0x7fu;
    s->regs[R_CSID] = 0x00u;
    s->regs[R_ERROR_ENABLE] = 0x1fu;
    s->regs[R_ERROR_STATUS] = 0x00u;
    s->regs[R_EVENT_ENABLE] = 0x00u;

    /* configopts registers are banked */
    memset(s->config_opts, 0, s->num_cs * sizeof(uint32_t));

    /* rxdata, txdata, and command registers are managed w/ FIFOs */
    fifo8_reset(s->rx_fifo);
    txfifo_reset(s->tx_fifo);
    cmdfifo_reset(s->cmd_fifo);

    s->events = 0u;
    s->last_events = FIELD_DP32(0u, EVENT_ENABLE, TXEMPTY, 1u);

    memset(&s->fsm, 0, sizeof(s->fsm));

    for (unsigned csid = 0u; csid < s->num_cs; csid++) {
        ot_spi_host_chip_select(s, csid, false);
    }

    for (unsigned ix = 0u; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_irq_set(&s->irqs[ix], 0);
    }
    ibex_irq_set(&s->alert, 0);

    ot_spi_host_update_regs(s);
    ot_spi_host_update_alert(s);
}

static void ot_spi_host_reset_hold(Object *obj)
{
    OtSPIHostState *s = OT_SPI_HOST(obj);
    s->on_reset = true;
}

static void ot_spi_host_reset_exit(Object *obj)
{
    OtSPIHostState *s = OT_SPI_HOST(obj);
    s->on_reset = false;
}

/**
 * Called either from the I/O functions (command, rx_data, tx_data) or from
 * the bottom handler to start a new command.
 */
static void ot_spi_host_step_fsm(OtSPIHostState *s, const char *cause)
{
    trace_ot_spi_host_fsm(cause);

    CmdFifoSlot *headcmd = cmdfifo_peek(s->cmd_fifo);

    s->fsm.active = true;
    ot_spi_host_update_event(s);

    uint32_t command = (uint32_t)headcmd->command;
    bool read = ot_spi_host_is_rx(command);
    bool write = ot_spi_host_is_tx(command);
    bool multi = FIELD_EX32(command, COMMAND, SPEED) != 0;
    uint32_t length = FIELD_EX32(command, COMMAND, LEN) + 1u;
    if (!(read || write)) {
        /* dummy mode uses clock cycle count rather than byte count */
        if (length % 8u) {
            qemu_log_mask(LOG_UNIMP, "Unsupported clock cycle count: %u\n",
                          length);
        }
        length = DIV_ROUND_UP(length, 8u);
    }

    ot_spi_host_trace_status("S>", ot_spi_host_get_status(s));

    trace_ot_spi_host_command(
        F_COMMAND_DIRECTION[FIELD_EX32(command, COMMAND, DIRECTION)],
        F_COMMAND_SPEED[FIELD_EX32(command, COMMAND, SPEED)],
        (uint32_t)headcmd->csid, (bool)FIELD_EX32(command, COMMAND, CSAAT),
        length, s->fsm.transaction);

    while (length && !ot_spi_host_is_on_error(s)) {
        if (write && txfifo_is_empty(s->tx_fifo)) {
            break;
        }
        if (read && fifo8_is_full(s->rx_fifo)) {
            break;
        }

        if (!s->fsm.transaction) {
            s->fsm.transaction = true;
            ot_spi_host_chip_select(s, (uint32_t)headcmd->csid,
                                    s->fsm.transaction);
        }

        uint8_t tx =
            write ? (uint8_t)txfifo_pop(s->tx_fifo, length == 1u) : 0xffu;

        uint8_t rx = s->fsm.output_en ? ssi_transfer(s->ssi, tx) : 0xffu;

        if (multi && read && write) {
            /* invalid command, lets corrupt input data */
            trace_ot_spi_host_debug("conflicting command: input is overridden");
            rx ^= tx;
        }

        if (read) {
            fifo8_push(s->rx_fifo, rx);
        }

        trace_ot_spi_host_transfer(tx, rx);

        length--;
    }

    bool ongoing;
    if (length) {
        /* if the transfer early ended, a stall condition has been detected */
        if (write && txfifo_is_empty(s->tx_fifo)) {
            trace_ot_spi_host_debug("Tx stall");
            s->fsm.tx_stall = true;
        }
        if (read && fifo8_is_full(s->rx_fifo)) {
            trace_ot_spi_host_debug("Rx stall");
            s->fsm.rx_stall = true;
        }

        command = FIELD_DP32(command, COMMAND, LEN, length - 1);
        ongoing = true;
    } else {
        command = FIELD_DP32(command, COMMAND, LEN, 0);
        ongoing = false;
    }

    headcmd->command = (uint16_t)command;
    headcmd->ongoing = ongoing;

    timer_mod(s->fsm_delay,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FSM_TRIGGER_DELAY_NS);

    ot_spi_host_trace_status("S<", ot_spi_host_get_status(s));
}

/**
 * Called from the bottom handler to start the next queued command.
 */
static void ot_spi_host_schedule_fsm(void *opaque)
{
    OtSPIHostState *s = opaque;
    ot_spi_host_step_fsm(s, "bh");
}

/**
 * Called only from the timer once a command step is over (either completed or
 * stalled)
 */
static void ot_spi_host_post_fsm(void *opaque)
{
    trace_ot_spi_host_fsm("post");

    OtSPIHostState *s = opaque;

    CmdFifoSlot *headcmd = cmdfifo_peek(s->cmd_fifo);
    uint32_t command = (uint32_t)headcmd->command;
    bool ongoing = headcmd->ongoing;

    ot_spi_host_trace_status("P>", ot_spi_host_get_status(s));

    if (!ongoing) {
        if (ot_spi_host_is_rx(command)) {
            /*
             * transfer has been completed, RX FIFO may need padding up to a
             * word
             */
            while (!fifo8_is_full(s->rx_fifo) &&
                   fifo8_num_used(s->rx_fifo) & 0x3u) {
                fifo8_push(s->rx_fifo, 0u);
            }
        }

        /* release /CS if this is the last command of the current transaction */
        if (!FIELD_EX32(command, COMMAND, CSAAT)) {
            s->fsm.transaction = false;
            ot_spi_host_chip_select(s, (uint32_t)headcmd->csid,
                                    s->fsm.transaction);
        }

        /* retire command */
        cmdfifo_pop(s->cmd_fifo);

        /* "the command is complete when STATUS.ACTIVE goes low." */
        s->fsm.active = false;
    }

    ot_spi_host_update_regs(s);

    ot_spi_host_trace_status("P<", ot_spi_host_get_status(s));

    if (!ongoing) {
        /* last command has completed */
        if (!cmdfifo_is_empty(s->cmd_fifo)) {
            /* more commands have been scheduled */
            trace_ot_spi_host_debug("Next cmd");
            if (!ot_spi_host_is_on_error(s)) {
                qemu_bh_schedule(s->fsm_bh);
            } else {
                trace_ot_spi_host_debug("no resched: on err");
            }
        } else {
            trace_ot_spi_host_debug("no resched: no cmd");
        }
    } else {
        trace_ot_spi_host_debug("no resched: ongoing");
    }
}

static uint64_t ot_spi_host_read(void *opaque, hwaddr addr, unsigned int size)
{
    OtSPIHostState *s = opaque;
    uint32_t val32;

    if (s->on_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "device in reset");
        return 0u;
    }

    /* Match reg index */
    hwaddr reg = R32_OFF(addr);
    switch (reg) {
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_COMMAND:
    case R_TXDATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "W/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        val32 = 0u;
        break;
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CONTROL:
    case R_ERROR_ENABLE:
    case R_ERROR_STATUS:
    case R_EVENT_ENABLE:
    case R_CSID:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        val32 = ot_spi_host_get_status(s);
        break;
    case R_CONFIGOPTS:
        if (s->regs[R_CSID] < s->num_cs) {
            val32 = s->config_opts[s->regs[R_CSID]];
        } else {
            val32 = 0u;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: CSID invalid, Hardware settings discarded\n",
                          __func__);
        }
        break;
    case R_RXDATA: {
        /* here, size != 4 is illegal, what to do in this case? */
        if (fifo8_num_used(s->rx_fifo) < sizeof(uint32_t)) {
            REG_UPDATE(s, ERROR_STATUS, UNDERFLOW, 1);
            ot_spi_host_update_regs(s);
            val32 = 0u;
            break;
        }
        val32 = 0u;
        for (unsigned ix = 0u; ix < size; ix++) {
            val32 <<= 8u;
            val32 |= (uint32_t)fifo8_pop(s->rx_fifo);
        }
        val32 = bswap32(val32);
        bool resume = !cmdfifo_is_empty(s->cmd_fifo) && s->fsm.rx_stall &&
                      !s->fsm.tx_stall;
        s->fsm.rx_stall = false;
        if (resume) {
            ot_spi_host_step_fsm(s, "rx");
        } else {
            ot_spi_host_update_regs(s);
        }
        break;
    }
    default:
        val32 = 0u;
        qemu_log_mask(LOG_GUEST_ERROR, "Bad offset 0x%" HWADDR_PRIx "\n", addr);
    }

    uint64_t pc = ibex_get_current_pc();

#ifdef DISCARD_REPEATED_STATUS_TRACES
    static TraceCache trace_cache;

    if (trace_cache.pc != pc || trace_cache.addr != addr ||
        trace_cache.value != val32) {
        if (trace_cache.count > 1u) {
            trace_ot_spi_host_read_repeat(trace_cache.count);
        }
#endif /* DISCARD_REPEATED_STATUS_TRACES */
        trace_ot_spi_host_read(addr, REG_NAME(reg), val32, pc);
        if (reg == R_STATUS) {
            ot_spi_host_trace_status("", val32);
        }
#ifdef DISCARD_REPEATED_STATUS_TRACES
        trace_cache.count = 1u;
    } else {
        trace_cache.count += 1u;
    }
    trace_cache.pc = pc;
    trace_cache.addr = addr;
    trace_cache.value = val32;
#endif /* DISCARD_REPEATED_STATUS_TRACES */

    return val32;
}

static void ot_spi_host_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned int size)
{
    OtSPIHostState *s = opaque;
    uint32_t val32 = val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_spi_host_write(addr, REG_NAME(reg), val64, pc);

    if (s->on_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "device in reset");
        return;
    }

    switch (reg) {
    /* Skipping any R/O registers */
    case R_INTR_STATE:
        /* rw1c register */
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] &= ~val32;
        if (val32 & INTR_SPI_EVENT_MASK) {
            /* store current state */
            s->last_events = ot_spi_host_build_event_bits(s);
            /* clear up all signalled events */
            s->events = 0u;
        }
        ot_spi_host_update_regs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_spi_host_update_regs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_TEST] = val32;
        ot_spi_host_update_regs(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        s->regs[R_ALERT_TEST] = val32;
        ot_spi_host_update_alert(s);
        break;
    case R_CONTROL:
        val32 &= R_CONTROL_MASK;
        s->regs[R_CONTROL] = val32;
        if (FIELD_EX32(val32, CONTROL, SW_RST)) {
            ot_spi_host_reset((DeviceState *)s);
        }
        s->fsm.output_en = FIELD_EX32(val32, CONTROL, OUTPUT_EN);
        break;
    case R_CONFIGOPTS:
        /* Update the respective config-opts register based on CSIDth index */
        if (s->regs[R_CSID] < s->num_cs) {
            val32 &= R_CONFIGOPTS_MASK;
            s->config_opts[s->regs[R_CSID]] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: CSID invalid, Hardware settings discarded\n",
                          __func__);
        }
        break;
    case R_CSID:
        s->regs[R_CSID] = val32;
        break;
    case R_COMMAND: {
        if (cmdfifo_is_full(s->cmd_fifo)) {
            trace_ot_spi_host_reject("cmd fifo full");
            REG_UPDATE(s, ERROR_STATUS, CMDBUSY, (uint32_t) true);
            ot_spi_host_update_error(s);
            return;
        }

        val32 &= R_COMMAND_MASK;

        /* IP not enabled */
        if (!(REG_GET(s, CONTROL, SPIEN))) {
            trace_ot_spi_host_reject("no SPI/EN");
            return;
        }

        if (!ot_spi_host_is_ready(s)) {
            trace_ot_spi_host_reject("busy");
            REG_UPDATE(s, ERROR_STATUS, CMDBUSY, 1);
            ot_spi_host_update_regs(s);
            break;
        }

        bool error = false;
        if (((FIELD_EX32(val32, COMMAND, DIRECTION) == 0x3u) &&
             (FIELD_EX32(val32, COMMAND, SPEED) != 0u)) ||
            (FIELD_EX32(val32, COMMAND, SPEED) == 3u)) {
            /* dual/quad SPI cannot be used w/ full duplex mode */
            trace_ot_spi_host_reject("invalid command parameters");
            REG_UPDATE(s, ERROR_STATUS, CMDINVAL, 1u);
            error = true;
        }
        if (!(s->regs[R_CSID] < s->num_cs)) {
            /* CSID exceeds max num_cs */
            trace_ot_spi_host_reject("invalid csid");
            REG_UPDATE(s, ERROR_STATUS, CSIDINVAL, 1u);
            error = true;
        }

        uint16_t csid = (uint16_t)s->regs[R_CSID];
        CmdFifoSlot slot = {
            .opts = s->config_opts[csid],
            .command = (uint16_t)val32, /* only b15..b0 are meaningful */
            .csid = csid,
            .ongoing = false,
        };

        bool activate = cmdfifo_is_empty(s->cmd_fifo) && !s->fsm.rx_stall &&
                        !s->fsm.tx_stall && !error;
        cmdfifo_push(s->cmd_fifo, slot);
        ot_spi_host_update_event(s); /* track ready */
        if (activate) {
            ot_spi_host_step_fsm(s, "cmd");
            break;
        }
        ot_spi_host_update_regs(s);
        break;
    }
    case R_RXDATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x%02" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    case R_TXDATA: {
        /*
         * This is a hardware `feature` where the first word written to TXDATA
         * after init is omitted entirely
         */
        if (s->initbug) {
            s->initbug = false;
            return;
        }

        if (txfifo_is_full(s->tx_fifo)) {
            REG_UPDATE(s, ERROR_STATUS, OVERFLOW, 1u);
            ot_spi_host_update_regs(s);
            return;
        }

        txfifo_push(s->tx_fifo, val32, size);
        bool resume = !cmdfifo_is_empty(s->cmd_fifo) && s->fsm.tx_stall &&
                      !s->fsm.rx_stall;
        s->fsm.tx_stall = false;
        if (resume) {
            ot_spi_host_step_fsm(s, "tx");
        } else {
            ot_spi_host_update_regs(s);
        }
    } break;
    case R_ERROR_ENABLE:
        val32 &= R_ERROR_ENABLE_MASK;
        s->regs[R_ERROR_ENABLE] = val32;
        ot_spi_host_update_error(s);
        break;
    case R_ERROR_STATUS:
        /*
         * Indicates any errors that have occurred.  When an error occurs, the
         * corresponding bit must be cleared here before issuing any further
         * commands
         */
        val32 &= R_ERROR_STATUS_MASK;
        s->regs[R_ERROR_STATUS] &= ~val32;
        if (!cmdfifo_is_empty(s->cmd_fifo) && !s->regs[R_ERROR_STATUS] &&
            !s->fsm.tx_stall && !s->fsm.rx_stall) {
            ot_spi_host_step_fsm(s, "err");
        } else {
            ot_spi_host_update_error(s);
        }
        break;
    case R_EVENT_ENABLE:
        s->regs[R_EVENT_ENABLE] = val32 & R_EVENT_ENABLE_MASK;
        ot_spi_host_update_event(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Bad offset 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Device description/instanciation */
/* ------------------------------------------------------------------------ */

/* clang-format off */
static const MemoryRegionOps ot_spi_host_ops = {
    .read = ot_spi_host_read,
    .write = ot_spi_host_write,
    /* OpenTitan default LE */
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        /* although some registers only supports 2 or 4 byte write access */
        .min_access_size = 1u,
        .max_access_size = 4u,
    }
};
/* clang-format on */

static Property ot_spi_host_properties[] = {
    DEFINE_PROP_UINT8("num-cs", OtSPIHostState, num_cs, 1),
    DEFINE_PROP_UINT32("bus-num", OtSPIHostState, bus_num, 0),
    DEFINE_PROP_BOOL("initbug", OtSPIHostState, initbug, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_spi_host_realize(DeviceState *dev, Error **errp)
{
    OtSPIHostState *s = OT_SPI_HOST(dev);

    s->cs_lines = g_new0(qemu_irq, (size_t)s->num_cs);

    qdev_init_gpio_out_named(DEVICE(s), s->cs_lines, SSI_GPIO_CS,
                             (int)s->num_cs);

    char busname[16u];
    if (snprintf(busname, sizeof(busname), "spi%u", s->bus_num) >=
        sizeof(busname)) {
        error_setg(&error_fatal, "Invalid SSI bus num %u", s->bus_num);
        return;
    }
    s->ssi = ssi_create_bus(DEVICE(s), busname);
}

static void ot_spi_host_instance_init(Object *obj)
{
    OtSPIHostState *s = OT_SPI_HOST(obj);

    memory_region_init_io(&s->mmio, obj, &ot_spi_host_ops, s, TYPE_OT_SPI_HOST,
                          0x1000u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    _Static_assert(_IRQ_COUNT == ARRAY_SIZE(s->irqs), "Incoherent IRQ count");

    ibex_qdev_init_irqs(obj, &s->irqs[0u], SYSBUS_DEVICE_GPIO_IRQ,
                        ARRAY_SIZE(s->irqs));
    ibex_qdev_init_irq(obj, &s->alert, OPENTITAN_DEVICE_ALERT);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    s->config_opts = g_new0(uint32_t, (size_t)s->num_cs);

    s->rx_fifo = g_new0(RxFifo, 1u);
    s->tx_fifo = g_new0(TxFifo, 1u);
    s->cmd_fifo = g_new0(CmdFifo, 1u);

    fifo8_create(s->rx_fifo, RXFIFO_LEN);
    txfifo_create(s->tx_fifo, TXFIFO_LEN);
    cmdfifo_create(s->cmd_fifo, CMDFIFO_LEN);

    s->fsm_bh = qemu_bh_new(&ot_spi_host_schedule_fsm, s);
    s->fsm_delay = timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_spi_host_post_fsm, s);
}

static void ot_spi_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ot_spi_host_realize;
    dc->reset = ot_spi_host_reset;
    device_class_set_props(dc, ot_spi_host_properties);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = &ot_spi_host_reset_hold;
    rc->phases.exit = &ot_spi_host_reset_exit;
}

static const TypeInfo ot_spi_host_info = {
    .name = TYPE_OT_SPI_HOST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSPIHostState),
    .instance_init = ot_spi_host_instance_init,
    .class_init = ot_spi_host_class_init,
    .class_size = sizeof(OtSPIHostClass),
};

static void ot_spi_host_register_types(void)
{
    type_register_static(&ot_spi_host_info);
}

type_init(ot_spi_host_register_types)
