/*
 * QEMU OpenTitan One Time Programmable (OTP) memory controller
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
 *  Alex Jones <alex.jones@lowrisc.org>
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

#ifndef HW_OPENTITAN_OT_OTP_ENGINE_H
#define HW_OPENTITAN_OT_OTP_ENGINE_H

#include "qemu/timer.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_otp_be_if.h"
#include "hw/opentitan/ot_otp_if.h"
#include "hw/opentitan/ot_otp_impl_if.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"

#undef OT_OTP_DEBUG

#define TYPE_OT_OTP_ENGINE "ot-otp_engine"
OBJECT_DECLARE_TYPE(OtOTPEngineState, OtOTPEngineClass, OT_OTP_ENGINE)

#define OT_OTP_NUM_IRQS         2u
#define OT_OTP_NUM_ALERTS       5u
#define OT_OTP_NUM_DAI_WORDS    2u
#define OT_OTP_NUM_DIGEST_WORDS 2u
#define OT_OTP_NUM_ZER_WORDS    2u


#define LC_TRANSITION_CNT_SIZE 48u
#define LC_STATE_SIZE          40u

#define OT_OTP_DIGEST_ADDR_MASK (sizeof(uint64_t) - 1u)
#define OT_OTP_ZER_ADDR_MASK    (sizeof(uint64_t) - 1u)

/* Error code (compliant with ERR_CODE registers) */
typedef enum {
    OT_OTP_NO_ERROR,
    OT_OTP_MACRO_ERROR,
    OT_OTP_MACRO_ECC_CORR_ERROR, /* This is NOT an error */
    OT_OTP_MACRO_ECC_UNCORR_ERROR,
    OT_OTP_MACRO_WRITE_BLANK_ERROR,
    OT_OTP_ACCESS_ERROR,
    OT_OTP_CHECK_FAIL_ERROR, /* Digest error */
    OT_OTP_FSM_STATE_ERROR,
    OT_OTP_ERROR_COUNT,
} OtOTPError;

/* States of an unbuffered partition FSM */
typedef enum {
    OT_OTP_UNBUF_RESET,
    OT_OTP_UNBUF_INIT,
    OT_OTP_UNBUF_INIT_WAIT,
    OT_OTP_UNBUF_IDLE,
    OT_OTP_UNBUF_READ,
    OT_OTP_UNBUF_READ_WAIT,
    OT_OTP_UNBUF_ERROR,
} OtOTPUnbufState;

/* States of a buffered partition FSM */
typedef enum {
    OT_OTP_BUF_RESET,
    OT_OTP_BUF_INIT,
    OT_OTP_BUF_INIT_WAIT,
    OT_OTP_BUF_INIT_DESCR,
    OT_OTP_BUF_INIT_DESCR_WAIT,
    OT_OTP_BUF_IDLE,
    OT_OTP_BUF_INTEG_SCR,
    OT_OTP_BUF_INTEG_SCR_WAIT,
    OT_OTP_BUF_INTEG_DIG_CLR,
    OT_OTP_BUF_INTEG_DIG,
    OT_OTP_BUF_INTEG_DIG_PAD,
    OT_OTP_BUF_INTEG_DIG_FIN,
    OT_OTP_BUF_INTEG_DIG_WAIT,
    OT_OTP_BUF_CNSTY_READ,
    OT_OTP_BUF_CNSTY_READ_WAIT,
    OT_OTP_BUF_ERROR,
} OtOTPBufState;

/* Direct Access Interface states */
typedef enum {
    OT_OTP_DAI_RESET,
    OT_OTP_DAI_INIT_OTP,
    OT_OTP_DAI_INIT_PART,
    OT_OTP_DAI_IDLE,
    OT_OTP_DAI_ERROR,
    OT_OTP_DAI_READ,
    OT_OTP_DAI_READ_WAIT,
    OT_OTP_DAI_DESCR,
    OT_OTP_DAI_DESCR_WAIT,
    OT_OTP_DAI_WRITE,
    OT_OTP_DAI_WRITE_WAIT,
    OT_OTP_DAI_SCR,
    OT_OTP_DAI_SCR_WAIT,
    OT_OTP_DAI_DIG_CLR,
    OT_OTP_DAI_DIG_READ,
    OT_OTP_DAI_DIG_READ_WAIT,
    OT_OTP_DAI_DIG,
    OT_OTP_DAI_DIG_PAD,
    OT_OTP_DAI_DIG_FIN,
    OT_OTP_DAI_DIG_WAIT,
} OtOTPDAIState;

typedef enum {
    OT_OTP_LCI_RESET,
    OT_OTP_LCI_IDLE,
    OT_OTP_LCI_WRITE,
    OT_OTP_LCI_WRITE_WAIT,
    OT_OTP_LCI_ERROR,
} OtOTPLCIState;

typedef enum {
    OT_OTP_DA_REG_REGWEN,
    OT_OTP_DA_REG_CMD,
    OT_OTP_DA_REG_ADDRESS,
    OT_OTP_DA_REG_WDATA_0,
    OT_OTP_DA_REG_WDATA_1,
    OT_OTP_DA_REG_RDATA_0,
    OT_OTP_DA_REG_RDATA_1,
} OtOTPDirectAccessRegister;

typedef struct {
    union {
        OtOTPBufState b;
        OtOTPUnbufState u;
    } state;
    struct {
        uint32_t *data; /* size, see OtOTPPartDesc w/o digest data */
        uint64_t next_digest; /* computed HW digest to store into OTP cell */
    } buffer; /* only meaningful for buffered partitions */
    uint64_t digest; /* digest as read from OTP back end at init time */
    bool locked;
    bool failed;
    bool read_lock;
    bool write_lock;
    /* OTP scrambling key constant, not constant for deriving other keys */
    uint8_t *otp_scramble_key; /* may be NULL */
    uint8_t *inv_default_data; /* may be NULL */
} OtOTPPartController;

typedef struct OtOTPDAIController {
    QEMUTimer *delay; /* simulate delayed access completion */
    QEMUBH *digest_bh; /* write computed digest to OTP cell */
    OtOTPDAIState state;
    int partition; /* current partition being worked on or -1 */
} OtOTPDAIController;

typedef struct {
    QEMUTimer *prog_delay; /* OTP cell prog delay (use OT_OTP_HW_CLOCK) */
    OtOTPLCIState state;
    OtOTPError error;
    ot_otp_program_ack_fn ack_fn;
    void *ack_data;
    uint16_t *data;
    unsigned hpos; /* current offset in data */
} OtOTPLCIController;

typedef struct {
    uint32_t *storage; /* overall buffer for the storage backend */
    uint32_t *data; /* data buffer (all partitions) */
    uint32_t *ecc; /* ecc buffer for date */
    unsigned size; /* overall storage size in bytes */
    unsigned data_size; /* data buffer size in bytes */
    unsigned ecc_size; /* ecc buffer size in bytes */
    unsigned ecc_bit_count; /* count of ECC bit for each data granule */
    unsigned ecc_granule; /* size of a granule in bytes */
} OtOTPStorage;

typedef struct {
    QEMUBH *bh;
    uint16_t signal; /* each bit tells if signal needs to be handled */
    uint16_t level; /* level of the matching signal */
    uint16_t current_level; /* current level of all signals */
} OtOTPLcBroadcast;

static_assert(OT_OTP_LC_BROADCAST_COUNT < 8 * sizeof(uint16_t),
              "Invalid OT_OTP_LC_BROADCAST_COUNT");

typedef struct OtOTPKeyGen_ OtOTPKeyGen;
typedef struct OtOTPScrmblKeyInit_ OtOTPScrmblKeyInit;

typedef struct {
    uint32_t dai_base; /* offset of DIRECT_ACCESS_REGWEN register */
    uint32_t err_code_base; /* offset of ERR_CODE_0 register */
    uint32_t read_lock_base; /* offset of first *_READ_LOCK  */
} OtOTPRegisterOffset;

struct OtOTPEngineState {
    SysBusDevice parent_obj;

    QEMUBH *pwr_otp_bh;
    IbexIRQ irqs[OT_OTP_NUM_IRQS];
    IbexIRQ alerts[OT_OTP_NUM_ALERTS];
    IbexIRQ pwc_otp_rsp;

    uint32_t *regs;
    uint32_t alert_bm;

    OtOTPLcBroadcast lc_broadcast;
    OtOTPDAIController *dai;
    OtOTPLCIController *lci;
    OtOTPPartController *part_ctrls;
    const OtOTPPartDesc *part_descs;
    unsigned part_count;
    unsigned part_lc_num;
    OtOTPKeyGen *keygen;
    OtOTPScrmblKeyInit *scrmbl_key_init;
    OtOtpBeCharacteristics be_chars;
    OtOTPRegisterOffset reg_offset;
    uint64_t digest_iv;
    uint8_t digest_const[16u];
    uint64_t sram_iv;
    uint8_t sram_const[16u];
    /* flash_* are only defined for OtOTPImplIf with has_flash_support set */
    uint64_t flash_data_iv;
    uint8_t flash_data_const[16u];
    uint64_t flash_addr_iv;
    uint8_t flash_addr_const[16u];

    OtOTPStorage *otp;
    OtOTPHWCfg *hw_cfg;
    OtOTPTokens *tokens;
    char *hexstr;

    char *ot_id;
    BlockBackend *blk; /* OTP host backend */
    OtOtpBeIf *otp_backend;
    OtEDNState *edn;
    char *scrmbl_key_xstr;
    char *digest_const_xstr;
    char *digest_iv_xstr;
    char *sram_const_xstr;
    char *sram_iv_xstr;
    /* flash xstrs are only valid for OtOTPImplIf with has_flash_support set */
    char *flash_data_iv_xstr;
    char *flash_data_const_xstr;
    char *flash_addr_iv_xstr;
    char *flash_addr_const_xstr;
    char **otp_scramble_key_xstrs; /* some entries may be NULL */
    char **inv_default_part_xstrs; /* some entries may be NULL */
    uint8_t edn_ep;
    bool fatal_escalate;
};

struct OtOTPEngineClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;

    /* @todo document all those */
    void (*update_irqs)(OtOTPEngineState *s);
    void (*update_alerts)(OtOTPEngineState *s);
    int (*get_part_from_address)(const OtOTPEngineState *s, hwaddr addr);
    uint32_t (*get_part_digest_reg)(OtOTPEngineState *s, uint32_t offset);
    bool (*is_readable)(const OtOTPEngineState *s, unsigned part_ix);
    void (*set_error)(OtOTPEngineState *s, unsigned part_ix, OtOTPError err);
    void (*dai_read)(OtOTPEngineState *s);
    void (*dai_write)(OtOTPEngineState *s);
    void (*dai_digest)(OtOTPEngineState *s);
    const OtOTPHWCfg *(*get_hw_cfg)(const OtOTPIf *dev);
    void (*get_otp_key)(OtOTPIf *dev, OtOTPKeyType type, OtOTPKey *key);
    bool (*program_req)(OtOTPIf *dev, const uint16_t *lc_tcount,
                        const uint16_t *lc_state, ot_otp_program_ack_fn ack,
                        void *opaque);
};

#ifdef OT_OTP_COMPORTABLE_REGS

/* Comportable registers are identical for all OTP variants */
REG32(INTR_STATE, 0x00u)
SHARED_FIELD(INTR_OTP_OPERATION_DONE, 0u, 1u)
SHARED_FIELD(INTR_OTP_ERROR, 1u, 1u)
REG32(INTR_ENABLE, 0x04u)
REG32(INTR_TEST, 0x08u)
REG32(ALERT_TEST, 0x0cu)
REG32(OTP_FIRST_IMPL_REG, 0x10u)
SHARED_FIELD(ALERT_FATAL_MACRO_ERROR, 0u, 1u)
SHARED_FIELD(ALERT_FATAL_CHECK_ERROR, 1u, 1u)
SHARED_FIELD(ALERT_FATAL_BUS_INTEG_ERROR, 2u, 1u)
SHARED_FIELD(ALERT_FATAL_PRIM_OTP_ALERT, 3u, 1u)
SHARED_FIELD(ALERT_RECOV_PRIM_OTP_ALERT, 4u, 1u)

#define INTR_WMASK (INTR_OTP_OPERATION_DONE_MASK | INTR_OTP_ERROR_MASK)

#define ALERT_WMASK \
    (ALERT_FATAL_MACRO_ERROR_MASK | ALERT_FATAL_CHECK_ERROR_MASK | \
     ALERT_FATAL_BUS_INTEG_ERROR_MASK | ALERT_FATAL_PRIM_OTP_ALERT_MASK | \
     ALERT_RECOV_PRIM_OTP_ALERT_MASK)

#endif /* OT_OTP_COMPORTABLE_REGS */

/* REG32 bitfields common to all new OTP versions */
FIELD(OT_OTP_DIRECT_ACCESS_CMD, RD, 0u, 1u)
FIELD(OT_OTP_DIRECT_ACCESS_CMD, WR, 1u, 1u)
FIELD(OT_OTP_DIRECT_ACCESS_CMD, DIGEST, 2u, 1u)
/* ZEROIZE has been introduced in OTP v2.10, after Earlgrey v1.0 / OTP v2.0 */
FIELD(OT_OTP_DIRECT_ACCESS_CMD, ZEROIZE, 3u, 1u)

SHARED_FIELD(OT_OTP_ERR_CODE, 0u, 3u)
static_assert(OT_OTP_ERR_CODE_MASK >= OT_OTP_ERROR_COUNT - 1u,
              "Error mask not large enough");

SHARED_FIELD(OT_OTP_READ_LOCK, 0u, 1u)

static inline unsigned
ot_otp_engine_part_data_offset(const OtOTPEngineState *s, unsigned part_ix)
{
    return (unsigned)(s->part_descs[part_ix].offset);
}

static inline unsigned
ot_otp_engine_part_data_byte_size(const OtOTPEngineState *s, unsigned part_ix)
{
    size_t size = s->part_descs[part_ix].size;

    if (s->part_descs[part_ix].hw_digest || s->part_descs[part_ix].sw_digest) {
        size -= sizeof(uint32_t) * OT_OTP_NUM_DIGEST_WORDS;
    }

    if (s->part_descs[part_ix].zeroizable) {
        size -= sizeof(uint32_t) * OT_OTP_NUM_ZER_WORDS;
    }

    return (unsigned)size;
}

static inline bool
ot_otp_engine_is_buffered(const OtOTPEngineState *s, unsigned part_ix)
{
    if (part_ix < s->part_count) {
        return s->part_descs[part_ix].buffered;
    }

    return false;
}

static inline bool
ot_otp_engine_is_secret(const OtOTPEngineState *s, unsigned part_ix)
{
    if (part_ix < s->part_count) {
        return s->part_descs[part_ix].secret;
    }

    return false;
}

static inline bool
ot_otp_engine_is_backend_ecc_enabled(const OtOTPEngineState *s)
{
    OtOtpBeIfClass *bec = OT_OTP_BE_IF_GET_CLASS(s->otp_backend);
    if (!bec->is_ecc_enabled) {
        return true;
    }

    return bec->is_ecc_enabled(s->otp_backend);
}

static inline bool ot_otp_engine_is_ecc_enabled(const OtOTPEngineState *s)
{
    return s->otp->ecc_granule == sizeof(uint16_t) &&
           ot_otp_engine_is_backend_ecc_enabled(s);
}

static inline bool
ot_otp_engine_has_digest(const OtOTPEngineState *s, unsigned part_ix)
{
    return s->part_descs[part_ix].hw_digest || s->part_descs[part_ix].sw_digest;
}

static inline bool ot_otp_engine_is_part_digest_offset(
    const OtOTPEngineState *s, unsigned part_ix, hwaddr addr)
{
    uint16_t offset = s->part_descs[part_ix].digest_offset;

    return (offset != UINT16_MAX) &&
           ((addr & ~OT_OTP_DIGEST_ADDR_MASK) == offset);
}

static inline bool ot_otp_engine_is_part_zer_offset(
    const OtOTPEngineState *s, unsigned part_ix, hwaddr addr)
{
    uint16_t offset = s->part_descs[part_ix].zer_offset;

    return (offset != UINT16_MAX) && ((addr & ~OT_OTP_ZER_ADDR_MASK) == offset);
}

static inline uint32_t ot_otp_engine_dai_is_busy(const OtOTPEngineState *s)
{
    return s->dai->state != OT_OTP_DAI_IDLE;
}

#endif /* HW_OPENTITAN_OT_OTP_ENGINE_H */
