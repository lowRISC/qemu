/*
 * QEMU OpenTitan Cryptographically Secure Random Number Generator
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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
 * Note: ** It is by NO MEANS cryptographically secure! **
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_csrng.h"
#include "hw/opentitan/ot_entropy_src.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_otp_if.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "tomcrypt.h"
#include "trace.h"


#define PARAM_NUM_IRQS   4u
#define PARAM_NUM_ALERTS 2u
#define N_APP_COUNT      (OT_CSRNG_HW_APP_MAX + 1u)

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_CS_CMD_REQ_DONE, 0u, 1u)
    SHARED_FIELD(INTR_CS_ENTROPY_REQ, 1u, 1u)
    SHARED_FIELD(INTR_CS_HW_INST_EXC, 2u, 1u)
    SHARED_FIELD(INTR_CS_FATAL_ERR, 3u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, RECOV_ALERT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_ALERT, 1u, 1u)
REG32(REGWEN, 0x10u)
    FIELD(REGWEN, EN, 0u, 1u)
REG32(CTRL, 0x14u)
    FIELD(CTRL, ENABLE, 0u, 4u)
    FIELD(CTRL, SW_APP_ENABLE, 4u, 4u)
    FIELD(CTRL, READ_INT_STATE, 8u, 4u)
    FIELD(CTRL, FIPS_FORCE_ENABLE, 12u, 4u)
REG32(CMD_REQ, 0x18u)
REG32(RESEED_INTERVAL, 0x1cu)
REG32(RESEED_COUNTER_0, 0x20u)
REG32(RESEED_COUNTER_1, 0x24u)
REG32(RESEED_COUNTER_2, 0x28u)
REG32(SW_CMD_STS, 0x2cu)
    FIELD(SW_CMD_STS, CMD_RDY, 1u, 1u)
    FIELD(SW_CMD_STS, CMD_ACK, 2u, 1u)
    FIELD(SW_CMD_STS, CMD_STS, 3u, 3u)
REG32(GENBITS_VLD, 0x30u)
    FIELD(GENBITS_VLD, GENBITS_VLD, 0u, 1u)
    FIELD(GENBITS_VLD, GENBITS_FIPS, 1u, 1u)
REG32(GENBITS, 0x34u)
REG32(INT_STATE_READ_ENABLE, 0x38u)
    FIELD(INT_STATE_READ_ENABLE, VAL, 0u, N_APP_COUNT)
REG32(INT_STATE_READ_ENABLE_REGWEN, 0x3cu)
    FIELD(INT_STATE_READ_ENABLE_REGWEN, EN, 0u, 1u)
REG32(INT_STATE_NUM, 0x40u)
    FIELD(INT_STATE_NUM, VAL, 0u, 4u)
REG32(INT_STATE_VAL, 0x44u)
REG32(FIPS_FORCE, 0x48u)
    FIELD(FIPS_FORCE, VAL, 0u, N_APP_COUNT)
REG32(HW_EXC_STS, 0x4cu)
    FIELD(HW_EXC_STS, VAL, 0u, 16u)
REG32(RECOV_ALERT_STS, 0x50u)
    FIELD(RECOV_ALERT_STS, ENABLE_FIELD_ALERT, 0u, 1u)
    FIELD(RECOV_ALERT_STS, SW_APP_ENABLE_FIELD_ALERT, 1u, 1u)
    FIELD(RECOV_ALERT_STS, READ_INT_STATE_FIELD_ALERT, 2u, 1u)
    FIELD(RECOV_ALERT_STS, FIPS_FORCE_ENABLE_FIELD_ALERT, 3u, 1u)
    FIELD(RECOV_ALERT_STS, ACMD_FLAG0_FIELD_ALERT, 4u, 1u)
    FIELD(RECOV_ALERT_STS, CS_BUS_CMP_ALERT, 12u, 1u)
    FIELD(RECOV_ALERT_STS, CMD_STAGE_INVALID_ACMD_ALERT, 13u, 1u)
    FIELD(RECOV_ALERT_STS, CMD_STAGE_INVALID_CMD_SEQ_ALERT, 14u, 1u)
    FIELD(RECOV_ALERT_STS, CMD_STAGE_INVALID_RESEED_CNT_ALERT, 15u, 1u)
REG32(ERR_CODE, 0x54u)
    FIELD(ERR_CODE, SFIFO_CMD_ERR, 0u, 1u)
    FIELD(ERR_CODE, SFIFO_GENBITS_ERR, 1u, 1u)
    FIELD(ERR_CODE, SFIFO_CMDREQ_ERR, 2u, 1u)
    FIELD(ERR_CODE, SFIFO_RCSTAGE_ERR, 3u, 1u)
    FIELD(ERR_CODE, SFIFO_KEYVRC_ERR, 4u, 1u)
    FIELD(ERR_CODE, SFIFO_UPDREQ_ERR, 5u, 1u)
    FIELD(ERR_CODE, SFIFO_BENCREQ_ERR, 6u, 1u)
    FIELD(ERR_CODE, SFIFO_BENCACK_ERR, 7u, 1u)
    FIELD(ERR_CODE, SFIFO_PDATA_ERR, 8u, 1u)
    FIELD(ERR_CODE, SFIFO_FINAL_ERR, 9u, 1u)
    FIELD(ERR_CODE, SFIFO_GBENCACK_ERR, 10u, 1u)
    FIELD(ERR_CODE, SFIFO_GRCSTAGE_ERR, 11u, 1u)
    FIELD(ERR_CODE, SFIFO_GGENREQ_ERR, 12u, 1u)
    FIELD(ERR_CODE, SFIFO_GADSTAGE_ERR, 13u, 1u)
    FIELD(ERR_CODE, SFIFO_GGENBITS_ERR, 14u, 1u)
    FIELD(ERR_CODE, SFIFO_BLKENC_ERR, 15u, 1u)
    FIELD(ERR_CODE, CMD_STAGE_SM_ERR, 20u, 1u)
    FIELD(ERR_CODE, MAIN_SM_ERR, 21u, 1u)
    FIELD(ERR_CODE, DRBG_GEN_SM_ERR, 22u, 1u)
    FIELD(ERR_CODE, DRBG_UPDBE_SM_ERR, 23u, 1u)
    FIELD(ERR_CODE, DRBG_UPDOB_SM_ERR, 24u, 1u)
    FIELD(ERR_CODE, AES_CIPHER_SM_ERR, 25u, 1u)
    FIELD(ERR_CODE, CMD_GEN_CNT_ERR, 26u, 1u)
    FIELD(ERR_CODE, FIFO_WRITE_ERR, 28u, 1u)
    FIELD(ERR_CODE, FIFO_READ_ERR, 29u, 1u)
    FIELD(ERR_CODE, FIFO_STATE_ERR, 30u, 1u)
REG32(ERR_CODE_TEST, 0x58u)
    FIELD(ERR_CODE_TEST, VAL, 0u, 5u)
REG32(MAIN_SM_STATE, 0x5cu)
    FIELD(MAIN_SM_STATE, VAL, 0u, 8u)
/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_MAIN_SM_STATE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define INTR_MASK \
    (INTR_CS_CMD_REQ_DONE_MASK | INTR_CS_ENTROPY_REQ_MASK | \
     INTR_CS_HW_INST_EXC_MASK | INTR_CS_FATAL_ERR_MASK)
#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_ALERT_MASK | R_ALERT_TEST_FATAL_ALERT_MASK)
#define CTRL_MASK \
    (R_CTRL_ENABLE_MASK | R_CTRL_SW_APP_ENABLE_MASK | \
     R_CTRL_READ_INT_STATE_MASK | R_CTRL_FIPS_FORCE_ENABLE_MASK)
#define GENBITS_VLD_MASK \
    (R_GENBITS_VLD_GENBITS_VLD_MASK | R_GENBITS_VLD_GENBITS_FIPS_MASK)
#define RECOV_ALERT_STS_MASK \
    (R_RECOV_ALERT_STS_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_SW_APP_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_READ_INT_STATE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_ACMD_FLAG0_FIELD_ALERT_MASK | \
     R_RECOV_ALERT_STS_CS_BUS_CMP_ALERT_MASK | \
     R_RECOV_ALERT_STS_CMD_STAGE_INVALID_ACMD_ALERT_MASK | \
     R_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_MASK | \
     R_RECOV_ALERT_STS_CMD_STAGE_INVALID_RESEED_CNT_ALERT_MASK)
#define ERR_CODE_MASK 0x77e0ffffu

#define OT_CSRNG_AES_KEY_SIZE   32u /* 256 bits */
#define OT_CSRNG_AES_BLOCK_SIZE 16u /* 128 bits */

#define OT_CSRNG_AES_BLOCK_WORD  (OT_CSRNG_AES_BLOCK_SIZE / sizeof(uint32_t))
#define OT_CSRNG_AES_BLOCK_DWORD (OT_CSRNG_AES_BLOCK_SIZE / sizeof(uint64_t))

/*
 * Should be limited to OT_CSRNG_CMD_WORD_MAX, but the HW does not enforce it
 * so be sure to accept CLEN up to 15, not 12. CLEN being coded on 4 bits, and
 * command using 1 slot, the actual CLEN is limited to 15. HW ignores the
 * trailing additional data words ([12..15])
 */
#define CMD_FIFO_CAPACITY (R_OT_CSNRG_CMD_CLEN_MASK + 1u)

#define SW_INSTANCE_ID OT_CSRNG_HW_APP_MAX

#define ALERT_STATUS_BIT(_x_) R_RECOV_ALERT_STS_##_x_##_FIELD_ALERT_MASK

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    /* clang-format off */
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(REGWEN),
    REG_NAME_ENTRY(CTRL),
    REG_NAME_ENTRY(CMD_REQ),
    REG_NAME_ENTRY(RESEED_INTERVAL),
    REG_NAME_ENTRY(RESEED_COUNTER_0),
    REG_NAME_ENTRY(RESEED_COUNTER_1),
    REG_NAME_ENTRY(RESEED_COUNTER_2),
    REG_NAME_ENTRY(SW_CMD_STS),
    REG_NAME_ENTRY(GENBITS_VLD),
    REG_NAME_ENTRY(GENBITS),
    REG_NAME_ENTRY(INT_STATE_READ_ENABLE),
    REG_NAME_ENTRY(INT_STATE_READ_ENABLE_REGWEN),
    REG_NAME_ENTRY(INT_STATE_NUM),
    REG_NAME_ENTRY(INT_STATE_VAL),
    REG_NAME_ENTRY(FIPS_FORCE),
    REG_NAME_ENTRY(HW_EXC_STS),
    REG_NAME_ENTRY(RECOV_ALERT_STS),
    REG_NAME_ENTRY(ERR_CODE),
    REG_NAME_ENTRY(ERR_CODE_TEST),
    REG_NAME_ENTRY(MAIN_SM_STATE),
    /* clang-format on */
};
#undef REG_NAME_ENTRY

#define CMD_NAME_ENTRY(_cmd_) [OT_CSRNG_CMD_##_cmd_] = stringify(_cmd_)
static const char *CMD_NAMES[] = {
    CMD_NAME_ENTRY(NONE),   CMD_NAME_ENTRY(INSTANTIATE),
    CMD_NAME_ENTRY(RESEED), CMD_NAME_ENTRY(GENERATE),
    CMD_NAME_ENTRY(UPDATE), CMD_NAME_ENTRY(UNINSTANTIATE),
};
#undef CMD_NAME_ENTRY
#define CMD_NAME(_cmd_) \
    (((size_t)(_cmd_)) < ARRAY_SIZE(CMD_NAMES) ? CMD_NAMES[(_cmd_)] : "?")

static_assert(OT_CSRNG_PACKET_WORD_COUNT <= OT_ENTROPY_SRC_WORD_COUNT,
              "CSRNG packet cannot be larger than entropy_src packet");
static_assert((OT_ENTROPY_SRC_WORD_COUNT % OT_CSRNG_PACKET_WORD_COUNT) == 0,
              "CSRNG packet should be a multiple of entropy_src packet");
static_assert(OT_CSRNG_AES_BLOCK_SIZE + OT_CSRNG_AES_KEY_SIZE ==
                  OT_CSRNG_SEED_BYTE_COUNT,
              "Invalid seed size");

#define xtrace_ot_csrng_error(_msg_) \
    trace_ot_csrng_error(__func__, __LINE__, _msg_)
#define xtrace_ot_csrng_info(_msg_, _val_) \
    trace_ot_csrng_info(__func__, __LINE__, _msg_, _val_)
#define xtrace_ot_csrng_show_buffer(_id_, _msg_, _buf_, _len_) \
    ot_csrng_show_buffer(__func__, __LINE__, _id_, _msg_, _buf_, _len_)

#define ENTROPY_SRC_INITIAL_REQUEST_COUNT 5u

enum {
    ALERT_RECOVERABLE,
    ALERT_FATAL,
    ALERT_COUNT,
};

static_assert(ALERT_COUNT == PARAM_NUM_ALERTS, "Invalid alert count");

/*
 * Command execution result.
 *
 * Zero value signals the success and immediate completion of a command
 * Positive values signal a deferred completion of a command
 * Negative values signal an error, encoding a OtCSRNGCmdStatus value
 */
typedef enum {
    /* entropy stack is stalled */
    CSRNG_CMD_STALLED = -CSRNG_STATUS_COUNT,
    /* command errors, not recoverable */
    CSRNG_CMD_RESEED_CNT_EXCEEDED = -CSRNG_STATUS_RESEED_CNT_EXCEEDED,
    CSRNG_CMD_INVALID_CMD_SEQ = -CSRNG_STATUS_INVALID_CMD_SEQ,
    CSRNG_CMD_INVALID_GEN_CMD = -CSRNG_STATUS_INVALID_GEN_CMD,
    CSRNG_CMD_INVALID_ACMD = -CSRNG_STATUS_INVALID_ACMD,
    /* command completed ok */
    CSRNG_CMD_OK = 0,
    /* command cannot be executed for now */
    CSRNG_CMD_RETRY = 1,
    /* command completion deferred */
    CSRNG_CMD_DEFERRED = 2,
} OtCSRNDCmdResult;

typedef enum {
    CSRNG_IDLE, /* idle */
    CSRNG_PARSE_CMD, /* parse the cmd */
    CSRNG_INSTANT_PREP, /* instantiate prep */
    CSRNG_INSTANT_REQ, /* instantiate request (takes adata or entropy) */
    CSRNG_RESEED_PREP, /* reseed prep */
    CSRNG_RESEED_REQ, /* reseed request (takes adata & entropy & Key,V,RC)*/
    CSRNG_GENERATE_PREP, /* generate request (takes adata? & Key,V,RC) */
    CSRNG_GENERATE_REQ, /* generate request (takes adata? & Key,V,RC) */
    CSRNG_UPDATE_PREP, /* update prep */
    CSRNG_UPDATE_REQ, /* update request (takes adata & Key,V,RC) */
    CSRNG_UNINSTANT_PREP, /* uninstantiate prep */
    CSRNG_UNINSTANT_REQ, /* uninstantiate request */
    CSRNG_CLR_A_DATA, /* clear out the additional data packer fifo */
    CSRNG_CMD_COMP_WAIT, /* wait for command to complete */
    CSRNG_ERROR, /* error state, results in fatal alert */
} OtCSRNGFsmState;

typedef struct {
    symmetric_ECB ecb; /* AES ECB context for tomcrypt */
    uint8_t v_counter[OT_CSRNG_AES_BLOCK_SIZE]; /* V a.k.a. the counter */
    uint8_t key[OT_CSRNG_AES_KEY_SIZE];
    uint32_t material[OT_CSRNG_SEED_WORD_COUNT];
    unsigned material_len; /* in word count */
    unsigned reseed_counter; /* generate command count since last reseed */
    unsigned rem_packet_count; /* remaining packets to generate */
    bool instantiated;
    bool seeded; /* ready to generate randomness */
    bool no_fips;
    bool force_fips;
} OtCSRNGDrng;

typedef struct OtCSRNGInstance {
    OtFifo32 cmd_fifo;
    union {
        struct {
            OtFifo32 bits_fifo;
            bool fips;
        } sw;
        struct {
            OtCsrngGenbitFiller filler;
            void *opaque;
            QEMUBH *filler_bh;
            qemu_irq req_sts;
            bool genbits_ready;
        } hw;
    };
    OtCSRNGDrng drng;
    bool defer_completion;
    QSIMPLEQ_ENTRY(OtCSRNGInstance) cmd_request;
    OtCSRNGState *parent;
} OtCSRNGInstance;

typedef QSIMPLEQ_HEAD(OtCSRNGQueue, OtCSRNGInstance) OtCSRNGQueue;

struct OtCSRNGState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    QEMUBH *cmd_scheduler;
    QEMUTimer *entropy_scheduler;

    uint32_t *regs;
    bool enabled;
    bool sw_app_granted;
    bool read_int_granted;
    uint32_t scheduled_cmd;
    unsigned entropy_delay;
    unsigned es_retry_count;
    unsigned state_db_ix;
    int aes_cipher; /* AES handle for tomcrypt */
    OtCSRNGFsmState state;
    OtCSRNGInstance *instances;
    OtCSRNGQueue cmd_requests;

    OtEntropySrcState *entropy_src;
    DeviceState *otp_ctrl;
};

/* clang-format off */
static const uint8_t OtCSRNGFsmStateCode[] = {
    [CSRNG_IDLE]           = 0b01001110, // 0x4e: idle
    [CSRNG_PARSE_CMD]      = 0b10111011, // 0xbb: parse the cmd
    [CSRNG_INSTANT_PREP]   = 0b11000001, // 0xc1: instantiate prep
    [CSRNG_INSTANT_REQ]    = 0b01010100, // 0x54: instantiate request
    [CSRNG_RESEED_PREP]    = 0b11011101, // 0xdd: reseed prep
    [CSRNG_RESEED_REQ]     = 0b01011011, // 0x5b: reseed request
    [CSRNG_GENERATE_PREP]  = 0b11101111, // 0xef: generate prep
    [CSRNG_GENERATE_REQ]   = 0b00100100, // 0x24: generate request
    [CSRNG_UPDATE_PREP]    = 0b00110001, // 0x31: update prep
    [CSRNG_UPDATE_REQ]     = 0b00100100, // 0x24: update request
    [CSRNG_UNINSTANT_PREP] = 0b11110110, // 0xf6: uninstantiate prep
    [CSRNG_UNINSTANT_REQ]  = 0b01100011, // 0x63: uninstantiate request
    [CSRNG_CLR_A_DATA]     = 0b00000010, // 0x02: clear out the add. data fifo
    [CSRNG_CMD_COMP_WAIT]  = 0b10111100, // 0xbc: wait for command to complete
    [CSRNG_ERROR]          = 0b01111000  // 0x78: error state
};
/* clang-format on */

#define STATE_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
static const char *STATE_NAMES[] = {
    STATE_NAME_ENTRY(CSRNG_IDLE),
    STATE_NAME_ENTRY(CSRNG_PARSE_CMD),
    STATE_NAME_ENTRY(CSRNG_INSTANT_PREP),
    STATE_NAME_ENTRY(CSRNG_INSTANT_REQ),
    STATE_NAME_ENTRY(CSRNG_RESEED_PREP),
    STATE_NAME_ENTRY(CSRNG_RESEED_REQ),
    STATE_NAME_ENTRY(CSRNG_GENERATE_PREP),
    STATE_NAME_ENTRY(CSRNG_GENERATE_REQ),
    STATE_NAME_ENTRY(CSRNG_UPDATE_PREP),
    STATE_NAME_ENTRY(CSRNG_UPDATE_REQ),
    STATE_NAME_ENTRY(CSRNG_UNINSTANT_PREP),
    STATE_NAME_ENTRY(CSRNG_UNINSTANT_REQ),
    STATE_NAME_ENTRY(CSRNG_CLR_A_DATA),
    STATE_NAME_ENTRY(CSRNG_CMD_COMP_WAIT),
    STATE_NAME_ENTRY(CSRNG_ERROR),

};
#undef STATE_NAME_ENTRY
#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")
#define CHECK_MULTIBOOT(_s_, _r_, _b_) \
    ot_csrng_check_multibitboot((_s_), FIELD_EX32(s->regs[R_##_r_], _r_, _b_), \
                                ALERT_STATUS_BIT(_b_));
#define CHANGE_STATE(_s_, _st_) ot_csrng_change_state_line(_s_, _st_, __LINE__)

#define OT_CSRNG_HEXBUF_SIZE ((8u * 2u + 1u) * OT_CSRNG_SEED_WORD_COUNT + 4u)

static bool ot_csrng_check_multibitboot(OtCSRNGState *s, uint8_t mbbool,
                                        uint32_t alert_bit);
static void ot_csrng_command_schedule(OtCSRNGState *s, OtCSRNGInstance *inst);
static bool
ot_csrng_instance_is_command_ready(const OtCSRNGInstance *inst, bool fatal);
static void ot_csrng_complete_command(OtCSRNGInstance *inst,
                                      OtCSRNGCmdStatus sts);
static void ot_csrng_change_state_line(OtCSRNGState *s, OtCSRNGFsmState state,
                                       int line);
static unsigned ot_csrng_get_slot(const OtCSRNGInstance *inst);
static bool ot_csrng_drng_is_instantiated(const OtCSRNGInstance *inst);
static void ot_csrng_release_hw_app(OtCSRNGInstance *inst);
static void ot_csrng_update_irqs(OtCSRNGState *s);
static void ot_csrng_update_alerts(OtCSRNGState *s);

static OtCSRNDCmdResult ot_csrng_drng_reseed(OtCSRNGInstance *inst, bool flag0);

/* -------------------------------------------------------------------------- */
/* Client API */
/* -------------------------------------------------------------------------- */

static qemu_irq
ot_csrng_connect_hw_app(OtCSRNGState *s, unsigned app_id, qemu_irq req_sts,
                        OtCsrngGenbitFiller filler_fn, void *opaque)
{
    g_assert(app_id < OT_CSRNG_HW_APP_MAX);

    OtCSRNGInstance *inst = &s->instances[app_id];

    if (!filler_fn) {
        if (!inst->hw.filler) {
            xtrace_ot_csrng_info("HW app was not connected", app_id);
            return NULL;
        }

        trace_ot_csrng_disconnection(app_id, true);
        ot_csrng_release_hw_app(inst);

        return NULL;
    }

    g_assert(req_sts);

    /* if connection is invoked many times, there is no reason for changes */
    if (inst->hw.filler) {
        g_assert(inst->hw.filler == filler_fn);
    }
    if (inst->hw.req_sts) {
        g_assert(inst->hw.req_sts == req_sts);
    }

    trace_ot_csrng_connection(app_id, inst->hw.filler == NULL);

    inst->hw.filler = filler_fn;
    inst->hw.opaque = opaque;
    inst->hw.req_sts = req_sts;

    return qdev_get_gpio_in_named(DEVICE(s), TYPE_OT_CSRNG "-genbits_ready",
                                  (int)app_id);
}

static OtCSRNGCmdStatus
ot_csrng_push_command(OtCSRNGState *s, unsigned app_id, uint32_t word)
{
    g_assert(app_id < OT_CSRNG_HW_APP_MAX);

    if (s->state == CSRNG_ERROR) {
        return CSRNG_STATUS_INVALID_CMD_SEQ;
    }

    OtCSRNGInstance *inst = &s->instances[app_id];
    g_assert(inst->hw.filler);
    g_assert(inst->hw.req_sts);

    /* FIFO is emptied in #ot_csrng_complete_command */
    if (ot_fifo32_is_full(&inst->cmd_fifo)) {
        xtrace_ot_csrng_error("Command FIFO is full");
        return CSRNG_STATUS_INVALID_CMD_SEQ;
    }

    bool check_cmd = ot_fifo32_is_empty(&inst->cmd_fifo);

    ot_fifo32_push(&inst->cmd_fifo, word);
    uint32_t cmd = ot_fifo32_peek(&inst->cmd_fifo);

    uint32_t acmd = FIELD_EX32(cmd, OT_CSNRG_CMD, ACMD);
    uint32_t length = FIELD_EX32(cmd, OT_CSNRG_CMD, CLEN) + 1u;

    if (check_cmd) {
        /* NOLINTNEXTLINE */
        switch (acmd) {
        case OT_CSRNG_CMD_INSTANTIATE:
        case OT_CSRNG_CMD_RESEED:
            ot_csrng_check_multibitboot(s,
                                        (uint8_t)FIELD_EX32(cmd, OT_CSNRG_CMD,
                                                            FLAG0),
                                        ALERT_STATUS_BIT(ACMD_FLAG0));
            break;
        case OT_CSRNG_CMD_GENERATE ... OT_CSRNG_CMD_UNINSTANTIATE:
            break;
        default:
            xtrace_ot_csrng_error("Invalid command opcode");
            ot_fifo32_reset(&inst->cmd_fifo);
            return CSRNG_STATUS_INVALID_ACMD;
        }

        if (length > CMD_FIFO_CAPACITY) {
            xtrace_ot_csrng_error("Invalid command length (overflow)");
            /*
             * as CLEN width cannot encode more than CMD_FIFO_CAPACITY, this
             * error should never occur.
             */
            g_assert_not_reached();
        }
    }

    if (ot_fifo32_num_used(&inst->cmd_fifo) > length) {
        xtrace_ot_csrng_error("Invalid command length (too many)");
        /*
         * as the FIFO should have been handled and emptied in a previous call
         * to this function, this error should never occur.
         */
        g_assert_not_reached();
    }
    if (ot_fifo32_num_used(&inst->cmd_fifo) < length) {
        /* more payload is expected */
        return CSRNG_STATUS_SUCCESS;
    }

    if (acmd == OT_CSRNG_CMD_GENERATE) {
        uint32_t glen = FIELD_EX32(cmd, OT_CSNRG_CMD, GLEN);
        trace_ot_csrng_push_command(app_id, CMD_NAME(acmd), acmd, 'g', glen);

        const OtCSRNGDrng *drng = &inst->drng;
        if (drng->reseed_counter >= s->regs[R_RESEED_INTERVAL]) {
            s->regs[R_INTR_STATE] |= INTR_CS_HW_INST_EXC_MASK;
            s->regs[R_RECOV_ALERT_STS] |=
                R_RECOV_ALERT_STS_CMD_STAGE_INVALID_RESEED_CNT_ALERT_MASK;
            ot_fifo32_reset(&inst->cmd_fifo);
            ot_csrng_update_irqs(s);
            ot_csrng_update_alerts(s);
            return CSRNG_STATUS_RESEED_CNT_EXCEEDED;
        }
    } else {
        trace_ot_csrng_push_command(app_id, CMD_NAME(acmd), acmd, 'c', length);
    }

    if (acmd == OT_CSRNG_CMD_UNINSTANTIATE) {
        if (!ot_csrng_drng_is_instantiated(inst)) {
            /*
             * Very special hacky case:
             *
             * Uninstantiation is requested before instantiation has been
             * completed (instantiation command in still in the command queue).
             * Flush the command queue to discard the instantiation command,
             * which stays uncompleted on the client side, and resume (the
             * uninstantiation request should complete this new command).
             *
             * It might be useful to implement a "cancel outstanding request"
             * API so that the EDN can tell CSRNG its last command needs to be
             * cancelled before completion - and remove this workaround.
             */
            QSIMPLEQ_REMOVE(&s->cmd_requests, inst, OtCSRNGInstance,
                            cmd_request);

            if (QSIMPLEQ_EMPTY(&s->cmd_requests)) {
                qemu_bh_cancel(s->cmd_scheduler);
                timer_del(s->entropy_scheduler);
            }
        }
    }

    ot_csrng_command_schedule(s, inst);

    return CSRNG_STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* DRBG (Deterministic Random Bit Generator) */
/* -------------------------------------------------------------------------- */

static void ot_csrng_show_buffer(const char *func, int line, unsigned int appid,
                                 const char *msg, const void *buf,
                                 unsigned size)
{
    if (trace_event_get_state(TRACE_OT_CSRNG_SHOW_BUFFER) &&
        qemu_loglevel_mask(LOG_TRACE)) {
        static const char _hex[] = "0123456789ABCDEF";
        char hexstr[OT_CSRNG_HEXBUF_SIZE];
        unsigned len = MIN(size, OT_CSRNG_HEXBUF_SIZE / 2u - 4u);
        const uint8_t *pbuf = (const uint8_t *)buf;
        memset(hexstr, 0, sizeof(hexstr));
        unsigned hix = 0;
        for (unsigned ix = 0u; ix < len; ix++) {
            if (ix && !(ix & 0x3u)) {
                hexstr[hix++] = '-';
            }
            hexstr[hix++] = _hex[(pbuf[ix] >> 4u) & 0xfu];
            hexstr[hix++] = _hex[pbuf[ix] & 0xfu];
        }
        if (len < size) {
            hexstr[hix++] = '.';
            hexstr[hix++] = '.';
            hexstr[hix++] = '.';
        }

        trace_ot_csrng_show_buffer(func, line, appid, msg, hexstr);
    }
}

static void ot_csrng_drng_store_material(OtCSRNGInstance *inst,
                                         const uint32_t *buf, unsigned len)
{
    OtCSRNGDrng *drng = &inst->drng;

    /* convert OpenTitan order into natural order */
    for (unsigned ix = 0; ix < len; ix++) {
        drng->material[ix] = bswap32(buf[len - ix - 1]);
    }
    drng->material_len = len;
}

static void ot_csrng_drng_clear_material(OtCSRNGInstance *inst)
{
    OtCSRNGDrng *drng = &inst->drng;

    memset(drng->material, 0, sizeof(drng->material));
    drng->material_len = 0;
}

static unsigned ot_csrng_drng_remaining_count(const OtCSRNGInstance *inst)
{
    return inst->drng.rem_packet_count;
}

static bool ot_csrng_drng_is_instantiated(const OtCSRNGInstance *inst)
{
    return inst->drng.instantiated;
}

static void ot_csrng_drng_increment(OtCSRNGDrng *drng)
{
    unsigned pos = OT_CSRNG_AES_BLOCK_SIZE - 1u;
    while (pos) {
        if (++drng->v_counter[pos] != 0) {
            break;
        }
        pos -= 1u;
    }
}

static OtCSRNDCmdResult
ot_csrng_drng_instantiate(OtCSRNGInstance *inst, bool flag0)
{
    OtCSRNGDrng *drng = &inst->drng;
    if (drng->instantiated) {
        return CSRNG_CMD_INVALID_CMD_SEQ;
    }

    memset(drng->v_counter, 0, sizeof(drng->v_counter));
    drng->no_fips = false;

    uint8_t key[OT_CSRNG_AES_KEY_SIZE];
    memset(key, 0, sizeof(key));

    int res;
    res = ecb_start(inst->parent->aes_cipher, key, (int)sizeof(key), 0,
                    &drng->ecb);
    g_assert(res == CRYPT_OK);

    memcpy(drng->key, key, OT_CSRNG_AES_KEY_SIZE);
    drng->instantiated = true;

    res = ot_csrng_drng_reseed(inst, flag0);
    if (res) {
        drng->instantiated = false;
        return res;
    }

    if (inst->hw.genbits_ready) {
        /*
         * a HW client app of this instance has already requested randomness
         * through #ot_csrng_hwapp_ready_irq. This request had been deferred
         * till the instanciation stage has completed, it is now safe to
         * schedule the actual generation
         */
        qemu_bh_schedule(inst->hw.filler_bh);
    }

    return res;
}

static void ot_csrng_drng_uninstantiate(OtCSRNGInstance *inst)
{
    OtCSRNGDrng *drng = &inst->drng;

    if (drng->instantiated) {
        ecb_done(&drng->ecb);
    }

    drng->instantiated = false;
    drng->seeded = false;
    drng->no_fips = false;
    drng->rem_packet_count = 0;

    /* only to help debugging */
    memset(drng->v_counter, 0, sizeof(drng->v_counter));
}

static void ot_csrng_drng_update(OtCSRNGInstance *inst)
{
    OtCSRNGDrng *drng = &inst->drng;

    unsigned appid = ot_csrng_get_slot(inst);
    xtrace_ot_csrng_show_buffer(appid, "seed", drng->material,
                                drng->material_len * sizeof(uint32_t));
    xtrace_ot_csrng_show_buffer(appid, "c-V", drng->v_counter,
                                OT_CSRNG_AES_BLOCK_SIZE);

    uint32_t tmp[OT_CSRNG_SEED_WORD_COUNT];
    int res;
    memset(tmp, 0, sizeof(tmp));
    uint8_t *ptmp = (uint8_t *)tmp;
    for (unsigned ix = 0; ix < OT_CSRNG_SEED_BYTE_COUNT;
         ix += OT_CSRNG_AES_BLOCK_SIZE) {
        ot_csrng_drng_increment(drng);

        res = ecb_encrypt(drng->v_counter, ptmp, OT_CSRNG_AES_BLOCK_SIZE,
                          &drng->ecb);
        g_assert(res == CRYPT_OK);
        ptmp += OT_CSRNG_AES_BLOCK_SIZE;
    }

    for (unsigned ix = 0; ix < drng->material_len; ix++) {
        tmp[ix] ^= drng->material[ix];
    }

    ot_csrng_drng_clear_material(inst);

    res = ecb_done(&drng->ecb);
    g_assert(res == CRYPT_OK);

    ptmp = (uint8_t *)tmp;
    res = ecb_start(inst->parent->aes_cipher, ptmp, (int)OT_CSRNG_AES_KEY_SIZE,
                    0, &drng->ecb);
    g_assert(res == CRYPT_OK);

    memcpy(drng->key, ptmp, OT_CSRNG_AES_KEY_SIZE);
    memcpy(drng->v_counter, &ptmp[OT_CSRNG_AES_KEY_SIZE],
           OT_CSRNG_AES_BLOCK_SIZE);

    xtrace_ot_csrng_show_buffer(appid, "n-key", drng->key,
                                OT_CSRNG_AES_KEY_SIZE);
    xtrace_ot_csrng_show_buffer(appid, "n-V", drng->v_counter,
                                OT_CSRNG_AES_BLOCK_SIZE);
}

static OtCSRNDCmdResult ot_csrng_drng_reseed(OtCSRNGInstance *inst, bool flag0)
{
    OtCSRNGState *s = inst->parent;
    OtCSRNGDrng *drng = &inst->drng;
    g_assert(drng->instantiated);

    unsigned slot = ot_csrng_get_slot(inst);
    drng->seeded = false;

    if (!flag0) {
        uint64_t buffer[OT_ENTROPY_SRC_DWORD_COUNT];
        memset(buffer, 0, sizeof(buffer));
        unsigned len = drng->material_len * sizeof(uint32_t);
        memcpy(buffer, drng->material, MIN(len, sizeof(buffer)));


        uint64_t entropy[OT_ENTROPY_SRC_DWORD_COUNT];
        int res;
        bool fips;
        trace_ot_csrng_request_entropy(slot);
        OtEntropySrcState *ess = inst->parent->entropy_src;
        OtEntropySrcClass *esc = OT_ENTROPY_SRC_GET_CLASS(ess);
        res = esc->get_entropy(ess, entropy, &fips);

        if (res < 0) {
            s->entropy_delay = 0;
            trace_ot_csrng_entropy_rejected(slot, "error", res);
            return CSRNG_CMD_STALLED;
        }

        if (res > 0) {
            s->entropy_delay = (res > 1) ? (unsigned)res : 0;
            trace_ot_csrng_entropy_rejected(slot, "not ready", res);
            return CSRNG_CMD_RETRY;
        }

        /* always perform XOR which is a no-op if material_len is zero */
        for (unsigned ix = 0; ix < OT_ENTROPY_SRC_DWORD_COUNT; ix++) {
            buffer[ix] ^= entropy[ix];
        }
        memcpy(drng->material, buffer, sizeof(entropy));
        drng->material_len = sizeof(entropy) / (sizeof(uint32_t));
        ot_csrng_drng_update(inst);
        drng->no_fips |= !fips;
    } else {
        ot_csrng_drng_update(inst);
        drng->no_fips = true;
    }

    drng->reseed_counter = 0u;
    drng->seeded = true;
    drng->force_fips = (bool)((s->regs[R_FIPS_FORCE] >> slot) & 0x1u);

    return CSRNG_CMD_OK;
}

static void ot_csrng_drng_generate(OtCSRNGInstance *inst, uint32_t *out,
                                   bool *fips)
{
    OtCSRNGDrng *drng = &inst->drng;

    ot_csrng_drng_increment(drng);
    drng->rem_packet_count -= 1u;

    int res;

    res = ecb_encrypt(drng->v_counter, (uint8_t *)out, OT_CSRNG_AES_BLOCK_SIZE,
                      &drng->ecb);
    g_assert(res == CRYPT_OK);

    xtrace_ot_csrng_show_buffer(ot_csrng_get_slot(inst), "out", out,
                                OT_CSRNG_AES_BLOCK_SIZE);

    *fips = (!drng->no_fips) || drng->force_fips;

    if (!ot_csrng_drng_remaining_count(inst)) {
        ot_csrng_drng_update(inst);
        /* last packet generation for the current command */
        drng->reseed_counter += 1u;
    }
}

/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static unsigned ot_csrng_get_slot(const OtCSRNGInstance *inst)
{
    unsigned slot = (unsigned)(uintptr_t)(inst - &inst->parent->instances[0]);
    g_assert(slot <= SW_INSTANCE_ID);
    return slot;
}

static void ot_csrng_update_irqs(OtCSRNGState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_csrng_irqs(s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE], level);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

static void ot_csrng_update_alerts(OtCSRNGState *s)
{
    uint32_t level = s->regs[R_ALERT_TEST];
    s->regs[R_ALERT_TEST] = 0u;

    if (s->regs[R_RECOV_ALERT_STS]) {
        level |= 1u << ALERT_RECOVERABLE;
    }

    if (s->state == CSRNG_ERROR) {
        level |= 1u << ALERT_FATAL;
    }

    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
        ibex_irq_set(&s->alerts[ix], 0);
    }
}

static void ot_csrng_change_state_line(OtCSRNGState *s, OtCSRNGFsmState state,
                                       int line)
{
    trace_ot_csrng_change_state(line, STATE_NAME(s->state), s->state,
                                STATE_NAME(state), state);

    s->state = state;

    if (s->state == CSRNG_ERROR) {
        s->regs[R_INTR_STATE] |= INTR_CS_FATAL_ERR_MASK;
        ot_csrng_update_irqs(s);
        ot_csrng_update_alerts(s);
    }
}

static bool
ot_csrng_check_multibitboot(OtCSRNGState *s, uint8_t mbbool, uint32_t alert_bit)
{
    switch (mbbool) {
    case OT_MULTIBITBOOL4_TRUE:
        return true;
    case OT_MULTIBITBOOL4_FALSE:
        return false;
    default:
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: Invalid multiboot4: 0x%01x\n", __func__,
                  (unsigned)mbbool);

    s->regs[R_RECOV_ALERT_STS] |= 1u << alert_bit;
    ot_csrng_update_alerts(s);

    /* for CSRNG, default to false for invalid multibit boolean */
    return false;
}

static bool ot_csrng_is_ctrl_enabled(OtCSRNGState *s)
{
    return FIELD_EX32(s->regs[R_CTRL], CTRL, ENABLE) == OT_MULTIBITBOOL4_TRUE;
}

static bool ot_csrng_is_ctrl_disabled(OtCSRNGState *s)
{
    return FIELD_EX32(s->regs[R_CTRL], CTRL, ENABLE) == OT_MULTIBITBOOL4_FALSE;
}

static bool ot_csrng_is_sw_app_enabled(OtCSRNGState *s)
{
    return s->sw_app_granted &&
           FIELD_EX32(s->regs[R_CTRL], CTRL, SW_APP_ENABLE) ==
               OT_MULTIBITBOOL4_TRUE;
}

static bool ot_csrng_is_in_queue(OtCSRNGInstance *inst)
{
    return QSIMPLEQ_NEXT(inst, cmd_request) ||
           QSIMPLEQ_LAST(&inst->parent->cmd_requests, OtCSRNGInstance,
                         cmd_request) == inst;
}

static void ot_csrng_release_hw_app(OtCSRNGInstance *inst)
{
    /* remove the command from the queue since it is handled right here */
    OtCSRNGState *s = inst->parent;
    if (ot_csrng_is_in_queue(inst)) {
        QSIMPLEQ_REMOVE(&s->cmd_requests, inst, OtCSRNGInstance, cmd_request);
    }

    if (QSIMPLEQ_EMPTY(&s->cmd_requests)) {
        qemu_bh_cancel(s->cmd_scheduler);
        timer_del(s->entropy_scheduler);
    }

    unsigned slot = ot_csrng_get_slot(inst);
    trace_ot_csrng_uninstantiate(slot, false);
    ot_csrng_drng_uninstantiate(inst);

    ot_fifo32_reset(&inst->cmd_fifo);
    inst->defer_completion = false;

    inst->hw.filler = NULL;
    inst->hw.opaque = NULL;
    inst->hw.req_sts = NULL;
}

static void ot_csrng_handle_enable(OtCSRNGState *s)
{
    if (ot_csrng_is_ctrl_enabled(s)) {
        xtrace_ot_csrng_info("enabling CSRNG", 0);
        s->enabled = true;
        s->regs[R_SW_CMD_STS] |= R_SW_CMD_STS_CMD_RDY_MASK;
        s->es_retry_count = ENTROPY_SRC_INITIAL_REQUEST_COUNT;
    }

    if (ot_csrng_is_ctrl_disabled(s)) {
        xtrace_ot_csrng_info("disabling CSRNG", 0);
        /* skip SW instance */
        for (unsigned ix = 0u; ix < OT_CSRNG_HW_APP_MAX; ix++) {
            OtCSRNGInstance *inst = &s->instances[ix];
            if (inst->hw.filler) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: Forcing CSRNG disablement while EDN #%u "
                              "still active\n",
                              __func__, ot_csrng_get_slot(inst));

                trace_ot_csrng_disconnection(ix, false);
                ot_csrng_release_hw_app(inst);
            }
        }
        s->enabled = false;
        s->regs[R_SW_CMD_STS] &= ~R_SW_CMD_STS_CMD_RDY_MASK;
        s->es_retry_count = 0;

        /* cancel any outstanding asynchronous request */
        qemu_bh_cancel(s->cmd_scheduler);
        timer_del(s->entropy_scheduler);

        /* discard any on-going command request */
        OtCSRNGInstance *inst, *next;
        QSIMPLEQ_FOREACH_SAFE(inst, &s->cmd_requests, cmd_request, next) {
            xtrace_ot_csrng_info("discarding request for APP",
                                 ot_csrng_get_slot(inst));
            if (s->state != CSRNG_ERROR) {
                /*
                 * any ongoing command should be reported as failed, except if
                 * is an unstantiate command, in which case it is immediately
                 * completed
                 */
                OtCSRNGCmdStatus sts;
                if (!ot_fifo32_is_empty(&inst->cmd_fifo)) {
                    uint32_t cmd = ot_fifo32_peek(&inst->cmd_fifo);
                    uint32_t acmd = FIELD_EX32(cmd, OT_CSNRG_CMD, ACMD);
                    sts = acmd == OT_CSRNG_CMD_UNINSTANTIATE ?
                              CSRNG_STATUS_SUCCESS :
                              CSRNG_STATUS_INVALID_CMD_SEQ;
                } else {
                    sts = CSRNG_STATUS_INVALID_CMD_SEQ;
                }
                qemu_set_irq(inst->hw.req_sts, (int)sts);
            }

            QSIMPLEQ_REMOVE_HEAD(&s->cmd_requests, cmd_request);
        }

        /* reset all instances */
        for (unsigned ix = 0u; ix < N_APP_COUNT; ix++) {
            inst = &s->instances[ix];
            ot_csrng_drng_uninstantiate(inst);
            ot_fifo32_reset(&inst->cmd_fifo);
            if (ix == SW_INSTANCE_ID) {
                ot_fifo32_reset(&inst->sw.bits_fifo);
                inst->sw.fips = false;
            } else {
                inst->hw.genbits_ready = false;
            }
        }

        CHANGE_STATE(s, CSRNG_IDLE);

        xtrace_ot_csrng_info("CSRNG disabled", 0);
    }
}

static void ot_csrng_complete_sw_command(OtCSRNGInstance *inst, bool res)
{
    OtCSRNGState *s = inst->parent;

    if (res == CSRNG_CMD_OK) {
        s->regs[R_SW_CMD_STS] &= R_SW_CMD_STS_CMD_STS_MASK;
    } else {
        s->regs[R_SW_CMD_STS] |= R_SW_CMD_STS_CMD_STS_MASK;
    }
    s->regs[R_SW_CMD_STS] |= R_SW_CMD_STS_CMD_RDY_MASK;
    s->regs[R_INTR_STATE] |= INTR_CS_CMD_REQ_DONE_MASK;

    ot_csrng_update_irqs(s);
}

static void
ot_csrng_complete_hw_command(OtCSRNGInstance *inst, OtCSRNGCmdStatus sts)
{
    qemu_set_irq(inst->hw.req_sts, (int)sts);
}

static void ot_csrng_complete_command(OtCSRNGInstance *inst,
                                      OtCSRNGCmdStatus sts)
{
    uint32_t num;
    const uint32_t *buffer =
        ot_fifo32_peek_buf(&inst->cmd_fifo, ot_fifo32_num_used(&inst->cmd_fifo),
                           &num);
    uint32_t acmd = FIELD_EX32(buffer[0], OT_CSNRG_CMD, ACMD);

    OtCSRNGState *s = inst->parent;

    if (num > 1u) {
        CHANGE_STATE(s, CSRNG_CLR_A_DATA);
        buffer += 1u;
        memset((uint32_t *)buffer, 0, sizeof(uint32_t) * (num - 1u));
    }

    ot_fifo32_reset(&inst->cmd_fifo);
    inst->defer_completion = false;

    unsigned slot = ot_csrng_get_slot(inst);

    trace_ot_csrng_show_command("complete", slot, CMD_NAME(acmd), acmd);

    if (slot == SW_INSTANCE_ID) {
        trace_ot_csrng_complete_command(slot, "sw", CMD_NAME(acmd), acmd, sts);
        ot_csrng_complete_sw_command(inst, sts);
    } else {
        trace_ot_csrng_complete_command(slot, "hw", CMD_NAME(acmd), acmd, sts);
        ot_csrng_complete_hw_command(inst, sts);
    }

    CHANGE_STATE(s, CSRNG_IDLE);
}

static OtCSRNDCmdResult
ot_csrng_handle_instantiate(OtCSRNGState *s, unsigned slot)
{
    OtCSRNGInstance *inst = &s->instances[slot];

    trace_ot_csrng_instantiate(slot);

    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);
    uint32_t clen = FIELD_EX32(command, OT_CSNRG_CMD, CLEN);
    bool flag0 =
        FIELD_EX32(command, OT_CSNRG_CMD, FLAG0) == OT_MULTIBITBOOL4_TRUE;

    uint32_t num;
    const uint32_t *buffer =
        ot_fifo32_peek_buf(&inst->cmd_fifo, ot_fifo32_num_used(&inst->cmd_fifo),
                           &num);
    g_assert(num - 1u == clen);
    buffer += 1u;

    if (clen) {
        /* ignore trailing additional words, as HW does */
        clen = MIN(clen, OT_CSRNG_CMD_WORD_MAX);
        xtrace_ot_csrng_show_buffer(ot_csrng_get_slot(inst), "mat", buffer,
                                    clen * sizeof(uint32_t));
    }

    ot_csrng_drng_store_material(inst, buffer, clen);

    int res;

    res = ot_csrng_drng_instantiate(inst, flag0);
    if ((res == CSRNG_CMD_OK) && !flag0) {
        /* if flag0 is set, entropy source is not used for reseeding */
        s->regs[R_INTR_STATE] |= INTR_CS_ENTROPY_REQ_MASK;
        ot_csrng_update_irqs(s);
    }

    return res;
}

static OtCSRNDCmdResult
ot_csrng_handle_uninstantiate(OtCSRNGState *s, unsigned slot)
{
    OtCSRNGInstance *inst = &s->instances[slot];

    trace_ot_csrng_uninstantiate(slot, true);

    ot_csrng_drng_uninstantiate(inst);

    return CSRNG_CMD_OK;
}

static OtCSRNDCmdResult ot_csrng_handle_generate(OtCSRNGState *s, unsigned slot)
{
    OtCSRNGInstance *inst = &s->instances[slot];

    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);
    uint32_t packet_count = FIELD_EX32(command, OT_CSNRG_CMD, GLEN);

    if (!packet_count) {
        xtrace_ot_csrng_error("generation for no packet");
        CHANGE_STATE(s, CSRNG_ERROR);
        ot_csrng_update_alerts(s);
        return CSRNG_CMD_INVALID_GEN_CMD;
    }

    uint32_t clen = FIELD_EX32(command, OT_CSNRG_CMD, CLEN);
    if (clen) {
        uint32_t num;
        const uint32_t *buffer =
            ot_fifo32_peek_buf(&inst->cmd_fifo,
                               ot_fifo32_num_used(&inst->cmd_fifo), &num);
        g_assert(num - 1u == clen);
        buffer += 1u;
        /* ignore trailing additional words, as HW does */
        clen = MIN(clen, OT_CSRNG_CMD_WORD_MAX);

        xtrace_ot_csrng_show_buffer(ot_csrng_get_slot(inst), "mat", buffer,
                                    clen * sizeof(uint32_t));
        ot_csrng_drng_store_material(inst, buffer, clen);
    }

    trace_ot_csrng_generate(ot_csrng_get_slot(inst), packet_count);

    OtCSRNGDrng *drng = &inst->drng;
    g_assert(drng->instantiated);

    if (ot_csrng_drng_remaining_count(inst)) {
        xtrace_ot_csrng_info("remaining packets to generate",
                             ot_csrng_drng_remaining_count(inst));
        xtrace_ot_csrng_error("New generate request before completion");
        /* should we resume? */
    }

    inst->drng.rem_packet_count = packet_count;

    /*
     * do not ack command yet,
     * should only be completed once remamining packets reach zero
     */
    return CSRNG_CMD_DEFERRED;
}

static OtCSRNDCmdResult ot_csrng_handle_reseed(OtCSRNGState *s, unsigned slot)
{
    OtCSRNGInstance *inst = &s->instances[slot];

    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);
    bool flag0 =
        FIELD_EX32(command, OT_CSNRG_CMD, FLAG0) == OT_MULTIBITBOOL4_TRUE;

    xtrace_ot_csrng_info("reseed", flag0);

    uint32_t clen = FIELD_EX32(command, OT_CSNRG_CMD, CLEN);
    if (clen) {
        uint32_t num;
        const uint32_t *buffer =
            ot_fifo32_peek_buf(&inst->cmd_fifo,
                               ot_fifo32_num_used(&inst->cmd_fifo), &num);
        g_assert(num - 1u == clen);
        buffer += 1u;
        /* ignore trailing additional words, as HW does */
        clen = MIN(clen, OT_CSRNG_CMD_WORD_MAX);

        xtrace_ot_csrng_show_buffer(ot_csrng_get_slot(inst), "mat", buffer,
                                    clen * sizeof(uint32_t));

        ot_csrng_drng_store_material(inst, buffer, clen);
    }

    int res;
    res = ot_csrng_drng_reseed(inst, flag0);
    if ((res == CSRNG_CMD_OK) && !flag0) {
        /* if flag0 is set, entropy source is not used for reseeding */
        s->regs[R_INTR_STATE] |= INTR_CS_ENTROPY_REQ_MASK;
        ot_csrng_update_irqs(s);
    }

    return res;
}

static OtCSRNDCmdResult ot_csrng_handle_update(OtCSRNGState *s, unsigned slot)
{
    OtCSRNGInstance *inst = &s->instances[slot];

    xtrace_ot_csrng_info("update", 0);

    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);
    uint32_t clen = FIELD_EX32(command, OT_CSNRG_CMD, CLEN);

    if (clen) {
        uint32_t num;
        const uint32_t *buffer =
            ot_fifo32_peek_buf(&inst->cmd_fifo,
                               ot_fifo32_num_used(&inst->cmd_fifo), &num);
        g_assert(num - 1u == clen);
        buffer += 1u;
        /* ignore trailing additional words, as HW does */
        clen = MIN(clen, OT_CSRNG_CMD_WORD_MAX);

        xtrace_ot_csrng_show_buffer(ot_csrng_get_slot(inst), "mat", buffer,
                                    clen * sizeof(uint32_t));

        ot_csrng_drng_store_material(inst, buffer, clen);
    }

    ot_csrng_drng_update(inst);

    return CSRNG_CMD_OK;
}

static void ot_csrng_hwapp_ready_irq(void *opaque, int n, int level)
{
    /* signaled by a CSRNG HW APP client */
    OtCSRNGState *s = opaque;

    unsigned slot = (unsigned)n;
    g_assert(slot < OT_CSRNG_HW_APP_MAX);
    bool ready = (bool)level;

    OtCSRNGInstance *inst = &s->instances[slot];
    inst->hw.genbits_ready = ready;

    trace_ot_csrng_hwapp_ready(slot, ready,
                               ot_csrng_drng_remaining_count(inst));

    if (ready) {
        if (!inst->drng.seeded) {
            /*
             * instanciation has not completed yet, wait for generation.
             * see #ot_csrng_drng_instanciate
             */
            trace_ot_csrng_defer_generation(slot);
        } else {
            qemu_bh_schedule(inst->hw.filler_bh);
        }
    }
}

static void ot_csrng_hwapp_filler_bh(void *opaque)
{
    /* scheduled following a CSRNG HW APP client ready to receive entropy */
    OtCSRNGInstance *inst = opaque;

    /*
     * client may have updated its readiness status since this BH has been
     * scheduled, readiness should always be tested
     */
    if (inst->hw.genbits_ready && ot_csrng_drng_remaining_count(inst)) {
        uint32_t bits[OT_CSRNG_PACKET_WORD_COUNT];
        bool fips;
        ot_csrng_drng_generate(inst, bits, &fips);

        /*
         * client callback may trigger ot_csrng_hwapp_ready_irq to update its
         * readiness status, so client state past this point may have been
         * updated
         */
        inst->hw.filler(inst->hw.opaque, bits, fips);

        /*
         * reschedule self if the client does not update its readiness, it
         * expects more entropy from this instance.
         */
        if (ot_csrng_drng_remaining_count(inst)) {
            qemu_bh_schedule(inst->hw.filler_bh);
        }
    }

    /* check if the instance is running a deferred completion command */
    if (inst->defer_completion) {
        /*
         * if no more entropy packets can be generated with the current
         * command signal the command completion to the client, whatever
         * its readiness status (only generate commands complete async.)
         */
        if (!ot_csrng_drng_remaining_count(inst)) {
            ot_csrng_complete_command(inst, CSRNG_STATUS_SUCCESS);
        }
    }
}

static void ot_csrng_swapp_fill(OtCSRNGInstance *inst)
{
    unsigned count = ot_csrng_drng_remaining_count(inst);

    if (count > 0) {
        /* refill SW FIFO */
        trace_ot_csrng_swapp_fill(count);
        uint32_t bits[OT_CSRNG_PACKET_WORD_COUNT];
        ot_csrng_drng_generate(inst, bits, &inst->sw.fips);
        for (unsigned ix = 0; ix < OT_CSRNG_PACKET_WORD_COUNT; ix++) {
            /* reverse all bytes to fit OpenTitan order */
            ot_fifo32_push(&inst->sw.bits_fifo,
                           bswap32(bits[OT_CSRNG_PACKET_WORD_COUNT - ix - 1u]));
        }
        xtrace_ot_csrng_info("swfifo empty",
                             ot_fifo32_is_empty(&inst->sw.bits_fifo));
    } else {
        /* check if the instance is running an deferred completion command */
        if (inst->defer_completion) {
            ot_csrng_complete_command(inst, CSRNG_STATUS_SUCCESS);
        }
    }
}

static bool
ot_csrng_instance_is_command_ready(const OtCSRNGInstance *inst, bool fatal)
{
    /* there should be a full command stored in the command FIFO */
    if (ot_fifo32_is_empty(&inst->cmd_fifo)) {
        return false;
    }
    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);
    uint32_t length = FIELD_EX32(command, OT_CSNRG_CMD, CLEN) + 1u;

    bool is_ready = ot_fifo32_num_used(&inst->cmd_fifo) == length;

    if (fatal && !is_ready) {
        unsigned word_count = ot_fifo32_num_used(&inst->cmd_fifo);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %u: command 0x%06x empty: %u, length %u, exp: %u\n",
                      __func__, ot_csrng_get_slot(inst), command,
                      ot_fifo32_is_empty(&inst->cmd_fifo), word_count, length);
        if (word_count < length) {
            xtrace_ot_csrng_error("cannot execute an incomplete command");
        } else {
            xtrace_ot_csrng_error("cannot execute an overflowed command");
        }
    }

    return is_ready;
}

static OtCSRNDCmdResult ot_csrng_handle_command(OtCSRNGState *s, unsigned slot)
{
    OtCSRNGInstance *inst = &s->instances[slot];

    /* note: cmd_fifo is only emptied when command has been completed */
    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);

    uint32_t acmd = FIELD_EX32(command, OT_CSNRG_CMD, ACMD);

    trace_ot_csrng_show_command("handle", slot, CMD_NAME(acmd), acmd);

    switch (acmd) {
    case OT_CSRNG_CMD_INSTANTIATE:
    case OT_CSRNG_CMD_RESEED:
        break;
    case OT_CSRNG_CMD_GENERATE:
        g_assert(slot == SW_INSTANCE_ID || inst->hw.filler);
        break;
    case OT_CSRNG_CMD_UPDATE:
    case OT_CSRNG_CMD_UNINSTANTIATE:
        break;
    case OT_CSRNG_CMD_NONE:
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Unknown command: %u\n", acmd);
        // JW: check this shouldn't be CMD_STAGE_INVALID_CMD_SEQ_ALERT.
        s->regs[R_RECOV_ALERT_STS] |=
            R_RECOV_ALERT_STS_CMD_STAGE_INVALID_ACMD_ALERT_MASK;
        CHANGE_STATE(s, CSRNG_ERROR);
        ot_csrng_update_alerts(s);
        return CSRNG_CMD_INVALID_ACMD;
    }

    if (!s->enabled) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: not enabled\n", __func__);
        return CSRNG_CMD_INVALID_ACMD;
    }

    switch (acmd) {
    case OT_CSRNG_CMD_INSTANTIATE:
        if (ot_csrng_drng_is_instantiated(inst)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: instance %u already active\n",
                          __func__, slot);
            return CSRNG_CMD_INVALID_CMD_SEQ;
        }
        break;
    case OT_CSRNG_CMD_UNINSTANTIATE:
        break;
    default:
        if (!ot_csrng_drng_is_instantiated(inst)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: instance %u not instantiated\n",
                          __func__, slot);
            CHANGE_STATE(s, CSRNG_ERROR);
            ot_csrng_update_alerts(s);
            return CSRNG_CMD_INVALID_CMD_SEQ;
        }
    }

    OtCSRNDCmdResult res;

    switch (acmd) {
    case OT_CSRNG_CMD_INSTANTIATE:
        CHANGE_STATE(s, CSRNG_INSTANT_REQ);
        res = ot_csrng_handle_instantiate(s, slot);
        break;
    case OT_CSRNG_CMD_RESEED:
        CHANGE_STATE(s, CSRNG_RESEED_REQ);
        res = ot_csrng_handle_reseed(s, slot);
        break;
    case OT_CSRNG_CMD_GENERATE:
        CHANGE_STATE(s, CSRNG_GENERATE_REQ);
        res = ot_csrng_handle_generate(s, slot);
        if (res == CSRNG_CMD_DEFERRED) {
            if (ot_csrng_drng_remaining_count(inst)) {
                if (slot != SW_INSTANCE_ID) {
                    /* HW instance */
                    if (inst->hw.genbits_ready) {
                        qemu_bh_schedule(inst->hw.filler_bh);
                    } else {
                        xtrace_ot_csrng_info("genbit not ready", slot);
                    }
                } else {
                    /* SW instance */
                    ot_csrng_swapp_fill(inst);
                }
            }
        }
        break;
    case OT_CSRNG_CMD_UPDATE:
        CHANGE_STATE(s, CSRNG_UPDATE_REQ);
        res = ot_csrng_handle_update(s, slot);
        break;
    case OT_CSRNG_CMD_UNINSTANTIATE:
        CHANGE_STATE(s, CSRNG_UNINSTANT_REQ);
        res = ot_csrng_handle_uninstantiate(s, slot);
        break;
    default:
        g_assert_not_reached();
        break;
    }

    return res;
}

static void ot_csrng_command_schedule(OtCSRNGState *s, OtCSRNGInstance *inst)
{
    bool queued = ot_csrng_is_in_queue(inst);
    if (queued) {
        /* execution resumes, but this is likely internal bug...*/
        error_report("%s: instance executing several commands at once",
                     __func__);
        s->regs[R_INTR_STATE] |= INTR_CS_HW_INST_EXC_MASK;
        ot_csrng_update_irqs(s);
        g_assert(false);
        return;
    }

    QSIMPLEQ_INSERT_TAIL(&s->cmd_requests, inst, cmd_request);

    trace_ot_csrng_schedule(ot_csrng_get_slot(inst), "command");
    timer_del(s->entropy_scheduler);
    qemu_bh_schedule(s->cmd_scheduler);
}

static void ot_csrng_command_scheduler(void *opaque)
{
    OtCSRNGState *s = opaque;

    /* handle a single instance per cycle */
    OtCSRNGInstance *inst = QSIMPLEQ_FIRST(&s->cmd_requests);
    if (!inst) {
        xtrace_ot_csrng_error("scheduled request w/o request?");
        g_assert_not_reached();
    }

    if (s->state != CSRNG_IDLE) {
        trace_ot_csrng_invalid_state(__func__, STATE_NAME(s->state), s->state);
        g_assert_not_reached();
    }

    uint32_t command = ot_fifo32_peek(&inst->cmd_fifo);
    uint32_t acmd = FIELD_EX32(command, OT_CSNRG_CMD, ACMD);
    unsigned slot = ot_csrng_get_slot(inst);

    trace_ot_csrng_show_command("pop", slot, CMD_NAME(acmd), acmd);

    if (!ot_csrng_instance_is_command_ready(inst, true)) {
        g_assert_not_reached();
    }

    CHANGE_STATE(s, CSRNG_PARSE_CMD);

    /*
     * instance's command has been checked, remove it from the queue.
     * it is re-inserted to the tail if it cannot be completed for now. Doing so
     * allow a round-robin sequencing between several instances each requiring
     * command processing.
     */
    QSIMPLEQ_REMOVE_HEAD(&s->cmd_requests, cmd_request);

    /*
     * current instance has a pending command.
     * handle it, and prevent any other instances to be handled
     * in this round.
     */
    trace_ot_csrng_command_scheduler(slot, "cmd ready, execute");
    OtCSRNDCmdResult res;
    res = ot_csrng_handle_command(s, slot);
    switch (res) {
    case CSRNG_CMD_RETRY:
        CHANGE_STATE(s, CSRNG_IDLE);
        /* command re-insertion */
        trace_ot_csrng_show_command("push back", slot, CMD_NAME(acmd), acmd);
        QSIMPLEQ_INSERT_TAIL(&s->cmd_requests, inst, cmd_request);
        break;
    case CSRNG_CMD_DEFERRED:
        inst->defer_completion = true;
        CHANGE_STATE(s, CSRNG_IDLE);
        break;
    case CSRNG_CMD_STALLED:
        xtrace_ot_csrng_error("entropy stack stalled");
        /* discard any remaining requests, nothing can be done any further */
        while (!QSIMPLEQ_EMPTY(&s->cmd_requests)) {
            QSIMPLEQ_REMOVE_HEAD(&s->cmd_requests, cmd_request);
        }
        /* do not complete command either */
        break;
    case CSRNG_CMD_OK:
        ot_csrng_complete_command(inst, CSRNG_STATUS_SUCCESS);
        break;
    default:
        trace_ot_csrng_reject_command(slot, command, res);
        /*
         * Negative res values are errors, encoding the type of command error.
         * Convert the encoded error back into a OtCSRNGCmdStatus.
         */
        ot_csrng_complete_command(inst, -res);
        break;
    }

    if (!QSIMPLEQ_EMPTY(&s->cmd_requests)) {
        if (s->state != CSRNG_ERROR) {
            trace_ot_csrng_scheduling_command(slot);
            if (CSRNG_CMD_RETRY == res && s->entropy_delay) {
                timer_mod(s->entropy_scheduler,
                          qemu_clock_get_ns(OT_VIRTUAL_CLOCK) +
                              (int64_t)s->entropy_delay);
                s->entropy_delay = 0;
            } else {
                qemu_bh_schedule(s->cmd_scheduler);
            }
        } else {
            xtrace_ot_csrng_error("cannot schedule new command on error");
        }
    }
}

static uint32_t ot_csrng_read_state_db(OtCSRNGState *s)
{
    unsigned appid = s->regs[R_INT_STATE_NUM];
    if (appid >= N_APP_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid appid %d\n", __func__,
                      appid);
        return 0;
    }
    if (!s->read_int_granted) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read state db disabled\n",
                      __func__);
        return 0;
    }
    if (!((s->regs[R_INT_STATE_READ_ENABLE] >> appid) & 0x1)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read state db not enable for %d\n",
                      __func__, appid);
        return 0;
    }

    OtCSRNGInstance *inst = &s->instances[appid];
    OtCSRNGDrng *drng = &inst->drng;
    uint32_t val32;
    unsigned base;
    switch (s->state_db_ix) {
    case 0: /* Reseed counter */
        val32 = drng->reseed_counter;
        break;
    case 1u ... 4u: /* V (counter) */
        /* use big endian and reverse order to match OpenTitan order */
        base = 4u - s->state_db_ix;
        val32 = ldl_be_p(&drng->v_counter[base * sizeof(uint32_t)]);
        break;
    case 5u ... 12u: /* Key */
        /* use big endian and reverse order to match OpenTitan order */
        base = 12u - s->state_db_ix;
        val32 = ldl_be_p(&drng->key[base * sizeof(uint32_t)]);
        break;
    case 13u: /* Status + Compliance, only 8 LSBs matter */
        val32 = (uint32_t)((((uint8_t)drng->instantiated) << 0u) |
                           (((uint8_t)!drng->no_fips) << 1u));
        break;
    default:
        val32 = 0;
        break;
    }

    trace_ot_csrng_read_state_db(ot_csrng_get_slot(inst), s->state_db_ix,
                                 val32);

    s->state_db_ix += 1u;
    if (s->state_db_ix >= 14u) {
        s->state_db_ix = 0;
    }

    return val32;
}

static uint64_t ot_csrng_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtCSRNGState *s = opaque;
    (void)size;
    uint32_t val32;
    OtCSRNGInstance *inst;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_REGWEN:
    case R_CTRL:
    case R_RESEED_INTERVAL:
    case R_SW_CMD_STS:
    case R_INT_STATE_NUM:
    case R_HW_EXC_STS:
    case R_RECOV_ALERT_STS:
    case R_ERR_CODE:
    case R_ERR_CODE_TEST:
        val32 = s->regs[reg];
        break;
    case R_RESEED_COUNTER_0:
    case R_RESEED_COUNTER_1:
    case R_RESEED_COUNTER_2: {
        unsigned appid = reg - R_RESEED_COUNTER_0;
        g_assert(appid < N_APP_COUNT);
        inst = &s->instances[appid];
        const OtCSRNGDrng *drng = &inst->drng;
        val32 = drng->reseed_counter;
    } break;
    case R_INT_STATE_VAL:
        val32 = ot_csrng_read_state_db(s);
        break;
    case R_MAIN_SM_STATE:
        switch (s->state) {
        case CSRNG_IDLE ... CSRNG_ERROR:
            val32 = OtCSRNGFsmStateCode[s->state];
            break;
        default:
            val32 = OtCSRNGFsmStateCode[CSRNG_ERROR];
            break;
        }
        break;
    case R_GENBITS_VLD:
        inst = &s->instances[SW_INSTANCE_ID];
        if (ot_csrng_is_sw_app_enabled(s)) {
            bool avail = !ot_fifo32_is_empty(&inst->sw.bits_fifo);
            val32 = FIELD_DP32(0, GENBITS_VLD, GENBITS_VLD, (uint32_t)avail);
            val32 = FIELD_DP32(val32, GENBITS_VLD, GENBITS_FIPS, inst->sw.fips);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "SW APP not enabled\n");
            val32 = 0;
        }
        break;
    case R_GENBITS:
        if (ot_csrng_is_sw_app_enabled(s)) {
            inst = &s->instances[SW_INSTANCE_ID];
            if (!ot_fifo32_is_empty(&inst->sw.bits_fifo)) {
                val32 = ot_fifo32_pop(&inst->sw.bits_fifo);
                xtrace_ot_csrng_info("pop", val32);
                if (ot_fifo32_is_empty(&inst->sw.bits_fifo)) {
                    /* keep the SW FIFO filled up till end of generation */
                    ot_csrng_swapp_fill(inst);
                }
            } else {
                /* TBC: need to check if some error is signaled in ERR_CODE */
                qemu_log_mask(LOG_GUEST_ERROR, "FIFO read w/ entropy bits\n");
                val32 = 0;
            }
        } else {
            val32 = 0;
        }
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_CMD_REQ:
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
    trace_ot_csrng_io_read_out((uint32_t)addr, REG_NAME(reg), val32, pc);

    return (uint64_t)val32;
};

static void ot_csrng_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    OtCSRNGState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;
    OtCSRNGInstance *inst;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_csrng_io_write((uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_csrng_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_csrng_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_csrng_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        s->regs[reg] = val32;
        ot_csrng_update_alerts(s);
        break;
    case R_REGWEN:
        val32 &= R_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* rw0c */
        break;
    case R_CTRL:
        if (!s->regs[R_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s protected w/ REGWEN\n",
                          __func__, REG_NAME(reg));
            break;
        }
        uint32_t prev = s->regs[reg];
        val32 &= CTRL_MASK;
        s->regs[reg] = val32;
        CHECK_MULTIBOOT(s, CTRL, ENABLE);
        CHECK_MULTIBOOT(s, CTRL, SW_APP_ENABLE);
        CHECK_MULTIBOOT(s, CTRL, READ_INT_STATE);
        CHECK_MULTIBOOT(s, CTRL, FIPS_FORCE_ENABLE);
        uint32_t change = val32 ^ prev;
        if (change) {
            xtrace_ot_csrng_info("handling CTRL change", val32);
            ot_csrng_handle_enable(s);

            OtOTPIfClass *oc = OT_OTP_IF_GET_CLASS(s->otp_ctrl);
            OtOTPIf *oi = OT_OTP_IF(s->otp_ctrl);
            const OtOTPHWCfg *hw_cfg = oc->get_hw_cfg(oi);
            g_assert(hw_cfg);
            if (hw_cfg->en_csrng_sw_app_read_mb8 == OT_MULTIBITBOOL8_TRUE) {
                uint32_t sw_app_en = FIELD_EX32(val32, CTRL, SW_APP_ENABLE);
                s->sw_app_granted = sw_app_en == OT_MULTIBITBOOL4_TRUE;
                uint32_t read_int = FIELD_EX32(val32, CTRL, READ_INT_STATE);
                s->read_int_granted = read_int == OT_MULTIBITBOOL4_TRUE;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: SW APP disabled in OTP\n",
                              __func__);
                s->sw_app_granted = false;
                s->read_int_granted = false;
            }
        }
        break;
    case R_CMD_REQ:
        inst = &s->instances[SW_INSTANCE_ID];
        if (!ot_fifo32_is_full(&inst->cmd_fifo)) {
            ot_fifo32_push(&inst->cmd_fifo, val32);
            if (ot_csrng_instance_is_command_ready(inst, false)) {
                /*
                 * assume CMD RDY works the same way as the csrng_req_ready
                 * wire, which is a blind guess, need to check RTL here
                 */
                s->regs[R_SW_CMD_STS] &= ~R_SW_CMD_STS_CMD_RDY_MASK;
                ot_csrng_command_schedule(s, inst);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: cmd req overflow\n", __func__);
            s->regs[R_SW_CMD_STS] &= ~R_SW_CMD_STS_CMD_RDY_MASK;
            /* TBC: how to signal this error */
        }
        break;
    case R_RESEED_INTERVAL:
        s->regs[reg] = val32;
        ot_csrng_update_irqs(s);
        break;
    case R_INT_STATE_READ_ENABLE:
        if (!s->regs[R_INT_STATE_READ_ENABLE_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s protected w/ REGWEN\n",
                          __func__, REG_NAME(reg));
            break;
        }
        val32 &= R_INT_STATE_READ_ENABLE_VAL_MASK;
        s->regs[reg] = val32;
        break;
    case R_INT_STATE_READ_ENABLE_REGWEN:
        val32 &= R_INT_STATE_READ_ENABLE_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* rw0c */
        break;
    case R_INT_STATE_NUM:
        if (s->read_int_granted) {
            val32 &= R_INT_STATE_NUM_VAL_MASK;
            s->regs[reg] = val32;
            s->state_db_ix = 0;
            if (val32 > OT_CSRNG_HW_APP_MAX) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid INT_STATE_NUM %u\n",
                              __func__, val32);
            }
        }
        break;
    case R_FIPS_FORCE:
        if (!s->regs[R_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s protected w/ REGWEN\n",
                          __func__, REG_NAME(reg));
            break;
        }
        val32 &= R_FIPS_FORCE_VAL_MASK;
        s->regs[reg] = val32;
        break;
    case R_HW_EXC_STS:
        val32 &= R_HW_EXC_STS_VAL_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_RECOV_ALERT_STS:
        val32 &= RECOV_ALERT_STS_MASK;
        s->regs[reg] &= val32; /* RW0C */
        ot_csrng_update_alerts(s);
        break;
    case R_ERR_CODE_TEST:
        if (!s->regs[R_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s protected w/ REGWEN\n",
                          __func__, REG_NAME(reg));
            break;
        }
        val32 &= R_ERR_CODE_TEST_VAL_MASK;
        val32 = 1u << val32;
        val32 &= ERR_CODE_MASK;
        s->regs[R_ERR_CODE] = val32;
        break;
    case R_RESEED_COUNTER_0:
    case R_RESEED_COUNTER_1:
    case R_RESEED_COUNTER_2:
    case R_SW_CMD_STS:
    case R_GENBITS_VLD:
    case R_GENBITS:
    case R_INT_STATE_VAL:
    case R_ERR_CODE:
    case R_MAIN_SM_STATE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        break;
    default:
        // JW: handle new registers.
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        break;
    }
};

static const Property ot_csrng_properties[] = {
    DEFINE_PROP_LINK("entropy-src", OtCSRNGState, entropy_src,
                     TYPE_OT_ENTROPY_SRC, OtEntropySrcState *),
    DEFINE_PROP_LINK("otp-ctrl", OtCSRNGState, otp_ctrl, TYPE_OT_OTP_IF,
                     DeviceState *),
};

static const MemoryRegionOps ot_csrng_regs_ops = {
    .read = &ot_csrng_regs_read,
    .write = &ot_csrng_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_csrng_reset_enter(Object *obj, ResetType type)
{
    OtCSRNGClass *c = OT_CSRNG_GET_CLASS(obj);
    OtCSRNGState *s = OT_CSRNG(obj);

    trace_ot_csrng_reset();

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    qemu_bh_cancel(s->cmd_scheduler);
    timer_del(s->entropy_scheduler);
    for (unsigned ix = 0; ix < OT_CSRNG_HW_APP_MAX; ix++) {
        g_assert(ix != SW_INSTANCE_ID);
        OtCSRNGInstance *inst = &s->instances[ix];
        qemu_bh_cancel(inst->hw.filler_bh);
    }
    s->entropy_delay = 0;

    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_REGWEN] = 0x1u;
    s->regs[R_CTRL] = 0x9999u;
    s->regs[R_RESEED_INTERVAL] = 0xffffffffu;
    s->regs[R_INT_STATE_READ_ENABLE_REGWEN] = 0x1u;
    s->regs[R_INT_STATE_READ_ENABLE] = 0x7u;
    s->regs[R_MAIN_SM_STATE] = 0x4eu;
    s->enabled = false;
    s->sw_app_granted = false;
    s->read_int_granted = false;
    s->es_retry_count = 0;
    s->state = CSRNG_IDLE;

    for (unsigned ix = 0; ix < N_APP_COUNT; ix++) {
        OtCSRNGInstance *inst = &s->instances[ix];
        g_assert(inst->parent);
        ot_fifo32_reset(&inst->cmd_fifo);
        if (ix == SW_INSTANCE_ID) {
            ot_fifo32_reset(&inst->sw.bits_fifo);
            inst->sw.fips = false;
        }
        OtCSRNGDrng *drng = &inst->drng;
        memset(drng, 0, sizeof(*drng));
    }
    ot_csrng_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    while (!QSIMPLEQ_EMPTY(&s->cmd_requests)) {
        QSIMPLEQ_REMOVE_HEAD(&s->cmd_requests, cmd_request);
    }
}

static void ot_csrng_realize(DeviceState *dev, Error **errp)
{
    OtCSRNGState *s = OT_CSRNG(dev);
    (void)errp;

    g_assert(s->entropy_src);
    g_assert(s->otp_ctrl);

    (void)OBJECT_CHECK(OtOTPIf, s->otp_ctrl, TYPE_OT_OTP_IF);
}

static void ot_csrng_init(Object *obj)
{
    OtCSRNGState *s = OT_CSRNG(obj);

    memory_region_init_io(&s->mmio, obj, &ot_csrng_regs_ops, s, TYPE_OT_CSRNG,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    /* aes_desc is defined in libtomcrypt */
    s->aes_cipher = register_cipher(&aes_desc);
    if (s->aes_cipher < 0) {
        error_report("%s: unable to use libtomcrypt AES API", __func__);
    }

    s->regs = g_new0(uint32_t, REGS_COUNT);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    qdev_init_gpio_in_named_with_opaque(DEVICE(s), &ot_csrng_hwapp_ready_irq, s,
                                        TYPE_OT_CSRNG "-genbits_ready",
                                        OT_CSRNG_HW_APP_MAX);

    static_assert(CMD_FIFO_CAPACITY >= OT_CSRNG_CMD_WORD_MAX,
                  "Invalid CMD FIFO size");
    /* HW instances + 1 internal SW instance */
    s->instances = g_new0(OtCSRNGInstance, N_APP_COUNT);
    OtCSRNGInstance *inst = &s->instances[SW_INSTANCE_ID];
    for (unsigned ix = 0; ix < N_APP_COUNT; ix++) {
        inst = &s->instances[ix];
        inst->parent = s;
        ot_fifo32_create(&inst->cmd_fifo, CMD_FIFO_CAPACITY);
        if (ix != SW_INSTANCE_ID) {
            inst->hw.filler_bh = qemu_bh_new(&ot_csrng_hwapp_filler_bh, inst);
        }
    }
    /* current instance is the SW instance */
    ot_fifo32_create(&inst->sw.bits_fifo, OT_CSRNG_PACKET_WORD_COUNT);

    s->cmd_scheduler = qemu_bh_new(&ot_csrng_command_scheduler, s);
    s->entropy_scheduler =
        timer_new_ns(OT_VIRTUAL_CLOCK, &ot_csrng_command_scheduler, s);

    QSIMPLEQ_INIT(&s->cmd_requests);
}

static void ot_csrng_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_csrng_realize;
    device_class_set_props(dc, ot_csrng_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtCSRNGClass *cc = OT_CSRNG_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_csrng_reset_enter, NULL, NULL,
                                       &cc->parent_phases);

    cc->connect_hw_app = &ot_csrng_connect_hw_app;
    cc->push_command = &ot_csrng_push_command;
}

static const TypeInfo ot_csrng_info = {
    .name = TYPE_OT_CSRNG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtCSRNGState),
    .instance_init = &ot_csrng_init,
    .class_size = sizeof(OtCSRNGClass),
    .class_init = &ot_csrng_class_init,
};

static void ot_csrng_register_types(void)
{
    type_register_static(&ot_csrng_info);
}

type_init(ot_csrng_register_types);
