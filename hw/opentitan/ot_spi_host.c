/*
 * QEMU OpenTitan SPI Host controller
 *
 * Copyright (C) 2022 Western Digital
 * Copyright (c) 2022-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
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
#define FSM_COMPLETION_DELAY_NS 100U /* 100 ns (~ BH) */
/* initial FSM start up delay */
#define FSM_START_DELAY_NS 20000U /* 20 uS */

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
    FIELD(CONFIGOPTS, CLKDIV, 0u, 16u)
    FIELD(CONFIGOPTS, CSNIDLE, 16u, 4u)
    FIELD(CONFIGOPTS, CSNTRAIL, 20u, 4u)
    FIELD(CONFIGOPTS, CSNLEAD, 24u, 4u)
    FIELD(CONFIGOPTS, FULLCYC, 29u, 1u)
    FIELD(CONFIGOPTS, CPHA, 30u, 1u)
    FIELD(CONFIGOPTS, CPOL, 31u, 1u)
REG32(CSID, 0x1cu)
    FIELD(CSID, CSID, 0u, 32u)
REG32(COMMAND, 0x20u)
/* Darjeeling field definitions */
    FIELD(COMMAND, CSAAT, 0u, 1u)
    FIELD(COMMAND, SPEED, 1u, 2u)
    FIELD(COMMAND, DIRECTION, 3u, 2u)
    FIELD(COMMAND, LEN, 5u, 20u)
/* Earlgrey 1.0.0 field definitions */
    FIELD(COMMAND, EG_1_0_0_LEN, 0u, 9u)
    FIELD(COMMAND, EG_1_0_0_CSAAT, 9u, 1u)
    FIELD(COMMAND, EG_1_0_0_SPEED, 10u, 2u)
    FIELD(COMMAND, EG_1_0_0_DIRECTION, 12u, 2u)
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
    (R_CONFIGOPTS_CLKDIV_MASK   | \
     R_CONFIGOPTS_CSNIDLE_MASK  | \
     R_CONFIGOPTS_CSNTRAIL_MASK | \
     R_CONFIGOPTS_CSNLEAD_MASK  | \
     R_CONFIGOPTS_FULLCYC_MASK  | \
     R_CONFIGOPTS_CPHA_MASK     | \
     R_CONFIGOPTS_CPOL_MASK)

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
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

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

#ifdef DISCARD_REPEATED_STATUS_TRACES
typedef struct {
    uint64_t pc;
    uint32_t addr;
    uint32_t value;
    size_t count;
} TraceCache;
#endif /* DISCARD_REPEATED_STATUS_TRACES */

#define SPI_DEFAULT_TX_RX_VALUE ((uint8_t)0xffu)

/* ------------------------------------------------------------------------ */
/* Types */
/* ------------------------------------------------------------------------ */

/** RX FIFO is byte-written and word-read */
typedef Fifo8 RxFifo;

/**
 * TX FIFO needs 36 bits of storage (32-bit word + 4-bit tracking)
 */
typedef struct {
    uint32_t bits; /* which bytes are meaningful in data (4 bits) */
    uint32_t data; /* 1..4 bytes */
} TxFifoSlot;

static_assert(sizeof(TxFifoSlot) == sizeof(uint64_t),
              "Invalid TxFifoSlot size");

/**
 * Command FIFO stores commands along with SPI device configuration.
 */
typedef struct {
    uint32_t opts; /* configopts */
    uint32_t command; /* command */
    unsigned cs; /* csid */
    unsigned id; /* for debug/tracking */
} CmdFifoSlot;

/** TX FIFO contains TX data and tracks meaningful bytes in TX words */
typedef struct {
    TxFifoSlot *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
} TxFifo;

/** Command FIFO contains commands and SPI device configuration */
typedef struct {
    CmdFifoSlot *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t num;
} CmdFifo;

typedef struct {
    bool transaction; /* SPI transation (CS is active) */
    bool rx_stall; /* RX FIFO is full while processing a command */
    bool tx_stall; /* TX FIFO is empty while processing a command */
    bool output_en; /* SPI host output pins are enabled */
} OtSPIHostFsm;

typedef enum {
    CMD_NONE, /* no command (unused slot) */
    CMD_ONGOING, /* command is being executed, not yet completed */
    CMD_EXECUTED, /* commmand has been executed, need to be discarded */
} OtSPIHostCmdState;

/**
 * Current command, if any.
 */
typedef struct {
    CmdFifoSlot cmd;
    OtSPIHostCmdState state;
    int64_t ts;
    unsigned size;
} OtSPIHostCmd;

struct OtSPIHostState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    IbexIRQ *cs_lines; /* CS output lines */
    SSIBus *ssi; /* SPI bus */

    uint32_t *regs; /* Registers (except. fifos) */

    RxFifo *rx_fifo;
    TxFifo *tx_fifo;
    CmdFifo *cmd_fifo;

    QEMUTimer *fsm_delay; /* Simulate delayed SPI transfer completion */

    IbexIRQ irqs[2u]; /* System bus IRQs */
    IbexIRQ alert; /* OpenTitan alert */
    uint64_t total_transfer; /* Transfered bytes since reset */
    unsigned last_command_id; /* Command tracker (for debug purpose) */
    unsigned last_clkdiv; /* SPI clock tracker (for debug purpose) */
    OtSPIHostCmd active; /* Command being executed, if any */

    OtSPIHostFsm fsm;
    bool on_reset;

    /*
     * Upstream SPI Device Passthrough enable and chip select. If we support
     * passthrough mode (only one CS), The SPI Host CS is muxed with the
     * passthrough CS, controlled by the passthrough enable signal.
     */
    bool passthrough_en; /* Upstream SPI Device Passthrough enable line */
    bool passthrough_cs; /* Upstream SPI Device Chip Select */
    bool host_cs0; /* Our Chip Select 0 */

    /* Already emitted a guest error on first byte of invalid transfer */
    bool transfer_error_emitted;

    unsigned pclk; /* Current input clock */
    const char *clock_src_name; /* IRQ name once connected */

    /* properties */
    char *ot_id;
    char *clock_name; /* clock name */
    DeviceState *clock_src; /* clock source */
    uint32_t start_delay_ns; /* initial command kick off delay */
    uint32_t completion_delay_ns; /** completion delay/pacing */
    uint32_t bus_num; /* SPI host port number */
    uint32_t num_cs; /* Supported CS line count */
    uint8_t version;
};

/* ------------------------------------------------------------------------ */
/* FIFOs */
/* ------------------------------------------------------------------------ */

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

static bool txfifo_is_empty(const TxFifo *fifo)
{
    return (fifo->num == 0u);
}

static bool txfifo_is_full(const TxFifo *fifo)
{
    return (fifo->num == fifo->capacity);
}

static uint32_t txfifo_slot_used(const TxFifo *fifo)
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

static void cmdfifo_push(CmdFifo *fifo, const CmdFifoSlot *cmd)
{
    g_assert(fifo->num < fifo->capacity);
    memcpy(&fifo->data[(fifo->head + fifo->num) % fifo->capacity], cmd,
           sizeof(*cmd));
    fifo->num++;
}

static void cmdfifo_pop(CmdFifo *fifo, CmdFifoSlot *cmd)
{
    g_assert(fifo->num > 0u);
    if (cmd) {
        memcpy(cmd, &fifo->data[fifo->head], sizeof(*cmd));
    }
    fifo->head++;
    fifo->head %= fifo->capacity;
    fifo->num--;
}

static void cmdfifo_reset(CmdFifo *fifo)
{
    fifo->num = 0u;
    fifo->head = 0u;
}

static bool cmdfifo_is_empty(const CmdFifo *fifo)
{
    return (fifo->num == 0);
}

static bool cmdfifo_is_full(const CmdFifo *fifo)
{
    return (fifo->num == fifo->capacity);
}

static uint32_t cmdfifo_num_used(const CmdFifo *fifo)
{
    return fifo->num;
}

/* ------------------------------------------------------------------------ */
/* Helpers */
/* ------------------------------------------------------------------------ */

static uint32_t ot_spi_host_convert_from_eg_1_0_0_command(uint32_t eg_cmd)
{
    uint32_t csaat = FIELD_EX32(eg_cmd, COMMAND, EG_1_0_0_CSAAT);
    uint32_t speed = FIELD_EX32(eg_cmd, COMMAND, EG_1_0_0_SPEED);
    uint32_t direction = FIELD_EX32(eg_cmd, COMMAND, EG_1_0_0_DIRECTION);
    uint32_t len = FIELD_EX32(eg_cmd, COMMAND, EG_1_0_0_LEN);

    /* EG 1.0.0 has a narrower length field, so this conversion is safe. */
    uint32_t cmd = 0;
    cmd = FIELD_DP32(cmd, COMMAND, CSAAT, csaat);
    cmd = FIELD_DP32(cmd, COMMAND, SPEED, speed);
    cmd = FIELD_DP32(cmd, COMMAND, DIRECTION, direction);
    cmd = FIELD_DP32(cmd, COMMAND, LEN, len);
    return cmd;
}

static bool ot_spi_host_is_rx(uint32_t command)
{
    return (bool)(FIELD_EX32(command, COMMAND, DIRECTION) & 0x1u);
}

static bool ot_spi_host_is_tx(uint32_t command)
{
    return (bool)(FIELD_EX32(command, COMMAND, DIRECTION) & 0x2u);
}

static bool ot_spi_host_is_ready(const OtSPIHostState *s)
{
    return !cmdfifo_is_full(s->cmd_fifo);
}

/* Passthrough functionality is only implemented if there is one CS */
static inline bool ot_spi_host_supports_passthrough(const OtSPIHostState *s)
{
    return s->num_cs == 1u;
}

static void ot_spi_host_update_muxed_cs0(OtSPIHostState *s)
{
    if (ot_spi_host_supports_passthrough(s)) {
        ibex_irq_set(&s->cs_lines[0],
                     s->passthrough_en ? s->passthrough_cs : s->host_cs0);
    } else {
        ibex_irq_set(&s->cs_lines[0], s->host_cs0);
    }
}

static void ot_spi_host_chip_select(OtSPIHostState *s, unsigned csid,
                                    bool activate)
{
    if (csid < s->num_cs) {
        trace_ot_spi_host_cs(s->ot_id, csid, activate ? "" : "de");
        if (csid == 0u) {
            s->host_cs0 = !activate;
            ot_spi_host_update_muxed_cs0(s);
        } else {
            ibex_irq_set(&s->cs_lines[csid], !activate);
        }
    }
}

static inline bool ot_spi_host_passthrough_active(const OtSPIHostState *s)
{
    return ot_spi_host_supports_passthrough(s) && s->passthrough_en;
}

static bool ot_spi_host_update_stall(OtSPIHostState *s)
{
    g_assert(s->active.state != CMD_NONE);

    uint32_t command = s->active.cmd.command;
    unsigned length = FIELD_EX32(command, COMMAND, LEN) + 1u;
    bool read = ot_spi_host_is_rx(command);
    bool write = ot_spi_host_is_tx(command);

    bool resume = true;

    if (write && txfifo_is_empty(s->tx_fifo)) {
        trace_ot_spi_host_stall(s->ot_id, "TX", length);
        s->fsm.tx_stall = true;
        resume = false;
    }

    if (read && fifo8_is_full(s->rx_fifo)) {
        trace_ot_spi_host_stall(s->ot_id, "RX", length);
        s->fsm.rx_stall = true;

        resume = false;
    }

    return resume;
}

static void ot_spi_host_trace_status(const OtSPIHostState *s, const char *msg,
                                     uint32_t status)
{
    unsigned cmd = FIELD_EX32(status, STATUS, CMDQD);
    unsigned rxd = FIELD_EX32(status, STATUS, RXQD);
    unsigned txd = FIELD_EX32(status, STATUS, TXQD);
    char str[64u];
    int last =
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
    if (str[last - 1] == '|') {
        str[last - 1] = '\0';
    }
    char st = (char)(s->active.state == CMD_NONE ?
                         'N' :
                         (s->active.state == CMD_ONGOING ? 'O' : 'X'));
    trace_ot_spi_host_status(s->ot_id, msg, status, str, cmd, rxd, txd, st);
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
    status = FIELD_DP32(status, STATUS, ACTIVE,
                        (uint32_t)(s->active.state != CMD_NONE));

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
    events = FIELD_DP32(events, EVENT_ENABLE, IDLE,
                        (uint32_t)(s->active.state == CMD_NONE));
    return events;
}

/* ------------------------------------------------------------------------ */
/* IRQ and alert management */
/* ------------------------------------------------------------------------ */

/** IRQ lines */
enum OtSPIHostIrq {
    IRQ_ERROR,
    IRQ_SPI_EVENT,
    IRQ_COUNT,
};

static bool ot_spi_host_update_event(OtSPIHostState *s)
{
    uint32_t events = ot_spi_host_build_event_bits(s);
    uint32_t eff_events = events & s->regs[R_EVENT_ENABLE];
    trace_ot_spi_host_events(s->ot_id, events, events, events,
                             s->regs[R_EVENT_ENABLE], eff_events);

    bool event = (bool)eff_events;
    event |= (bool)(s->regs[R_INTR_TEST] & INTR_SPI_EVENT_MASK);
    s->regs[R_INTR_TEST] &= ~INTR_SPI_EVENT_MASK;
    if (event) {
        s->regs[R_INTR_STATE] |= INTR_SPI_EVENT_MASK;
    } else {
        s->regs[R_INTR_STATE] &= ~INTR_SPI_EVENT_MASK;
    }

    bool event_level = (bool)(s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE] &
                              INTR_SPI_EVENT_MASK);
    if (event_level != (bool)ibex_irq_get_level(&s->irqs[IRQ_SPI_EVENT])) {
        trace_ot_spi_host_update_irq(s->ot_id, "event", event_level);
    }
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
    if (error != (bool)ibex_irq_get_level(&s->irqs[IRQ_ERROR])) {
        trace_ot_spi_host_update_irq(s->ot_id, "error", error);
    }
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
    ibex_irq_set(&s->alert, alert);
}

/* ------------------------------------------------------------------------ */
/* State machine and I/O */
/* ------------------------------------------------------------------------ */

static void ot_spi_host_internal_reset(OtSPIHostState *s)
{
    trace_ot_spi_host_internal_reset(s->ot_id, s->start_delay_ns,
                                     s->completion_delay_ns);

    timer_del(s->fsm_delay);

    /* rxdata, txdata, and command registers are managed w/ FIFOs */
    fifo8_reset(s->rx_fifo);
    txfifo_reset(s->tx_fifo);
    cmdfifo_reset(s->cmd_fifo);

    memset(&s->fsm, 0, sizeof(s->fsm));
    memset(&s->active, 0, sizeof(s->active));

    for (unsigned csid = 0u; csid < s->num_cs; csid++) {
        ot_spi_host_chip_select(s, csid, false);
    }

    s->total_transfer = 0;
    s->last_command_id = 0;
    s->last_clkdiv = UINT32_MAX;

    ot_spi_host_update_regs(s);
    ot_spi_host_update_alert(s);

    ot_spi_host_trace_status(s, "intrst", ot_spi_host_get_status(s));
}

static void ot_spi_host_step_fsm(OtSPIHostState *s, const char *cause)
{
    trace_ot_spi_host_fsm(s->ot_id, s->active.cmd.id, cause);

    ot_spi_host_update_event(s);

    unsigned byte_count = 0;
    bool resched = true;

    if (s->active.state == CMD_EXECUTED) {
        goto post;
    }

    uint32_t command = s->active.cmd.command;
    bool read = ot_spi_host_is_rx(command);
    bool write = ot_spi_host_is_tx(command);
    unsigned speed = FIELD_EX32(command, COMMAND, SPEED);
    unsigned clkdiv = FIELD_EX32(s->active.cmd.opts, CONFIGOPTS, CLKDIV);
    if (trace_event_get_state(TRACE_OT_SPI_HOST_CMD_CLOCK)) {
        if (clkdiv != s->last_clkdiv) {
            unsigned spiclock = s->pclk / ((clkdiv + 1u) * 2u);
            trace_ot_spi_host_cmd_clock(s->ot_id, s->active.cmd.id,
                                        s->completion_delay_ns == 0, spiclock);
        }
    }
    s->last_clkdiv = clkdiv;
    unsigned length = FIELD_EX32(command, COMMAND, LEN) + 1u;
    if (!(read || write)) {
        /* dummy mode uses clock cycle count rather than byte count */
        if (length % (1u << (3u - speed))) {
            qemu_log_mask(
                LOG_UNIMP,
                "%s: %s: unsupported clk cycle count: %u for speed %u\n",
                __func__, s->ot_id, length, speed);
        }
        length = DIV_ROUND_UP(length, 8u);
    }

    ot_spi_host_trace_status(s, "S>", ot_spi_host_get_status(s));

    trace_ot_spi_host_exec_command(
        s->ot_id, s->active.cmd.id,
        F_COMMAND_DIRECTION[FIELD_EX32(command, COMMAND, DIRECTION)],
        F_COMMAND_SPEED[FIELD_EX32(command, COMMAND, SPEED)],
        (unsigned)s->active.cmd.cs, (bool)FIELD_EX32(command, COMMAND, CSAAT),
        length, s->fsm.transaction);

    if (s->active.size == 0) {
        s->active.ts = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    }

    bool multi = speed != 0;

    while (length) {
        if (write && txfifo_is_empty(s->tx_fifo)) {
            break;
        }
        if (read && fifo8_is_full(s->rx_fifo)) {
            break;
        }

        if (!s->fsm.transaction) {
            s->fsm.transaction = true;
            if (ot_spi_host_passthrough_active(s)) {
                qemu_log_mask(
                    LOG_GUEST_ERROR,
                    "%s: %s: SPI Host active CS while Passthrough is active\n",
                    __func__, s->ot_id);
            }
            ot_spi_host_chip_select(s, s->active.cmd.cs, s->fsm.transaction);
        }

        uint8_t tx = write ? (uint8_t)txfifo_pop(s->tx_fifo, length == 1u) :
                             SPI_DEFAULT_TX_RX_VALUE;

        uint8_t rx = SPI_DEFAULT_TX_RX_VALUE;

        if (s->fsm.output_en) {
            if (ot_spi_host_passthrough_active(s)) {
                if (!s->transfer_error_emitted) {
                    s->transfer_error_emitted = true;
                    qemu_log_mask(
                        LOG_GUEST_ERROR,
                        "%s: %s: Host transfer while Passthrough is active\n",
                        __func__, s->ot_id);
                }
            } else {
                rx = (uint8_t)ssi_transfer(s->ssi, tx);
            }
        }

        if (multi && read && write) {
            /* invalid command, lets corrupt input data */
            trace_ot_spi_host_debug(s->ot_id,
                                    "conflicting command: input is overridden");
            rx ^= tx;
        }

        if (read) {
            fifo8_push(s->rx_fifo, rx);
        }

        trace_ot_spi_host_transfer(s->ot_id, s->total_transfer, tx, rx);
        s->total_transfer += 1u;

        length--;
        byte_count++;
    }

    if (length) {
        /* if the transfer early ended, a stall condition has been detected */
        s->active.cmd.command = FIELD_DP32(command, COMMAND, LEN, length - 1);
        resched = ot_spi_host_update_stall(s);
    } else {
        s->active.cmd.command = FIELD_DP32(command, COMMAND, LEN, 0);
        s->active.state = CMD_EXECUTED;
    }

post:
    ot_spi_host_update_regs(s);

    s->active.size += byte_count;

    int64_t delay;
    if (s->active.state == CMD_EXECUTED && byte_count) {
        if (!s->completion_delay_ns) {
            unsigned spiclock = s->pclk / ((clkdiv + 1u) * 2u);
            delay = NANOSECONDS_PER_SECOND / (int64_t)spiclock;
            delay *= s->active.size;
            delay >>= speed;
        } else {
            delay = (int64_t)s->completion_delay_ns;
        }
        timer_mod(s->fsm_delay, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + delay);
    } else if (!timer_pending(s->fsm_delay)) {
        delay = (int64_t)s->completion_delay_ns;
        if (resched) {
            /*
             * Do not reschedule the FSM when a stall prevents any further
             * action. Once the SPI Host is read/written and data FIFO(s) get
             * some free slots, the FSM is automatically rescheduled
             */
            timer_mod(s->fsm_delay,
                      qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + delay);
        }
    }

    ot_spi_host_trace_status(s, "S<", ot_spi_host_get_status(s));
}

static void ot_spi_host_retire(OtSPIHostState *s)
{
    uint32_t command = s->active.cmd.command;

    if (trace_event_get_state(TRACE_OT_SPI_HOST_CMD_STAT)) {
        int64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
        int64_t elapsed = now - s->active.ts;
        if (s->active.size) {
            unsigned speed =
                (unsigned)((s->active.size * NANOSECONDS_PER_SECOND) / elapsed);
            trace_ot_spi_host_cmd_stat(s->ot_id, s->active.cmd.id,
                                       s->active.size, speed);
        }
    }

    trace_ot_spi_host_retire_command(s->ot_id, s->active.cmd.id);

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
        ot_spi_host_chip_select(s, s->active.cmd.cs, s->fsm.transaction);
    }

    /* retire command */
    s->active.state = CMD_NONE;

    /* last command has completed */
    if (!cmdfifo_is_empty(s->cmd_fifo)) {
        /* more commands have been scheduled */
        trace_ot_spi_host_debug(s->ot_id, "next cmd");
        if (s->active.state == CMD_NONE) {
            cmdfifo_pop(s->cmd_fifo, &s->active.cmd);
            s->active.state = CMD_ONGOING;
            s->active.size = 0u;
        }
        ot_spi_host_step_fsm(s, "post");
    } else {
        trace_ot_spi_host_debug(s->ot_id, "no resched: no cmd");
    }

    ot_spi_host_update_regs(s);
}

static void ot_spi_host_schedule_fsm(void *opaque)
{
    OtSPIHostState *s = opaque;

    trace_ot_spi_host_fsm(s->ot_id, s->active.cmd.id, "sched");

    bool retire = s->active.state == CMD_EXECUTED;

    ot_spi_host_trace_status(s, "P>", ot_spi_host_get_status(s));

    if (retire) {
        ot_spi_host_retire(s);
    } else {
        ot_spi_host_update_regs(s);

        uint32_t command = s->active.cmd.command;
        unsigned length = FIELD_EX32(command, COMMAND, LEN) + 1u;
        bool pending = timer_pending(s->fsm_delay);
        trace_ot_spi_host_cmd_ongoing(s->ot_id, s->active.cmd.id, length,
                                      pending);
        if (!pending) {
            ot_spi_host_step_fsm(s, "pending");
        }
    }

    ot_spi_host_trace_status(s, "P<", ot_spi_host_get_status(s));
}

static void ot_spi_host_clock_input(void *opaque, int irq, int level)
{
    OtSPIHostState *s = opaque;

    g_assert(irq == 0);

    s->pclk = (unsigned)level;

    /* TODO: disable SPI transfer when PCLK is 0 */
    trace_ot_spi_host_clock_update(s->ot_id, s->pclk);
}

static uint64_t ot_spi_host_io_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    OtSPIHostState *s = opaque;
    uint32_t val32;

    if (s->on_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: device in reset\n", __func__,
                      s->ot_id);
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
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0u;
        break;
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CONTROL:
    case R_ERROR_ENABLE:
    case R_ERROR_STATUS:
    case R_EVENT_ENABLE:
    case R_CSID:
    case R_CONFIGOPTS:
        val32 = s->regs[reg];
        break;
    case R_STATUS:
        val32 = ot_spi_host_get_status(s);
        break;
    case R_RXDATA: {
        /* here, size != 4 is illegal, what to do in this case? */
        if (fifo8_num_used(s->rx_fifo) < sizeof(uint32_t)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Read underflow: %u\n",
                          __func__, s->ot_id, fifo8_num_used(s->rx_fifo));
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
        ot_spi_host_trace_status(s, "rxd", ot_spi_host_get_status(s));
        val32 = bswap32(val32);
        bool resume = false;
        if (s->active.state != CMD_NONE) {
            if (!s->fsm.tx_stall) {
                uint32_t rxwm = REG_GET(s, CONTROL, RX_WATERMARK);
                if (rxwm < sizeof(uint32_t)) {
                    rxwm = sizeof(uint32_t);
                }
                uint32_t inlen = fifo8_num_used(s->rx_fifo) / sizeof(uint32_t);
                if (inlen <= rxwm) {
                    resume = true;
                }
            }
        }
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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
    }

    uint32_t pc = ibex_get_current_pc();

#ifdef DISCARD_REPEATED_STATUS_TRACES
    static TraceCache trace_cache;

    if (trace_cache.pc != pc || trace_cache.addr != addr ||
        trace_cache.value != val32) {
        if (trace_cache.count > 1u) {
            hwaddr rreg = R32_OFF(trace_cache.addr);
            trace_ot_spi_host_io_read_repeat(s->ot_id, REG_NAME(rreg),
                                             trace_cache.count);
        }
#endif /* DISCARD_REPEATED_STATUS_TRACES */
        trace_ot_spi_host_io_read(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                  val32, pc);
        if (reg == R_STATUS) {
            ot_spi_host_trace_status(s, "", val32);
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

static void ot_spi_host_io_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned int size)
{
    OtSPIHostState *s = opaque;
    uint32_t val32 = val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_spi_host_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                               pc);

    if (s->on_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: device in reset\n", __func__,
                      s->ot_id);
        return;
    }

    switch (reg) {
    /* Skipping any R/O registers */
    case R_INTR_STATE:
        val32 &= INTR_ERROR_MASK; /* rw1c bit */
        s->regs[R_INTR_STATE] &= ~val32;
        /* this call also regenerates all raised events */
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
            ot_spi_host_internal_reset(s);
        }
        s->fsm.output_en = FIELD_EX32(val32, CONTROL, OUTPUT_EN);
        if (!cmdfifo_is_empty(s->cmd_fifo)) {
            ot_spi_host_step_fsm(s, "ctrl");
        }
        break;
    case R_CONFIGOPTS:
    case R_CSID:
        s->regs[reg] = val32;
        break;
    case R_COMMAND: {
        /* Convert command for back-compat with Earlgrey 1.0.0 fields */
        if (s->version == OT_SPI_HOST_VERSION_EG_1_0_0) {
            val32 = ot_spi_host_convert_from_eg_1_0_0_command(val32);
        }
        val32 &= R_COMMAND_MASK;

        /* IP not enabled */
        if (!(REG_GET(s, CONTROL, SPIEN))) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: no SPI/EN\n", __func__,
                          s->ot_id);
            return;
        }

        if (!ot_spi_host_is_ready(s)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: busy (cmd_fifo full)\n",
                          __func__, s->ot_id);
            REG_UPDATE(s, ERROR_STATUS, CMDBUSY, 1u);
            ot_spi_host_update_regs(s);
            break;
        }

        if (((FIELD_EX32(val32, COMMAND, DIRECTION) == 0x3u) &&
             (FIELD_EX32(val32, COMMAND, SPEED) != 0u)) ||
            (FIELD_EX32(val32, COMMAND, SPEED) == 3u)) {
            /* dual/quad SPI cannot be used w/ full duplex mode */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: invalid command parameters\n", __func__,
                          s->ot_id);
            REG_UPDATE(s, ERROR_STATUS, CMDINVAL, 1u);
        }

        unsigned csid = s->regs[R_CSID];

        if (!(csid < s->num_cs)) {
            /* CSID exceeds max num_cs */
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: invalid csid: %u\n",
                          __func__, s->ot_id, csid);
            REG_UPDATE(s, ERROR_STATUS, CSIDINVAL, 1u);
            csid = 0;
        }

        CmdFifoSlot slot = {
            .opts = s->regs[R_CONFIGOPTS],
            .command = val32,
            .cs = csid,
            .id = s->last_command_id++,
        };

        trace_ot_spi_host_new_command(
            s->ot_id, slot.id,
            F_COMMAND_DIRECTION[FIELD_EX32(slot.command, COMMAND, DIRECTION)],
            F_COMMAND_SPEED[FIELD_EX32(slot.command, COMMAND, SPEED)], csid,
            (bool)FIELD_EX32(slot.command, COMMAND, CSAAT),
            FIELD_EX32(slot.command, COMMAND, LEN) + 1u);

        cmdfifo_push(s->cmd_fifo, &slot);

        if (s->active.state == CMD_NONE) {
            cmdfifo_pop(s->cmd_fifo, &s->active.cmd);
            s->active.state = CMD_ONGOING;
            s->active.size = 0u;
            bool activate = ot_spi_host_update_stall(s);
            ot_spi_host_update_regs(s);

            trace_ot_spi_host_kick_command(s->ot_id, s->active.cmd.id,
                                           activate);

            if (activate) {
                /*
                 * add a small delay before kicking of the command FSM. This
                 * yield execution back to the vCPU (releasing the IO thread),
                 * so the guest can check the SPI Host status, etc. This very
                 * short delay does not slow down execution.
                 */
                timer_mod(s->fsm_delay, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) +
                                            s->start_delay_ns);
            }
            break;
        }
        ot_spi_host_update_regs(s);
        break;
    }
    case R_RXDATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    case R_TXDATA: {
        if (txfifo_is_full(s->tx_fifo)) {
            REG_UPDATE(s, ERROR_STATUS, OVERFLOW, 1u);
            ot_spi_host_update_regs(s);
            return;
        }

        txfifo_push(s->tx_fifo, val32, size);
        bool resume = (s->active.state != CMD_NONE) && s->fsm.tx_stall &&
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
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->ot_id, addr);
        break;
    }
}

static uint8_t ot_spi_host_downstream_transfer(OtSPIHostState *s, uint8_t tx)
{
    if (!ot_spi_host_supports_passthrough(s)) {
        if (!s->transfer_error_emitted) {
            s->transfer_error_emitted = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: SPI Host does not support passthrough: more "
                          "than one CS\n",
                          __func__, s->ot_id);
        }
        return SPI_DEFAULT_TX_RX_VALUE;
    }

    if (!s->passthrough_en) {
        trace_ot_spi_host_passthrough_disabled(s->ot_id);
        return SPI_DEFAULT_TX_RX_VALUE;
    }

    /* Forward to downstream flash */
    return (uint8_t)ssi_transfer(s->ssi, (uint32_t)tx);
}

static void
ot_spi_host_device_passthrough_en_input(void *opaque, int irq, int level)
{
    OtSPIHostState *s = opaque;
    g_assert(irq == 0u);

    s->transfer_error_emitted = false;

    if (!ot_spi_host_supports_passthrough(s)) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: %s: SPI Host does not support passthrough: more than one CS\n",
            __func__, s->ot_id);
        return;
    }

    if ((bool)level && !s->host_cs0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Passthrough enabled while Host CS is active\n",
                      __func__, s->ot_id);
    }

    s->passthrough_en = (bool)level;

    ot_spi_host_update_muxed_cs0(s);
}

static void
ot_spi_host_device_passthrough_cs_input(void *opaque, int irq, int level)
{
    OtSPIHostState *s = opaque;
    g_assert(irq == 0u);

    if (!ot_spi_host_supports_passthrough(s)) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: %s: SPI Host does not support passthrough: more than one CS\n",
            __func__, s->ot_id);
        return;
    }

    if (!s->passthrough_en) {
        trace_ot_spi_host_passthrough_disabled(s->ot_id);
    }

    s->passthrough_cs = (bool)level;

    ot_spi_host_update_muxed_cs0(s);
}

/* ------------------------------------------------------------------------ */
/* Device description/instanciation */
/* ------------------------------------------------------------------------ */

/* clang-format off */
static const MemoryRegionOps ot_spi_host_ops = {
    .read = ot_spi_host_io_read,
    .write = ot_spi_host_io_write,
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
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtSPIHostState, ot_id),
    DEFINE_PROP_UINT32("num-cs", OtSPIHostState, num_cs, 1u),
    DEFINE_PROP_UINT32("bus-num", OtSPIHostState, bus_num, 0u),
    DEFINE_PROP_STRING("clock-name", OtSPIHostState, clock_name),
    DEFINE_PROP_LINK("clock-src", OtSPIHostState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_UINT32("start-delay", OtSPIHostState, start_delay_ns,
                       FSM_START_DELAY_NS),
    DEFINE_PROP_UINT32("completion-delay", OtSPIHostState, completion_delay_ns,
                       0),
    DEFINE_PROP_UINT8("version", OtSPIHostState, version, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_spi_host_reset_enter(Object *obj, ResetType type)
{
    OtSPIHostClass *c = OT_SPI_HOST_GET_CLASS(obj);
    OtSPIHostState *s = OT_SPI_HOST(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    s->regs[R_INTR_STATE] = 0x00u;
    s->regs[R_INTR_ENABLE] = 0x00u;
    s->regs[R_INTR_TEST] = 0x00u;
    s->regs[R_ALERT_TEST] = 0x00u;
    s->regs[R_CONTROL] = 0x7fu;
    s->regs[R_CSID] = 0x00u;
    s->regs[R_ERROR_ENABLE] = 0x1fu;
    s->regs[R_ERROR_STATUS] = 0x00u;
    s->regs[R_EVENT_ENABLE] = 0x00u;

    s->on_reset = true;

    s->passthrough_en = false;
    s->passthrough_cs = true;
    s->host_cs0 = true;

    s->transfer_error_emitted = false;

    if (!s->clock_src_name) {
        IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
        IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

        s->clock_src_name =
            ic->get_clock_source(ii, s->clock_name, DEVICE(s), &error_fatal);
        qemu_irq in_irq = qdev_get_gpio_in_named(DEVICE(s), "clock-in", 0);
        qdev_connect_gpio_out_named(s->clock_src, s->clock_src_name, 0, in_irq);
    }

    ot_spi_host_internal_reset(s);
}

static void ot_spi_host_reset_exit(Object *obj, ResetType type)
{
    OtSPIHostClass *c = OT_SPI_HOST_GET_CLASS(obj);
    OtSPIHostState *s = OT_SPI_HOST(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    s->on_reset = false;
}

static void ot_spi_host_realize(DeviceState *dev, Error **errp)
{
    OtSPIHostState *s = OT_SPI_HOST(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->clock_name);
    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    g_assert(s->version < OT_SPI_HOST_VERSION_COUNT);

    s->cs_lines = g_new0(IbexIRQ, (size_t)s->num_cs);

    ibex_qdev_init_irqs(OBJECT(dev), &s->cs_lines[0], SSI_GPIO_CS, s->num_cs);

    qdev_init_gpio_in_named(DEVICE(s), &ot_spi_host_clock_input, "clock-in", 1);

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
                          0x40u);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    _Static_assert(IRQ_COUNT == ARRAY_SIZE(s->irqs), "Incoherent IRQ count");

    ibex_qdev_init_irqs(obj, &s->irqs[0u], SYSBUS_DEVICE_GPIO_IRQ,
                        ARRAY_SIZE(s->irqs));
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    qdev_init_gpio_in_named(DEVICE(s), &ot_spi_host_device_passthrough_en_input,
                            OT_SPI_HOST_PASSTHROUGH_EN, 1);
    qdev_init_gpio_in_named(DEVICE(s), &ot_spi_host_device_passthrough_cs_input,
                            OT_SPI_HOST_PASSTHROUGH_CS, 1);

    s->regs = g_new0(uint32_t, REGS_COUNT);

    s->rx_fifo = g_new0(RxFifo, 1u);
    s->tx_fifo = g_new0(TxFifo, 1u);
    s->cmd_fifo = g_new0(CmdFifo, 1u);

    fifo8_create(s->rx_fifo, RXFIFO_LEN);
    txfifo_create(s->tx_fifo, TXFIFO_LEN);
    cmdfifo_create(s->cmd_fifo, CMDFIFO_LEN);

    s->fsm_delay = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_spi_host_schedule_fsm, s);
}

static void ot_spi_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = ot_spi_host_realize;
    device_class_set_props(dc, ot_spi_host_properties);

    OtSPIHostClass *sc = OT_SPI_HOST_CLASS(klass);
    sc->ssi_downstream_transfer = &ot_spi_host_downstream_transfer;

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_spi_host_reset_enter, NULL,
                                       &ot_spi_host_reset_exit,
                                       &sc->parent_phases);
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

type_init(ot_spi_host_register_types);
