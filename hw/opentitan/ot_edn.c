/*
 * QEMU OpenTitan Entropy Distribution Network device
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
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
 * Note: for now, only a minimalist subset of EDN device is implemented in order
 *       to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_csrng.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"


/* TODO: need to better understand HW behavior */
#undef EDN_DISCARD_PENDING_REQUEST_ON_DISABLE

#define PARAM_NUM_IRQS   2u
#define PARAM_NUM_ALERTS 2u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_EDN_CMD_REQ_DONE_BIT, 0u, 1u)
    SHARED_FIELD(INTR_EDN_FATAL_ERR_BIT, 1u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, RECOV_ALERT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_ALERT, 1u, 1u)
REG32(REGWEN, 0x10u)
    FIELD(REGWEN, EN, 0u, 1u)
REG32(CTRL, 0x14u)
    FIELD(CTRL, EDN_ENABLE, 0u, 4u)
    FIELD(CTRL, BOOT_REQ_MODE, 4u, 4u)
    FIELD(CTRL, AUTO_REQ_MODE, 8u, 4u)
    FIELD(CTRL, CMD_FIFO_RST, 12u, 4u)
REG32(BOOT_INS_CMD, 0x18u)
REG32(BOOT_GEN_CMD, 0x1cu)
REG32(SW_CMD_REQ, 0x20u)
REG32(SW_CMD_STS, 0x24u)
    FIELD(SW_CMD_STS, CMD_RDY, 0u, 1u)
    FIELD(SW_CMD_STS, CMD_STS, 1u, 1u)
REG32(RESEED_CMD, 0x28u)
REG32(GENERATE_CMD, 0x2cu)
REG32(MAX_NUM_REQS_BETWEEN_RESEEDS, 0x30u)
REG32(RECOV_ALERT_STS, 0x34u)
    FIELD(RECOV_ALERT_STS, EDN_ENABLE_FIELD_ALERT, 0u, 1u)
    FIELD(RECOV_ALERT_STS, BOOT_REQ_MODE_FIELD_ALERT, 1u, 1u)
    FIELD(RECOV_ALERT_STS, AUTO_REQ_MODE_FIELD_ALERT, 2u, 1u)
    FIELD(RECOV_ALERT_STS, CMD_FIFO_RST_FIELD_ALERT, 3u, 1u)
    FIELD(RECOV_ALERT_STS, EDN_BUS_CMP_ALERT, 12u, 1u)
REG32(ERR_CODE, 0x38u)
    FIELD(ERR_CODE, SFIFO_RESCMD_ERR, 0u, 1u)
    FIELD(ERR_CODE, SFIFO_GENCMD_ERR, 1u, 1u)
    FIELD(ERR_CODE, SFIFO_OUTPUT_ERR, 2u, 1u)
    FIELD(ERR_CODE, EDN_ACK_SM_ERR, 20u, 1u)
    FIELD(ERR_CODE, EDN_MAIN_SM_ERR, 21u, 1u)
    FIELD(ERR_CODE, EDN_CNTR_ERR, 22u, 1u)
    FIELD(ERR_CODE, FIFO_WRITE_ERR, 28u, 1u)
    FIELD(ERR_CODE, FIFO_READ_ERR, 29u, 1u)
    FIELD(ERR_CODE, FIFO_STATE_ERR, 30u, 1u)
REG32(ERR_CODE_TEST, 0x3cu)
    FIELD(ERR_CODE_TEST, VAL, 0u, 5u)
REG32(MAIN_SM_STATE, 0x40u)
    FIELD(MAIN_SM_STATE, VAL, 0u, 9u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MAIN_SM_STATE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define INTR_MASK (INTR_EDN_CMD_REQ_DONE_BIT_MASK | INTR_EDN_FATAL_ERR_BIT_MASK)
#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_ALERT_MASK | R_ALERT_TEST_FATAL_ALERT_MASK)
#define CTRL_MASK \
    (R_CTRL_EDN_ENABLE_MASK | R_CTRL_BOOT_REQ_MODE_MASK | \
     R_CTRL_AUTO_REQ_MODE_MASK | R_CTRL_CMD_FIFO_RST_MASK)
#define RECOV_ALERT_STS_MASK \
    (R_RECOV_ALERT_STS_EDN_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_BOOT_REQ_MODE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_AUTO_REQ_MODE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_CMD_FIFO_RST_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_EDN_BUS_CMP_ALERT_MASK)
#define ERR_CODE_MASK \
    (R_ERR_CODE_SFIFO_RESCMD_ERR_MASK | R_ERR_CODE_SFIFO_GENCMD_ERR_MASK | \
     R_ERR_CODE_SFIFO_OUTPUT_ERR_MASK | R_ERR_CODE_EDN_ACK_SM_ERR_MASK | \
     R_ERR_CODE_EDN_MAIN_SM_ERR_MASK | R_ERR_CODE_EDN_CNTR_ERR_MASK | \
     R_ERR_CODE_FIFO_WRITE_ERR_MASK | R_ERR_CODE_FIFO_READ_ERR_MASK | \
     R_ERR_CODE_FIFO_STATE_ERR_MASK)
#define ERR_CODE_FIFO_MASK \
    (R_ERR_CODE_SFIFO_RESCMD_ERR_MASK | R_ERR_CODE_SFIFO_GENCMD_ERR_MASK | \
     R_ERR_CODE_SFIFO_OUTPUT_ERR_MASK | R_ERR_CODE_FIFO_WRITE_ERR_MASK | \
     R_ERR_CODE_FIFO_READ_ERR_MASK | R_ERR_CODE_FIFO_STATE_ERR_MASK)

#define ALERT_STATUS_BIT(_x_) R_RECOV_ALERT_STS_##_x_##_FIELD_ALERT_MASK

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(REGWEN),
    REG_NAME_ENTRY(CTRL),
    REG_NAME_ENTRY(BOOT_INS_CMD),
    REG_NAME_ENTRY(BOOT_GEN_CMD),
    REG_NAME_ENTRY(SW_CMD_REQ),
    REG_NAME_ENTRY(SW_CMD_STS),
    REG_NAME_ENTRY(RESEED_CMD),
    REG_NAME_ENTRY(GENERATE_CMD),
    REG_NAME_ENTRY(MAX_NUM_REQS_BETWEEN_RESEEDS),
    REG_NAME_ENTRY(RECOV_ALERT_STS),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(ERR_CODE_TEST),
    REG_NAME_ENTRY(MAIN_SM_STATE),
};
#undef REG_NAME_ENTRY

#define ENDPOINT_COUNT_MAX 8u

#define xtrace_ot_edn_error(_id_, _msg_) \
    trace_ot_edn_error(_id_, __func__, __LINE__, _msg_)
#define xtrace_ot_edn_xinfo(_id_, _msg_, _val_) \
    trace_ot_edn_xinfo(_id_, __func__, __LINE__, _msg_, _val_)
#define xtrace_ot_edn_dinfo(_id_, _msg_, _val_) \
    trace_ot_edn_dinfo(_id_, __func__, __LINE__, _msg_, _val_)

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
};

typedef enum {
    EDN_IDLE, /* idle */
    EDN_BOOT_LOAD_INS, /* boot: load the instantiate command */
    EDN_BOOT_LOAD_GEN, /* boot: load the generate command */
    EDN_BOOT_INS_ACK_WAIT, /* boot: wait for instantiate command ack */
    EDN_BOOT_CAPT_GEN_CNT, /* boot: capture the gen fifo count */
    EDN_BOOT_SEND_GEN_CMD, /* boot: send the generate command */
    EDN_BOOT_GEN_ACK_WAIT, /* boot: wait for generate command ack */
    EDN_BOOT_PULSE, /* boot: signal a done pulse */
    EDN_BOOT_DONE, /* boot: stay in done state until reset */
    EDN_AUTO_LOAD_INS, /* auto: load the instantiate command */
    EDN_AUTO_FIRST_ACK_WAIT, /* auto: wait for first instantiate command ack */
    EDN_AUTO_ACK_WAIT, /* auto: wait for auto command ack */
    EDN_AUTO_DISPATCH, /* auto: determine next command to be sent */
    EDN_AUTO_CAPT_GEN_CNT, /* auto: capture the gen fifo count */
    EDN_AUTO_SEND_GEN_CMD, /* auto: send the generate command */
    EDN_AUTO_CAPT_RESEED_CNT, /* auto: capture the reseed fifo count */
    EDN_AUTO_SEND_RESEED_CMD, /* auto: send the reseed command */
    EDN_SW_PORT_MODE, /* swport: no hw request mode */
    EDN_ERROR, /* illegal state reached and hang */
} OtEDNFsmState;

typedef struct {
    OtCSRNGState *device; /* CSRNG instance */
    qemu_irq genbits_ready; /* Set when ready to receive entropy */
    uint32_t appid; /* unique HW application id to identify on CSRNG */
    bool instantiated; /* instantiated state, not yet uninstantiated */
    bool no_fips; /* true if 1+ rcv entropy packets were no FIPS-compliant */
    unsigned rem_packet_count; /* remaining expected packets in generate cmd */
    uint32_t buffer[OT_CSRNG_CMD_WORD_MAX]; /* temp buffer for commands */
    OtFifo32 bits_fifo; /* input FIFO with entropy received from CSRNG */
    OtFifo32 sw_cmd_fifo; /* generic command output FIFO */
    OtFifo32 cmd_gen_fifo; /* FIFO to store replayed generate command */
    OtFifo32 cmd_reseed_fifo; /* FIFO to store replayed reseed command */
} OtEDNCSRNG;

typedef struct OtEDNEndPoint {
    ot_edn_push_entropy_fn fn; /* function to call when entropy is available */
    void *opaque; /* opaque pointer forwaded with the fn() call */
    OtFifo32 fifo; /* output unpacker */
    bool fips; /* whether stored entropy is FIPS-compliant */
    size_t gen_count; /* Number of 32-bit entropy word in current generation */
    size_t total_count; /* Total number of 32-bit entropy words */
    QSIMPLEQ_ENTRY(OtEDNEndPoint) request;
} OtEDNEndPoint;

typedef QSIMPLEQ_HEAD(OtEndpointQueue, OtEDNEndPoint) OtEndpointQueue;

struct OtEDNState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    QEMUBH *ep_bh; /**< Endpoint requests */

    uint32_t *regs;

    unsigned reseed_counter; /* track remaining requests before reseeding */
    bool last_cmd_failed; /* status of the last CSRNG command */
    bool sw_cmd_ready; /* ready to receive command in SW port mode */
    OtEDNFsmState state; /* Main FSM state */
    OtEDNCSRNG rng;
    OtEDNEndPoint endpoints[ENDPOINT_COUNT_MAX];
    OtEndpointQueue ep_requests;
};

static const uint16_t OtEDNFsmStateCode[] = {
    [EDN_IDLE] = 0b110000101,
    [EDN_BOOT_LOAD_INS] = 0b110110111,
    [EDN_BOOT_LOAD_GEN] = 0b000000011,
    [EDN_BOOT_INS_ACK_WAIT] = 0b011010010,
    [EDN_BOOT_CAPT_GEN_CNT] = 0b010111010,
    [EDN_BOOT_SEND_GEN_CMD] = 0b011100100,
    [EDN_BOOT_GEN_ACK_WAIT] = 0b101101100,
    [EDN_BOOT_PULSE] = 0b100001010,
    [EDN_BOOT_DONE] = 0b011011111,
    [EDN_AUTO_LOAD_INS] = 0b001110000,
    [EDN_AUTO_FIRST_ACK_WAIT] = 0b001001101,
    [EDN_AUTO_ACK_WAIT] = 0b101100011,
    [EDN_AUTO_DISPATCH] = 0b110101110,
    [EDN_AUTO_CAPT_GEN_CNT] = 0b000110101,
    [EDN_AUTO_SEND_GEN_CMD] = 0b111111000,
    [EDN_AUTO_CAPT_RESEED_CNT] = 0b000100110,
    [EDN_AUTO_SEND_RESEED_CMD] = 0b101010110,
    [EDN_SW_PORT_MODE] = 0b100111001,
    [EDN_ERROR] = 0b010010001,
};

#define STATE_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
static const char *STATE_NAMES[] = {
    STATE_NAME_ENTRY(EDN_IDLE),
    STATE_NAME_ENTRY(EDN_BOOT_LOAD_INS),
    STATE_NAME_ENTRY(EDN_BOOT_LOAD_GEN),
    STATE_NAME_ENTRY(EDN_BOOT_INS_ACK_WAIT),
    STATE_NAME_ENTRY(EDN_BOOT_CAPT_GEN_CNT),
    STATE_NAME_ENTRY(EDN_BOOT_SEND_GEN_CMD),
    STATE_NAME_ENTRY(EDN_BOOT_GEN_ACK_WAIT),
    STATE_NAME_ENTRY(EDN_BOOT_PULSE),
    STATE_NAME_ENTRY(EDN_BOOT_DONE),
    STATE_NAME_ENTRY(EDN_AUTO_LOAD_INS),
    STATE_NAME_ENTRY(EDN_AUTO_FIRST_ACK_WAIT),
    STATE_NAME_ENTRY(EDN_AUTO_ACK_WAIT),
    STATE_NAME_ENTRY(EDN_AUTO_DISPATCH),
    STATE_NAME_ENTRY(EDN_AUTO_CAPT_GEN_CNT),
    STATE_NAME_ENTRY(EDN_AUTO_SEND_GEN_CMD),
    STATE_NAME_ENTRY(EDN_AUTO_CAPT_RESEED_CNT),
    STATE_NAME_ENTRY(EDN_AUTO_SEND_RESEED_CMD),
    STATE_NAME_ENTRY(EDN_SW_PORT_MODE),
    STATE_NAME_ENTRY(EDN_ERROR),
};
#undef STATE_NAME_ENTRY
#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")

static void ot_edn_fill_bits(void *opaque, const uint32_t *bits, bool fips);
static void ot_edn_csrng_ack_irq(void *opaque, int n, int level);

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

void ot_edn_connect_endpoint(OtEDNState *s, unsigned ep_id,
                             ot_edn_push_entropy_fn fn, void *opaque)
{
    assert(ep_id < ENDPOINT_COUNT_MAX);
    assert(fn);

    OtEDNEndPoint *ep = &s->endpoints[ep_id];
    if (ep->fn) {
        assert(ep->fn == fn);
    } else {
        trace_ot_edn_connect_endpoint(s->rng.appid, ep_id);
    }

    ep->fn = fn;
    ep->opaque = opaque;
}

int ot_edn_request_entropy(OtEDNState *s, unsigned ep_id)
{
    trace_ot_edn_request_entropy(s->rng.appid, ep_id);

    assert(ep_id < ENDPOINT_COUNT_MAX);
    OtEDNEndPoint *ep = &s->endpoints[ep_id];
    if (!ep->fn) {
        xtrace_ot_edn_error(s->rng.appid,
                            "entropy request w/o initial connection");
        return -1;
    }

    if (QSIMPLEQ_NEXT(ep, request) ||
        QSIMPLEQ_LAST(&s->ep_requests, OtEDNEndPoint, request) == ep) {
        xtrace_ot_edn_error(s->rng.appid, "endpoint already scheduled");
        return -1;
    }

    QSIMPLEQ_INSERT_TAIL(&s->ep_requests, ep, request);

    trace_ot_edn_schedule(s->rng.appid, "external entropy request");
    qemu_bh_schedule(s->ep_bh);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static void ot_edn_update_irqs(OtEDNState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_edn_irqs(s->rng.appid, s->regs[R_INTR_STATE],
                      s->regs[R_INTR_ENABLE], level);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_edn_report_alert(OtEDNState *s)
{
    if (s->regs[R_RECOV_ALERT_STS]) {
        ibex_irq_set(&s->alerts[ALERT_RECOVERABLE], 1);
    }
}

static void ot_edn_signal_error(OtEDNState *s, unsigned error)
{
    uint32_t bit = (1u << error) & ERR_CODE_MASK;
    if (bit) {
        s->regs[R_ERR_CODE] |= bit;
        ibex_irq_set(&s->alerts[ALERT_FATAL], 1);
    }
}

static void ot_edn_check_multibitboot(OtEDNState *s, uint8_t mbbool,
                                      uint32_t alert_bit)
{
    switch (mbbool) {
    case OT_MULTIBITBOOL4_TRUE:
    case OT_MULTIBITBOOL4_FALSE:
        return;
    default:
        break;
    }

    s->regs[R_RECOV_ALERT_STS] |= 1u << alert_bit;
    ot_edn_report_alert(s);
}

static bool ot_edn_is_enabled(OtEDNState *s)
{
    uint32_t enable = FIELD_EX32(s->regs[R_CTRL], CTRL, EDN_ENABLE);

    return enable == OT_MULTIBITBOOL4_TRUE;
}

static bool ot_edn_is_disabled(OtEDNState *s)
{
    uint32_t enable = FIELD_EX32(s->regs[R_CTRL], CTRL, EDN_ENABLE);

    return enable == OT_MULTIBITBOOL4_FALSE;
}

static bool ot_edn_is_boot_req_mode(OtEDNState *s)
{
    uint32_t auto_req = FIELD_EX32(s->regs[R_CTRL], CTRL, BOOT_REQ_MODE);

    return auto_req == OT_MULTIBITBOOL4_TRUE;
}

static bool ot_edn_is_auto_req_mode(OtEDNState *s)
{
    uint32_t auto_req = FIELD_EX32(s->regs[R_CTRL], CTRL, AUTO_REQ_MODE);

    return auto_req == OT_MULTIBITBOOL4_TRUE;
}

static bool ot_edn_is_sw_port_mode(OtEDNState *s)
{
    return !ot_edn_is_boot_req_mode(s) && !ot_edn_is_auto_req_mode(s);
}

static OtCsrngCmd ot_edn_get_last_csrng_command(OtEDNState *s)
{
    const OtEDNCSRNG *c = &s->rng;

    uint32_t last_cmd = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, ACMD);

    switch (last_cmd) {
    case OT_CSRNG_CMD_NONE:
    case OT_CSRNG_CMD_INSTANTIATE:
    case OT_CSRNG_CMD_RESEED:
    case OT_CSRNG_CMD_GENERATE:
    case OT_CSRNG_CMD_UPDATE:
    case OT_CSRNG_CMD_UNINSTANTIATE:
        return (OtCsrngCmd)last_cmd;
    default:
        g_assert_not_reached();
    }
}

static bool ot_edn_is_cmd_rdy(OtEDNState *s)
{
    if (!ot_edn_is_enabled(s)) {
        return false;
    }
    if (ot_fifo32_is_full(&s->rng.sw_cmd_fifo)) {
        return false;
    }
    switch (s->state) {
    case EDN_IDLE:
    case EDN_BOOT_PULSE:
    case EDN_BOOT_DONE:
    case EDN_AUTO_LOAD_INS:
    case EDN_AUTO_FIRST_ACK_WAIT:
        return true;
    case EDN_SW_PORT_MODE:
        return s->sw_cmd_ready;
    default:
        return false;
    }
}

static void ot_edn_change_state_line(OtEDNState *s, OtEDNFsmState state,
                                     int line)
{
    trace_ot_edn_change_state(s->rng.appid, line, STATE_NAME(s->state),
                              s->state, STATE_NAME(state), state);

    s->state = state;

    if (s->state == EDN_ERROR) {
        s->regs[R_ERR_CODE] |= R_ERR_CODE_EDN_MAIN_SM_ERR_MASK;
        ot_edn_report_alert(s);
    }
}

#define ot_edn_change_state(_s_, _st_) \
    ot_edn_change_state_line(_s_, _st_, __LINE__)

static bool ot_edn_update_genbits_ready(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    bool accept_entropy =
        ot_edn_is_enabled(s) && (c->rem_packet_count > 0) &&
        ot_fifo32_num_free(&c->bits_fifo) > OT_CSRNG_PACKET_WORD_COUNT;

    trace_ot_edn_update_genbits_ready(c->appid, c->rem_packet_count,
                                      ot_fifo32_num_free(&c->bits_fifo) /
                                          sizeof(uint32_t),
                                      accept_entropy);
    qemu_set_irq(c->genbits_ready, accept_entropy);

    return accept_entropy;
}

static int ot_edn_push_csrng_request(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    /* if the IRQ is not initialized, the EDN has yet to connected to CSRNG */
    if (!c->genbits_ready) {
        qemu_irq req_sts;
        req_sts = qdev_get_gpio_in_named(DEVICE(s), TYPE_OT_EDN "-req_sts", 0);
        c->genbits_ready = ot_csnrg_connect_hw_app(c->device, c->appid, req_sts,
                                                   &ot_edn_fill_bits, s);
        assert(c->genbits_ready);
    }
    int res = ot_csrng_push_command(c->device, c->appid, c->buffer);
    if (res) {
        xtrace_ot_edn_error(c->appid, "CSRNG rejected command");
        /* do not expect any delayed completion */
        memset(c->buffer, 0, sizeof(*c->buffer));
    }

    return res;
}

static void ot_edn_send_boot_req(OtEDNState *s, unsigned reg)
{
    OtEDNCSRNG *c = &s->rng;

    uint32_t command = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, ACMD);
    if (command != OT_CSRNG_CMD_NONE) {
        xtrace_ot_edn_error(c->appid, "Another command is already scheduled");
        ot_edn_change_state(s, EDN_ERROR);
        return;
    }

    c->buffer[0u] = s->regs[reg];
    command = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, ACMD);

    if (command == OT_CSRNG_CMD_GENERATE) {
        c->rem_packet_count = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, GLEN);
        xtrace_ot_edn_dinfo(c->appid, "Boot generation w/ packets",
                            c->rem_packet_count);
        ot_edn_update_genbits_ready(s);
        ot_edn_change_state(s, EDN_BOOT_CAPT_GEN_CNT);
        for (unsigned epix = 0; epix < ARRAY_SIZE(s->endpoints); epix++) {
            s->endpoints[epix].gen_count = 0;
        }
    }
    if (ot_edn_push_csrng_request(s)) {
        xtrace_ot_edn_error(c->appid, "Cannot push command");
        ot_edn_change_state(s, EDN_ERROR);
        return;
    }

    /*
     * CSRNG request should be completed asynchronously, changing the state here
     * should occur before ot_edn_csrng_ack_irq is called.
     */
    switch (command) {
    case OT_CSRNG_CMD_INSTANTIATE:
        ot_edn_change_state(s, EDN_BOOT_INS_ACK_WAIT);
        break;
    case OT_CSRNG_CMD_GENERATE:
        ot_edn_change_state(s, EDN_BOOT_GEN_ACK_WAIT);
        break;
    default:
        g_assert_not_reached();
    }
}

static void ot_edn_try_auto_instantiate(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    if (ot_fifo32_is_empty(&c->sw_cmd_fifo)) {
        /* instantiate command not yet loaded into the SW_CMD_REQ */
        xtrace_ot_edn_error(c->appid, "no SW cmd in FIFO");
        return;
    }

    uint32_t command = ot_fifo32_peek(&c->sw_cmd_fifo);
    uint32_t length = FIELD_EX32(command, OT_CSNRG_CMD, CLEN) + 1u;
    if (ot_fifo32_num_used(&c->sw_cmd_fifo) < length) {
        /* instantiate command not fully loaded into the SW_CMD_REQ */
        xtrace_ot_edn_error(c->appid, "SW cmd FIFO incomplete");
        return;
    }

    uint32_t num = length;
    const uint32_t *cmd = ot_fifo32_pop_buf(&c->sw_cmd_fifo, num, &num);
    assert(num == length);
    memcpy(c->buffer, cmd, length * sizeof(uint32_t));

    if (ot_edn_push_csrng_request(s)) {
        xtrace_ot_edn_error(c->appid, "Cannot execute command");
        ot_edn_change_state(s, EDN_ERROR);
        return;
    }

    ot_edn_change_state(s, EDN_AUTO_FIRST_ACK_WAIT);
}

static void ot_edn_send_reseed_cmd(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    if (ot_fifo32_is_empty(&c->cmd_reseed_fifo)) {
        s->regs[R_ERR_CODE] |=
            R_ERR_CODE_SFIFO_RESCMD_ERR_MASK | R_ERR_CODE_FIFO_READ_ERR_MASK;
        s->regs[R_INTR_STATE] |= INTR_EDN_FATAL_ERR_BIT_MASK;
        ot_edn_update_irqs(s);
        ot_edn_report_alert(s);
        return;
    }

    uint32_t command = ot_fifo32_peek(&c->cmd_reseed_fifo);
    uint32_t length = FIELD_EX32(command, OT_CSNRG_CMD, CLEN) + 1u;
    if (ot_fifo32_num_used(&c->cmd_reseed_fifo) < length) {
        s->regs[R_ERR_CODE] |=
            R_ERR_CODE_SFIFO_RESCMD_ERR_MASK | R_ERR_CODE_FIFO_READ_ERR_MASK;

        s->regs[R_INTR_STATE] |= INTR_EDN_FATAL_ERR_BIT_MASK;
        ot_edn_update_irqs(s);
        ot_edn_report_alert(s);
        return;
    }

    ot_edn_change_state(s, EDN_AUTO_SEND_RESEED_CMD);

    uint32_t num = length;
    const uint32_t *cmd = ot_fifo32_peek_buf(&c->cmd_reseed_fifo, num, &num);
    assert(num == length);
    memcpy(c->buffer, cmd, length * sizeof(uint32_t));

    if (ot_edn_push_csrng_request(s)) {
        xtrace_ot_edn_error(c->appid, "Cannot execute command");
        ot_edn_change_state(s, EDN_ERROR);
        return;
    }

    ot_edn_change_state(s, EDN_AUTO_ACK_WAIT);
}

static void ot_edn_send_generate_cmd(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    if (ot_fifo32_is_empty(&c->cmd_gen_fifo)) {
        s->regs[R_ERR_CODE] |=
            R_ERR_CODE_SFIFO_GENCMD_ERR_MASK | R_ERR_CODE_FIFO_READ_ERR_MASK;
        s->regs[R_INTR_STATE] |= INTR_EDN_FATAL_ERR_BIT_MASK;
        ot_edn_update_irqs(s);
        ot_edn_report_alert(s);
        return;
    }

    uint32_t command = ot_fifo32_peek(&c->cmd_gen_fifo);
    xtrace_ot_edn_xinfo(c->appid, "COMMAND", command);
    uint32_t length = FIELD_EX32(command, OT_CSNRG_CMD, CLEN) + 1u;
    if (ot_fifo32_num_used(&c->cmd_gen_fifo) < length) {
        s->regs[R_ERR_CODE] |=
            R_ERR_CODE_SFIFO_GENCMD_ERR_MASK | R_ERR_CODE_FIFO_READ_ERR_MASK;
        s->regs[R_INTR_STATE] |= INTR_EDN_FATAL_ERR_BIT_MASK;
        ot_edn_update_irqs(s);
        ot_edn_report_alert(s);
        return;
    }

    uint32_t num = length;
    const uint32_t *cmd = ot_fifo32_peek_buf(&c->cmd_gen_fifo, num, &num);
    assert(num == length);
    memcpy(c->buffer, cmd, length * sizeof(uint32_t));
    c->rem_packet_count = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, GLEN);
    xtrace_ot_edn_dinfo(c->appid, "Generate cmd w/ packets",
                        c->rem_packet_count);
    ot_edn_update_genbits_ready(s);

    ot_edn_change_state(s, EDN_AUTO_SEND_GEN_CMD);

    for (unsigned epix = 0; epix < ARRAY_SIZE(s->endpoints); epix++) {
        s->endpoints[epix].gen_count = 0;
    }
    if (ot_edn_push_csrng_request(s)) {
        xtrace_ot_edn_error(c->appid, "Cannot execute command");
        ot_edn_change_state(s, EDN_ERROR);
        return;
    }

    if (s->reseed_counter) {
        s->reseed_counter -= 1u;
    }

    ot_edn_change_state(s, EDN_AUTO_ACK_WAIT);
}

static void ot_edn_send_uninstanciate_cmd(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    memset(c->buffer, 0, sizeof(c->buffer));
    c->buffer[0u] =
        FIELD_DP32(0, OT_CSNRG_CMD, ACMD, OT_CSRNG_CMD_UNINSTANTIATE);

    if (ot_edn_push_csrng_request(s)) {
        xtrace_ot_edn_error(c->appid, "Cannot execute command");
        ot_edn_change_state(s, EDN_ERROR);
        return;
    }

    /* TODO: what is the actual state of uninstantiating from boot mode? */
    ot_edn_change_state(s, EDN_AUTO_ACK_WAIT);
}

static void ot_edn_try_sw_request(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    uint32_t command = ot_fifo32_peek(&c->sw_cmd_fifo);
    uint32_t length = FIELD_EX32(command, OT_CSNRG_CMD, CLEN) + 1u;
    if (ot_fifo32_num_used(&c->sw_cmd_fifo) != length) {
        /* command not yet complete */
        return;
    }

    assert(s->state == EDN_SW_PORT_MODE);

    uint32_t num = length;
    const uint32_t *cmd = ot_fifo32_peek_buf(&c->sw_cmd_fifo, num, &num);
    memcpy(c->buffer, cmd, length * sizeof(uint32_t));
    ot_fifo32_reset(&c->sw_cmd_fifo);

    s->sw_cmd_ready = false;

    command = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, ACMD);

    if (command == OT_CSRNG_CMD_GENERATE) {
        c->rem_packet_count = FIELD_EX32(c->buffer[0], OT_CSNRG_CMD, GLEN);
        xtrace_ot_edn_dinfo(c->appid, "SW generation w/ packets",
                            c->rem_packet_count);
        ot_edn_update_genbits_ready(s);
        for (unsigned epix = 0; epix < ARRAY_SIZE(s->endpoints); epix++) {
            s->endpoints[epix].gen_count = 0;
        }
    }

    if (ot_edn_push_csrng_request(s)) {
        xtrace_ot_edn_error(c->appid, "Cannot execute command");
        ot_edn_change_state(s, EDN_ERROR);
        s->sw_cmd_ready = true;
        return;
    }
}

static void ot_edn_handle_disable(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    assert(ot_edn_get_last_csrng_command(s) != OT_CSRNG_CMD_UNINSTANTIATE);

    c->no_fips = false;
    c->rem_packet_count = 0;
    xtrace_ot_edn_dinfo(c->appid, "discard all entropy packets",
                        ot_fifo32_num_used(&c->bits_fifo));
    ot_fifo32_reset(&c->bits_fifo);

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->endpoints); ix++) {
        OtEDNEndPoint *ep = &s->endpoints[ix];
        ot_fifo32_reset(&ep->fifo);
        ep->fips = false;
    }

/* discard any on-going EP request */
#ifdef EDN_DISCARD_PENDING_REQUEST_ON_DISABLE
    OtEDNEndPoint *tmp, *next;
    QSIMPLEQ_FOREACH_SAFE(tmp, &s->ep_requests, request, next) {
        QSIMPLEQ_REMOVE_HEAD(&s->ep_requests, request);
    }
#endif

    /* signal CSRNG that EDN is no longer ready to receive entropy */
    ot_edn_update_genbits_ready(s);

    switch (s->state) {
    case EDN_BOOT_GEN_ACK_WAIT:
        /* no longer expect a generate ack */
        ot_edn_change_state(s, EDN_BOOT_DONE);
        break;
    case EDN_AUTO_ACK_WAIT:
        /* no longer expect a generate ack */
        ot_edn_change_state(s, EDN_IDLE);
        break;
    default:
        break;
    }

    ot_edn_send_uninstanciate_cmd(s);
}

static void ot_edn_complete_sw_req(OtEDNState *s)
{
    s->sw_cmd_ready = true;
    s->regs[R_INTR_STATE] |= INTR_EDN_CMD_REQ_DONE_BIT_MASK;
    ot_edn_update_irqs(s);
}

static void ot_edn_dispatch(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    if (ot_edn_is_boot_req_mode(s)) {
        xtrace_ot_edn_dinfo(c->appid, "Dispatch in boot mode",
                            c->rem_packet_count);
        s->reseed_counter = s->regs[R_MAX_NUM_REQS_BETWEEN_RESEEDS];
        ot_edn_change_state(s, EDN_IDLE);
        return;
    }

    if (ot_edn_is_sw_port_mode(s)) {
        xtrace_ot_edn_dinfo(c->appid, "Dispatch in sw port mode", 0);
        ot_edn_complete_sw_req(s);
        return;
    }

    xtrace_ot_edn_dinfo(c->appid, "Dispatch in auto mode", c->rem_packet_count);
    if (s->reseed_counter == 0) {
        ot_edn_change_state(s, EDN_AUTO_CAPT_RESEED_CNT);
        ot_edn_send_reseed_cmd(s);
        s->reseed_counter = s->regs[R_MAX_NUM_REQS_BETWEEN_RESEEDS];
    } else {
        ot_edn_change_state(s, EDN_AUTO_CAPT_GEN_CNT);
        ot_edn_send_generate_cmd(s);
    }
}

static void ot_edn_clean_up(OtEDNState *s)
{
    OtEDNCSRNG *c = &s->rng;

    c->instantiated = false;
    s->sw_cmd_ready = false;
    s->last_cmd_failed = false;
    s->reseed_counter = 0;
    memset(c->buffer, 0, sizeof(*c->buffer));
    ot_fifo32_reset(&c->bits_fifo);
    ot_fifo32_reset(&c->sw_cmd_fifo);
    ot_edn_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }
#ifdef EDN_DISCARD_PENDING_REQUEST_ON_DISABLE
    /* clear all pending end point requests */
    while (QSIMPLEQ_FIRST(&s->ep_requests)) {
        QSIMPLEQ_REMOVE_HEAD(&s->ep_requests, request);
    }
#endif
    for (unsigned epix = 0; epix < ARRAY_SIZE(s->endpoints); epix++) {
        ot_fifo32_reset(&s->endpoints[epix].fifo);
        s->endpoints[epix].fips = false;
    }
    if (c->genbits_ready) {
        qemu_set_irq(c->genbits_ready, 0);
    }
    ot_edn_change_state(s, EDN_IDLE);
}

static void ot_edn_fill_bits(void *opaque, const uint32_t *bits, bool fips)
{
    OtEDNState *s = opaque;
    OtEDNCSRNG *c = &s->rng;

    switch (s->state) {
    case EDN_BOOT_GEN_ACK_WAIT:
    case EDN_AUTO_ACK_WAIT:
    case EDN_SW_PORT_MODE:
        break;
    default:
        xtrace_ot_edn_error(c->appid, "unexpected state");
        ot_edn_change_state(s, EDN_ERROR);
        exit(1);
        break;
    }

    if (!c->rem_packet_count ||
        ot_fifo32_num_free(&c->bits_fifo) < OT_CSRNG_PACKET_WORD_COUNT) {
        xtrace_ot_edn_error(c->appid, "unexpected entropy");
        return;
    }

    for (unsigned ix = 0; ix < OT_CSRNG_PACKET_WORD_COUNT; ix++) {
        if (ot_fifo32_is_full(&c->bits_fifo)) {
            xtrace_ot_edn_error(c->appid, "entropy input fifo overflow");
            s->regs[R_ERR_CODE] |= R_ERR_CODE_SFIFO_GENCMD_ERR_MASK |
                                   R_ERR_CODE_FIFO_WRITE_ERR_MASK;
            s->regs[R_INTR_STATE] |= INTR_EDN_FATAL_ERR_BIT_MASK;
            ot_edn_update_irqs(s);
            ot_edn_report_alert(s);
            return;
        }
        ot_fifo32_push(&c->bits_fifo, bits[ix]);
    }
    c->rem_packet_count -= 1u;
    c->no_fips |= !fips;

    trace_ot_edn_fill_bits(c->appid, c->rem_packet_count, fips, !c->no_fips);

    ot_edn_update_genbits_ready(s);

    /* serve any queued enpoints if any */
    if (!QSIMPLEQ_EMPTY(&s->ep_requests)) {
        trace_ot_edn_schedule(c->appid, "queued entropy request");
        qemu_bh_schedule(s->ep_bh);
    }
}

static void ot_edn_handle_ep_request(void *opaque)
{
    /* called from ep_bh */
    OtEDNState *s = opaque;
    OtEDNCSRNG *c = &s->rng;

    OtEDNEndPoint *ep = QSIMPLEQ_FIRST(&s->ep_requests);
    if (!ep) {
        xtrace_ot_edn_error(c->appid, "scheduled request w/o request?");
        g_assert_not_reached();
    }

    unsigned ep_id = (unsigned)(uintptr_t)(ep - &s->endpoints[0]);
    trace_ot_edn_handle_ep_request(c->appid, ep_id);

    uint32_t bits;
    bool available; /* entropy is available in EP unpacker output */
    if (ot_fifo32_is_empty(&ep->fifo)) {
        /* if the local packer is empty ... */
        if (ot_fifo32_num_used(&c->bits_fifo) >= OT_CSRNG_PACKET_WORD_COUNT) {
            /*
             * .... try to refill if from the input entropy FIFO, 128 bits
             * at a time, first 32-bit packet is pushed to the EP client,
             * remaining 96-bits are stored in the unpacker FIFO
             */
            bits = ot_fifo32_pop(&c->bits_fifo);
            for (unsigned int ix = 1; ix < OT_CSRNG_PACKET_WORD_COUNT; ix++) {
                ot_fifo32_push(&ep->fifo, ot_fifo32_pop(&c->bits_fifo));
            }
            ep->fips = !c->no_fips;
            available = true;
            trace_ot_edn_ep_fifo(c->appid, "reload",
                                 ot_fifo32_num_used(&ep->fifo));
        } else {
            /* ... otherwise, the input FIFO being empty, defer handling */
            available = false;
            trace_ot_edn_ep_fifo(c->appid, "empty", 0);
        }
    } else {
        bits = ot_fifo32_pop(&ep->fifo);
        available = true;
        trace_ot_edn_ep_fifo(c->appid, "pop", ot_fifo32_num_used(&ep->fifo));
    }

    bool refill = ot_edn_update_genbits_ready(s);

    trace_ot_edn_ep_request(c->appid, available, STATE_NAME(s->state), s->state,
                            refill, c->rem_packet_count);

    if (available) {
        /* remove the request from the queue only if it has been handled */
        QSIMPLEQ_REMOVE_HEAD(&s->ep_requests, request);
        /* signal the endpoint with the requested entropy */
        if (ep->fn) {
            ep->gen_count += 1u;
            ep->total_count += 1u;
            trace_ot_edn_fill_endpoint(c->appid, ep_id, bits, ep->fips,
                                       ep->gen_count, ep->total_count);
            (*ep->fn)(ep->opaque, bits, ep->fips);
        } else {
            xtrace_ot_edn_error(c->appid, "no filler function");
        }
    }
}

static void ot_edn_csrng_ack_irq(void *opaque, int n, int level)
{
    OtEDNState *s = opaque;
    OtEDNCSRNG *c = &s->rng;

    trace_ot_edn_csrng_ack(c->appid, STATE_NAME(s->state), level);

    OtCsrngCmd last_cmd = ot_edn_get_last_csrng_command(s);
    /*
     * cleaning up the first world would be enough, clearing the whole buffer
     * help debugging
     */
    memset(c->buffer, 0, sizeof(*c->buffer));

    s->last_cmd_failed = level != 0;

    if (level) {
        xtrace_ot_edn_error(c->appid, "last command failed");
        ot_edn_change_state(s, EDN_ERROR);
        /* TODO: better error handling is required (IRQ signalling, ...)*/
        g_assert_not_reached();
        return;
    }

    /* success */
    switch (last_cmd) {
    case OT_CSRNG_CMD_INSTANTIATE:
        c->instantiated = true;
        break;
    case OT_CSRNG_CMD_UNINSTANTIATE:
        if (ot_edn_is_sw_port_mode(s)) {
            ot_edn_complete_sw_req(s);
        }
        ot_edn_clean_up(s);
        return;
    default:
        break;
    }

    switch (s->state) {
    case EDN_BOOT_INS_ACK_WAIT:
        ot_edn_change_state(s, EDN_BOOT_LOAD_GEN);
        ot_edn_send_boot_req(s, R_BOOT_GEN_CMD);
        break;
    case EDN_BOOT_GEN_ACK_WAIT:
        /* todo: boot pulse/done only activated once the generation is over? */
        ot_edn_change_state(s, EDN_BOOT_PULSE);
        ot_edn_change_state(s, EDN_BOOT_DONE);
        break;
    case EDN_AUTO_FIRST_ACK_WAIT:
        ot_edn_change_state(s, EDN_AUTO_DISPATCH);
        ot_edn_dispatch(s);
        break;
    case EDN_AUTO_ACK_WAIT:
        ot_edn_change_state(s, EDN_AUTO_DISPATCH);
        ot_edn_dispatch(s);
        break;
    case EDN_SW_PORT_MODE:
        ot_edn_dispatch(s);
        break;
    default:
        trace_ot_edn_invalid_state(c->appid, __func__, STATE_NAME(s->state),
                                   s->state);
        s->regs[R_ERR_CODE] |= R_ERR_CODE_EDN_ACK_SM_ERR_MASK;
        ot_edn_report_alert(s);
        g_assert_not_reached();
    }
}

static uint64_t ot_edn_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtEDNState *s = opaque;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_REGWEN:
    case R_CTRL:
    case R_BOOT_INS_CMD:
    case R_BOOT_GEN_CMD:
    case R_MAX_NUM_REQS_BETWEEN_RESEEDS:
    case R_RECOV_ALERT_STS:
    case R_ERR_CODE:
    case R_ERR_CODE_TEST:
        val32 = s->regs[reg];
        break;
    case R_SW_CMD_STS:
        val32 =
            FIELD_DP32(0, SW_CMD_STS, CMD_STS, (uint32_t)s->last_cmd_failed);
        val32 = FIELD_DP32(val32, SW_CMD_STS, CMD_RDY,
                           (uint32_t)ot_edn_is_cmd_rdy(s));
        break;
    case R_MAIN_SM_STATE:
        switch (s->state) {
        case EDN_IDLE ... EDN_ERROR:
            val32 = OtEDNFsmStateCode[s->state];
            break;
        default:
            val32 = OtEDNFsmStateCode[EDN_ERROR];
            break;
        }
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_SW_CMD_REQ:
    case R_RESEED_CMD:
    case R_GENERATE_CMD:
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
    trace_ot_edn_io_read_out(s->rng.appid, (unsigned)addr, REG_NAME(reg),
                             (uint64_t)val32, pc);

    return (uint64_t)val32;
};

#define CHECK_MULTIBOOT(_s_, _r_, _b_) \
    ot_edn_check_multibitboot((_s_), FIELD_EX32(s->regs[R_##_r_], _r_, _b_), \
                              ALERT_STATUS_BIT(_b_));

static void ot_edn_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned size)
{
    OtEDNState *s = opaque;
    OtEDNCSRNG *c = &s->rng;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_edn_io_write(c->appid, (unsigned)addr, REG_NAME(reg), val64, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_edn_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_edn_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_edn_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        if (val32) {
            for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
                ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
            }
        }
        break;
    case R_REGWEN:
        val32 &= R_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_CTRL:
        if (s->regs[R_REGWEN]) {
            val32 &= CTRL_MASK;
            s->regs[reg] = val32;
            CHECK_MULTIBOOT(s, CTRL, EDN_ENABLE);
            CHECK_MULTIBOOT(s, CTRL, BOOT_REQ_MODE);
            CHECK_MULTIBOOT(s, CTRL, AUTO_REQ_MODE);
            CHECK_MULTIBOOT(s, CTRL, CMD_FIFO_RST);
            if (FIELD_EX32(s->regs[reg], CTRL, CMD_FIFO_RST) ==
                OT_MULTIBITBOOL4_TRUE) {
                ot_fifo32_reset(&s->rng.cmd_gen_fifo);
                ot_fifo32_reset(&s->rng.cmd_reseed_fifo);
            }
            trace_ot_edn_ctrl_in_state(c->appid, STATE_NAME(s->state),
                                       s->state);
            if (s->state == EDN_IDLE) {
                if (ot_edn_is_enabled(s)) {
                    if (ot_edn_is_boot_req_mode(s)) {
                        trace_ot_edn_enable(c->appid, "boot mode");
                        ot_edn_change_state(s, EDN_BOOT_LOAD_INS);
                        ot_edn_send_boot_req(s, R_BOOT_INS_CMD);
                    } else if (ot_edn_is_auto_req_mode(s)) {
                        trace_ot_edn_enable(c->appid, "auto mode");
                        ot_edn_change_state(s, EDN_AUTO_LOAD_INS);
                        ot_edn_try_auto_instantiate(s);
                    } else {
                        trace_ot_edn_enable(c->appid, "sw mode");
                        ot_fifo32_reset(&c->sw_cmd_fifo);
                        s->sw_cmd_ready = true;
                        ot_edn_change_state(s, EDN_SW_PORT_MODE);
                    }
                }
            } else if (ot_edn_is_disabled(s)) {
                OtCsrngCmd last_cmd = ot_edn_get_last_csrng_command(s);
                /* do not pile up uninstanciation requests */
                if (last_cmd != OT_CSRNG_CMD_UNINSTANTIATE) {
                    trace_ot_edn_enable(c->appid, "disabling");
                    ot_edn_handle_disable(s);
                }
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Cannot change CTRL, REGWEN disabled");
        }
        break;
    case R_BOOT_INS_CMD:
    case R_BOOT_GEN_CMD:
        s->regs[reg] = val32;
        break;
    case R_SW_CMD_REQ:
        /* ignore all sw commands in auto req mode once instantiated */
        if (ot_edn_is_auto_req_mode(s) && c->instantiated) {
            xtrace_ot_edn_dinfo(c->appid, "ignore SW REQ", c->appid);
            break;
        }
        if (!ot_fifo32_is_full(&c->sw_cmd_fifo)) {
            ot_fifo32_push(&c->sw_cmd_fifo, val32);
            if (s->state == EDN_AUTO_LOAD_INS) {
                ot_edn_try_auto_instantiate(s);
                break;
            }
            if (ot_edn_is_sw_port_mode(s)) {
                ot_edn_try_sw_request(s);
                break;
            }
        } else {
            ot_edn_signal_error(s, R_ERR_CODE_FIFO_WRITE_ERR_SHIFT);
            xtrace_ot_edn_error(c->appid, "sw fifo full");
        }
        break;
    case R_RESEED_CMD:
        if (ot_fifo32_is_full(&c->cmd_reseed_fifo)) {
            ot_edn_signal_error(s, R_ERR_CODE_SFIFO_RESCMD_ERR_SHIFT);
        } else {
            ot_fifo32_push(&c->cmd_reseed_fifo, val32);
        }
        break;
    case R_GENERATE_CMD:
        if (ot_fifo32_is_full(&c->cmd_gen_fifo)) {
            ot_edn_signal_error(s, R_ERR_CODE_SFIFO_GENCMD_ERR_SHIFT);
        } else {
            xtrace_ot_edn_xinfo(c->appid, "PUSH GEN CMD", val32);
            ot_fifo32_push(&c->cmd_gen_fifo, val32);
        }
        break;
    case R_MAX_NUM_REQS_BETWEEN_RESEEDS:
        s->regs[reg] = val32;
        s->reseed_counter = val32;
        break;
    case R_RECOV_ALERT_STS:
        val32 &= RECOV_ALERT_STS_MASK;
        s->regs[reg] = val32;
        break;
    case R_ERR_CODE_TEST:
        val32 &= R_ERR_CODE_TEST_VAL_MASK;
        ot_edn_signal_error(s, (unsigned)val32);
        break;
    case R_SW_CMD_STS:
    case R_ERR_CODE:
    case R_MAIN_SM_STATE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "R/O register 0x02%" HWADDR_PRIx " (%s)\n", addr,
                      REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_edn_properties[] = {
    DEFINE_PROP_LINK("csrng", OtEDNState, rng.device, TYPE_OT_CSRNG,
                     OtCSRNGState *),
    DEFINE_PROP_UINT32("csrng-app", OtEDNState, rng.appid, UINT32_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_edn_regs_ops = {
    .read = &ot_edn_regs_read,
    .write = &ot_edn_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_edn_reset(DeviceState *dev)
{
    OtEDNState *s = OT_EDN(dev);
    OtEDNCSRNG *c = &s->rng;

    memset(s->regs, 0, REGS_SIZE);
    s->regs[R_REGWEN] = 0x1u;
    s->regs[R_CTRL] = 0x9999u;
    s->regs[R_BOOT_INS_CMD] = 0x901u;
    s->regs[R_BOOT_GEN_CMD] = 0xfff003u;

    ot_edn_clean_up(s);

    /* do not reset connection info since reset order is not known */
    (void)c->genbits_ready;
    ot_fifo32_reset(&c->cmd_gen_fifo);
    ot_fifo32_reset(&c->cmd_reseed_fifo);

    /* check that properties have been initialized */
    assert(c->device);
    assert(c->appid < OT_CSRNG_HW_APP_MAX);
}

static void ot_edn_init(Object *obj)
{
    OtEDNState *s = OT_EDN(obj);
    OtEDNCSRNG *c = &s->rng;

    memory_region_init_io(&s->mmio, obj, &ot_edn_regs_ops, s, TYPE_OT_EDN,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }
    qdev_init_gpio_in_named_with_opaque(DEVICE(s), &ot_edn_csrng_ack_irq, s,
                                        TYPE_OT_EDN "-req_sts", 1);

    s->ep_bh = qemu_bh_new(&ot_edn_handle_ep_request, s);

    ot_fifo32_create(&c->bits_fifo, OT_CSRNG_PACKET_WORD_COUNT * 2u);
    ot_fifo32_create(&c->sw_cmd_fifo, OT_CSRNG_CMD_WORD_MAX);
    ot_fifo32_create(&c->cmd_gen_fifo, OT_CSRNG_CMD_WORD_MAX);
    ot_fifo32_create(&c->cmd_reseed_fifo, OT_CSRNG_CMD_WORD_MAX);

    for (unsigned epix = 0; epix < ARRAY_SIZE(s->endpoints); epix++) {
        memset(&s->endpoints[epix], 0, sizeof(OtEDNEndPoint));
        ot_fifo32_create(&s->endpoints[epix].fifo, OT_CSRNG_PACKET_WORD_COUNT);
    }

    QSIMPLEQ_INIT(&s->ep_requests);
}

static void ot_edn_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = &ot_edn_reset;
    device_class_set_props(dc, ot_edn_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_edn_info = {
    .name = TYPE_OT_EDN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtEDNState),
    .instance_init = &ot_edn_init,
    .class_init = &ot_edn_class_init,
};

static void ot_edn_register_types(void)
{
    type_register_static(&ot_edn_info);
}

type_init(ot_edn_register_types)
