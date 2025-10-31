/*
 * QEMU OpenTitan EarlGrey One Time Programmable (OTP) memory controller
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

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_fifo32.h"
#include "hw/opentitan/ot_lc_ctrl.h"
#include "hw/opentitan/ot_otp_engine.h"
#include "hw/opentitan/ot_otp_impl_if.h"
#include "hw/opentitan/ot_present.h"
#include "hw/opentitan/ot_prng.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "sysemu/block-backend.h"
#include "trace.h"

/*
 * The OTP may be used before any CPU is started, This may cause the default
 * virtual clock to stall, as the hart does not execute. OTP nevertheless may
 * be active, updating the OTP content where write delays are still needed.
 * Use the alternative clock source which counts even when the CPU is stalled.
 */
#define OT_OTP_HW_CLOCK QEMU_CLOCK_VIRTUAL_RT

/* the following delays are arbitrary for now */
#define DAI_DIGEST_DELAY_NS 50000u /* 50us */
#define LCI_PROG_SCHED_NS   1000u /* 1us*/

/* The size of keys used for OTP scrambling */
#define OTP_SCRAMBLING_KEY_WIDTH 128u
#define OTP_SCRAMBLING_KEY_BYTES ((OTP_SCRAMBLING_KEY_WIDTH) / 8u)

#define LC_TRANSITION_CNT_SIZE 48u
#define LC_STATE_SIZE          40u

/* Sizes of constants used for deriving scrambling keys */
#define KEY_MGR_KEY_WIDTH 256u

static const char *DAI_STATE_NAMES[] = {
    /* clang-format off */
    OTP_NAME_ENTRY(OTP_DAI_RESET),
    OTP_NAME_ENTRY(OTP_DAI_INIT_OTP),
    OTP_NAME_ENTRY(OTP_DAI_INIT_PART),
    OTP_NAME_ENTRY(OTP_DAI_IDLE),
    OTP_NAME_ENTRY(OTP_DAI_ERROR),
    OTP_NAME_ENTRY(OTP_DAI_READ),
    OTP_NAME_ENTRY(OTP_DAI_READ_WAIT),
    OTP_NAME_ENTRY(OTP_DAI_DESCR),
    OTP_NAME_ENTRY(OTP_DAI_DESCR_WAIT),
    OTP_NAME_ENTRY(OTP_DAI_WRITE),
    OTP_NAME_ENTRY(OTP_DAI_WRITE_WAIT),
    OTP_NAME_ENTRY(OTP_DAI_SCR),
    OTP_NAME_ENTRY(OTP_DAI_SCR_WAIT),
    OTP_NAME_ENTRY(OTP_DAI_DIG_CLR),
    OTP_NAME_ENTRY(OTP_DAI_DIG_READ),
    OTP_NAME_ENTRY(OTP_DAI_DIG_READ_WAIT),
    OTP_NAME_ENTRY(OTP_DAI_DIG),
    OTP_NAME_ENTRY(OTP_DAI_DIG_PAD),
    OTP_NAME_ENTRY(OTP_DAI_DIG_FIN),
    OTP_NAME_ENTRY(OTP_DAI_DIG_WAIT),
    /* clang-format on */
};

static const char *LCI_STATE_NAMES[] = {
    /* clang-format off */
    OTP_NAME_ENTRY(OTP_LCI_RESET),
    OTP_NAME_ENTRY(OTP_LCI_IDLE),
    OTP_NAME_ENTRY(OTP_LCI_WRITE),
    OTP_NAME_ENTRY(OTP_LCI_WRITE_WAIT),
    OTP_NAME_ENTRY(OTP_LCI_ERROR),
    /* clang-format on */
};

static const char *ERR_CODE_NAMES[] = {
    /* clang-format off */
    OTP_NAME_ENTRY(OTP_NO_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_ECC_CORR_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_ECC_UNCORR_ERROR),
    OTP_NAME_ENTRY(OTP_MACRO_WRITE_BLANK_ERROR),
    OTP_NAME_ENTRY(OTP_ACCESS_ERROR),
    OTP_NAME_ENTRY(OTP_CHECK_FAIL_ERROR),
    OTP_NAME_ENTRY(OTP_FSM_STATE_ERROR),
    /* clang-format on */
};

#define BUF_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(BUF_STATE_NAMES) ? \
         BUF_STATE_NAMES[(_st_)] : \
         "?")
#define UNBUF_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(UNBUF_STATE_NAMES) ? \
         UNBUF_STATE_NAMES[(_st_)] : \
         "?")
#define DAI_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(DAI_STATE_NAMES) ? \
         DAI_STATE_NAMES[(_st_)] : \
         "?")
#define LCI_STATE_NAME(_st_) \
    ((unsigned)(_st_) < ARRAY_SIZE(LCI_STATE_NAMES) ? \
         LCI_STATE_NAMES[(_st_)] : \
         "?")
#define ERR_CODE_NAME(_err_) \
    (((unsigned)(_err_)) < ARRAY_SIZE(ERR_CODE_NAMES) ? \
         ERR_CODE_NAMES[(_err_)] : \
         "?")

/* @todo add assertion to validate those */
#define OTP_ENTRY_DAI(_s_)   ((_s_)->part_count + 0u)
#define OTP_ENTRY_KDI(_s_)   ((_s_)->part_count + 1u)
#define OTP_ENTRY_COUNT(_s_) ((_s_)->part_count + 2u)

#define DIRECT_ACCESS_REG(_s_, _reg_) \
    ((_s_)->regs[(_s_)->reg_offset.dai_base + (unsigned)(DA_REG_##_reg_)])
#define ERR_CODE_PART_REG(_s_, _pix_) \
    ((_s_)->regs[(_s_)->reg_offset.err_code_base + (_pix_)])

/*
 * See RTL files otp_ctrl/rtl/otp_ctrl_kdi.sv and otp_ctrl/rtl/otp_ctrl_pkg.sv
 * and OTP doc otp_ctrl/theory_of_operation.html#scrambling-key-derivation.
 */
#define SRAM_KEY_BYTES(_ic_)   ((_ic_)->key_seeds[OTP_KEY_SRAM].size)
#define SRAM_NONCE_BYTES(_ic_) ((_ic_)->key_seeds[OTP_KEY_SRAM].size)
#define OTBN_KEY_BYTES(_ic_)   ((_ic_)->key_seeds[OTP_KEY_OTBN].size)
#define OTBN_NONCE_BYTES(_ic_) (((_ic_)->key_seeds[OTP_KEY_OTBN].size) / 2u)

#define OT_OTP_SCRMBL_KEY_SIZE  16u
#define OT_OTP_SCRMBL_NONE_SIZE (OT_OTP_SCRMBL_KEY_SIZE)

/* Need 128 bits of entropy to compute each 64-bit key part */
#define OTP_ENTROPY_PRESENT_BYTES(_ic_) \
    (((((_ic_)->sram_key_req_slot_count) * SRAM_KEY_BYTES(_ic_)) + \
      (OTBN_KEY_BYTES(_ic_))) * \
     2u)
#define OTP_ENTROPY_NONCE_BYTES(_ic_) \
    (((_ic_)->sram_key_req_slot_count) * SRAM_NONCE_BYTES(_ic_) + \
     OTBN_NONCE_BYTES(_ic_))
#define OTP_ENTROPY_BUF_COUNT(_ic_) \
    ((OTP_ENTROPY_PRESENT_BYTES(_ic_) + OTP_ENTROPY_NONCE_BYTES(_ic_)) / 4u)

#ifdef OT_OTP_DEBUG
#define OT_OTP_HEXSTR_SIZE  256u
#define TRACE_OTP(msg, ...) qemu_log("%s: " msg "\n", __func__, ##__VA_ARGS__);
#define ot_otp_hexdump(_s_, _b_, _l_) \
    ot_common_lhexdump((const uint8_t *)_b_, _l_, false, (_s_)->hexstr, \
                       OT_OTP_HEXSTR_SIZE)
#else
#define TRACE_OTP(msg, ...)
#define ot_otp_hexdump(_s_, _b_, _l_)
#endif

#define DAI_CHANGE_STATE(_s_, _st_) \
    ot_otp_engine_dai_change_state_line(_s_, _st_, __LINE__)
#define LCI_CHANGE_STATE(_s_, _st_) \
    ot_otp_engine_lci_change_state_line(_s_, _st_, __LINE__)

static void ot_otp_engine_dai_set_error(OtOTPEngineState *s, OtOTPError err);
static void ot_otp_engine_dai_change_state_line(OtOTPEngineState *s,
                                                OtOTPDAIState state, int line);
static void ot_otp_engine_lci_change_state_line(OtOTPEngineState *s,
                                                OtOTPLCIState state, int line);

struct OtOTPScrmblKeyInit_ {
    uint8_t key[OT_OTP_SCRMBL_KEY_SIZE];
    uint8_t nonce[OT_OTP_SCRMBL_NONE_SIZE];
};

struct OtOTPKeyGen_ {
    QEMUBH *entropy_bh;
    OtPresentState *present;
    OtPrngState *prng;
    OtFifo32 entropy_buf;
    bool edn_sched;
};

static void ot_otp_engine_update_irqs(OtOTPEngineState *s)
{
    uint32_t levels = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->irqs[ix])) {
            trace_ot_otp_update_irq(s->ot_id, ibex_irq_get_level(&s->irqs[ix]),
                                    level);
        }
        ibex_irq_set(&s->irqs[ix], level);
    }
}

static void ot_otp_engine_update_alerts(OtOTPEngineState *s)
{
    uint32_t levels = s->regs[R_ALERT_TEST];

    levels |= s->alert_bm;

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        int level = (int)((levels >> ix) & 0x1u);
        if (level != ibex_irq_get_level(&s->alerts[ix])) {
            trace_ot_otp_update_alert(s->ot_id,
                                      ibex_irq_get_level(&s->alerts[ix]),
                                      level);
        }
        ibex_irq_set(&s->alerts[ix], level);
    }

    /* alert test is transient */
    if (s->regs[R_ALERT_TEST]) {
        s->regs[R_ALERT_TEST] = 0;

        levels = s->alert_bm;
        for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
            int level = (int)((levels >> ix) & 0x1u);
            if (level != ibex_irq_get_level(&s->alerts[ix])) {
                trace_ot_otp_update_alert(s->ot_id,
                                          ibex_irq_get_level(&s->alerts[ix]),
                                          level);
            }
            ibex_irq_set(&s->alerts[ix], level);
        }
    }
}

static const char *
ot_otp_engine_part_name(const OtOTPEngineState *s, unsigned part_ix)
{
    if (part_ix < s->part_count) {
        return s->part_descs[part_ix].name;
    }

    if (part_ix == OTP_ENTRY_DAI(s)) {
        return "DAI";
    }

    if (part_ix == OTP_ENTRY_KDI(s)) {
        return "KDI";
    }

    return "?";
}

static void ot_otp_engine_disable_all_partitions(OtOTPEngineState *s)
{
    DAI_CHANGE_STATE(s, OTP_DAI_ERROR);
    LCI_CHANGE_STATE(s, OTP_LCI_ERROR);

    for (unsigned pix = 0; pix < s->part_count; pix++) {
        OtOTPPartController *pctrl = &s->part_ctrls[pix];
        pctrl->failed = true;
    }
}

static void ot_otp_engine_set_error(OtOTPEngineState *s, unsigned part_ix,
                                    OtOTPError err)
{
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);
    g_assert(part_ix < OTP_ENTRY_COUNT(s));

    uint32_t errval = ((uint32_t)err) & ERR_CODE_MASK;
    if (errval || errval != ERR_CODE_PART_REG(s, part_ix)) {
        trace_ot_otp_set_error(s->ot_id, ot_otp_engine_part_name(s, part_ix),
                               part_ix, ERR_CODE_NAME(err), err);
    }
    ERR_CODE_PART_REG(s, part_ix) = errval;

    switch (err) {
    case OTP_MACRO_ERROR:
    case OTP_MACRO_ECC_UNCORR_ERROR:
        s->alert_bm |= ALERT_FATAL_MACRO_ERROR_MASK;
        ot_otp_engine_update_alerts(s);
        break;
    /* NOLINTNEXTLINE */
    case OTP_MACRO_ECC_CORR_ERROR:
        /*
         * "The corresponding controller automatically recovers from this error
         *  when issuing a new command."
         */
        break;
    case OTP_MACRO_WRITE_BLANK_ERROR:
        break;
    case OTP_ACCESS_ERROR:
        ic->update_status_error(OT_OTP_IMPL_IF(s), OT_OTP_STATUS_DAI, true);
        break;
    case OTP_CHECK_FAIL_ERROR:
    case OTP_FSM_STATE_ERROR:
        s->alert_bm |= ALERT_FATAL_CHECK_ERROR_MASK;
        ot_otp_engine_update_alerts(s);
        break;
    default:
        break;
    }

    if (s->alert_bm & ALERT_FATAL_CHECK_ERROR_MASK) {
        ot_otp_engine_disable_all_partitions(s);
        error_report("%s: %s: OTP disabled on fatal error", __func__, s->ot_id);
    }

    if (err != OTP_NO_ERROR) {
        s->regs[R_INTR_STATE] |= INTR_OTP_ERROR_MASK;
        ot_otp_engine_update_irqs(s);
    }
}

static int
ot_otp_engine_get_part_from_address(const OtOTPEngineState *s, hwaddr addr)
{
    for (unsigned part_ix = 0; part_ix < s->part_count; part_ix++) {
        const OtOTPPartDesc *part = &s->part_descs[part_ix];
        if ((addr >= part->offset) &&
            ((addr + sizeof(uint32_t)) <= (part->offset + part->size))) {
            trace_ot_otp_addr_to_part(s->ot_id, (uint32_t)addr,
                                      ot_otp_engine_part_name(s, part_ix),
                                      part_ix);
            return (int)part_ix;
        }
    }

    return -1;
}

static uint8_t ot_otp_engine_compute_ecc_u16(uint16_t data)
{
    uint32_t data_o = (uint32_t)data;

    data_o |= __builtin_parity(data_o & 0x00ad5bu) << 16u;
    data_o |= __builtin_parity(data_o & 0x00366du) << 17u;
    data_o |= __builtin_parity(data_o & 0x00c78eu) << 18u;
    data_o |= __builtin_parity(data_o & 0x0007f0u) << 19u;
    data_o |= __builtin_parity(data_o & 0x00f800u) << 20u;
    data_o |= __builtin_parity(data_o & 0x1fffffu) << 21u;

    return (uint8_t)(data_o >> 16u);
}

static uint16_t ot_otp_engine_compute_ecc_u32(uint32_t data)
{
    uint16_t data_lo = (uint16_t)(data & UINT16_MAX);
    uint16_t data_hi = (uint16_t)(data >> 16u);

    uint16_t ecc_lo = (uint16_t)ot_otp_engine_compute_ecc_u16(data_lo);
    uint16_t ecc_hi = (uint16_t)ot_otp_engine_compute_ecc_u16(data_hi);

    return (ecc_hi << 8u) | ecc_lo;
}

static uint32_t ot_otp_engine_compute_ecc_u64(uint64_t data)
{
    uint32_t data_lo = (uint32_t)(data & UINT32_MAX);
    uint32_t data_hi = (uint32_t)(data >> 32u);

    uint32_t ecc_lo = (uint32_t)ot_otp_engine_compute_ecc_u32(data_lo);
    uint32_t ecc_hi = (uint32_t)ot_otp_engine_compute_ecc_u32(data_hi);

    return (ecc_hi << 16u) | ecc_lo;
}

static uint32_t ot_otp_engine_verify_ecc_22_16_u16(
    const OtOTPEngineState *s, uint32_t data_i, unsigned *err_o)
{
    unsigned syndrome = 0u;

    syndrome |= __builtin_parity(data_i & 0x01ad5bu) << 0u;
    syndrome |= __builtin_parity(data_i & 0x02366du) << 1u;
    syndrome |= __builtin_parity(data_i & 0x04c78eu) << 2u;
    syndrome |= __builtin_parity(data_i & 0x0807f0u) << 3u;
    syndrome |= __builtin_parity(data_i & 0x10f800u) << 4u;
    syndrome |= __builtin_parity(data_i & 0x3fffffu) << 5u;

    unsigned err = (syndrome >> 5u) & 1u;
    if (!err && (syndrome & 0x1fu)) {
        err = 2u;
    }

    *err_o = err;

    if (!err) {
        return data_i & UINT16_MAX;
    }

    uint32_t data_o = 0;

#define OTP_ECC_RECOVER(_sy_, _di_, _ix_) \
    ((unsigned)((syndrome == (_sy_)) ^ (bool)((_di_) & (1u << (_ix_)))) \
     << (_ix_))

    data_o |= OTP_ECC_RECOVER(0x23u, data_i, 0u);
    data_o |= OTP_ECC_RECOVER(0x25u, data_i, 1u);
    data_o |= OTP_ECC_RECOVER(0x26u, data_i, 2u);
    data_o |= OTP_ECC_RECOVER(0x27u, data_i, 3u);
    data_o |= OTP_ECC_RECOVER(0x29u, data_i, 4u);
    data_o |= OTP_ECC_RECOVER(0x2au, data_i, 5u);
    data_o |= OTP_ECC_RECOVER(0x2bu, data_i, 6u);
    data_o |= OTP_ECC_RECOVER(0x2cu, data_i, 7u);
    data_o |= OTP_ECC_RECOVER(0x2du, data_i, 8u);
    data_o |= OTP_ECC_RECOVER(0x2eu, data_i, 9u);
    data_o |= OTP_ECC_RECOVER(0x2fu, data_i, 10u);
    data_o |= OTP_ECC_RECOVER(0x31u, data_i, 11u);
    data_o |= OTP_ECC_RECOVER(0x32u, data_i, 12u);
    data_o |= OTP_ECC_RECOVER(0x33u, data_i, 13u);
    data_o |= OTP_ECC_RECOVER(0x34u, data_i, 14u);
    data_o |= OTP_ECC_RECOVER(0x35u, data_i, 15u);

#undef OTP_ECC_RECOVER

    if (err > 1u) {
        trace_ot_otp_ecc_unrecoverable_error(s->ot_id, data_i & UINT16_MAX);
    } else {
        if ((data_i & UINT16_MAX) != data_o) {
            trace_ot_otp_ecc_recovered_error(s->ot_id, data_i & UINT16_MAX,
                                             data_o);
        } else {
            /* ECC bit is corrupted */
            trace_ot_otp_ecc_parity_error(s->ot_id, data_i & UINT16_MAX,
                                          data_i >> 16u);
        }
    }

    return data_o;
}

static uint32_t ot_otp_engine_verify_ecc(
    const OtOTPEngineState *s, uint32_t data, uint32_t ecc, unsigned *err)
{
    uint32_t data_lo_i, data_lo_o, data_hi_i, data_hi_o;
    unsigned err_lo, err_hi;

    data_lo_i = (data & 0xffffu) | ((ecc & 0xffu) << 16u);
    data_lo_o = ot_otp_engine_verify_ecc_22_16_u16(s, data_lo_i, &err_lo);

    data_hi_i = (data >> 16u) | (((ecc >> 8u) & 0xffu) << 16u);
    data_hi_o = ot_otp_engine_verify_ecc_22_16_u16(s, data_hi_i, &err_hi);

    *err |= err_lo | err_hi;

    return (data_hi_o << 16u) | data_lo_o;
}

static uint64_t ot_otp_engine_apply_digest_ecc(
    OtOTPEngineState *s, unsigned partition, uint64_t digest, uint32_t ecc)
{
    uint32_t dig_lo = (uint32_t)(digest & UINT32_MAX);
    uint32_t dig_hi = (uint32_t)(digest >> 32u);

    unsigned err = 0;
    dig_lo = ot_otp_engine_verify_ecc(s, dig_lo, ecc & 0xffffu, &err);
    dig_hi = ot_otp_engine_verify_ecc(s, dig_hi, ecc >> 16u, &err);
    digest = (((uint64_t)dig_hi) << 32u) | ((uint64_t)dig_lo);

    if (err) {
        OtOTPError otp_err =
            (err > 1) ? OTP_MACRO_ECC_UNCORR_ERROR : OTP_MACRO_ECC_CORR_ERROR;
        /*
         * Note: need to check if any caller could override the error/state
         * in this case
         */
        ot_otp_engine_set_error(s, partition, otp_err);
    }

    return digest;
}

static int ot_otp_engine_apply_ecc(OtOTPEngineState *s, unsigned part_ix)
{
    g_assert(ot_otp_engine_is_ecc_enabled(s));

    unsigned start = s->part_descs[part_ix].offset >> 2u;
    unsigned end = (ot_otp_engine_is_buffered(s, (int)part_ix) &&
                    ot_otp_engine_has_digest(s, part_ix)) ?
                       (unsigned)(s->part_descs[part_ix].digest_offset >> 2u) :
                       start + (unsigned)(s->part_descs[part_ix].size >> 2u);

    g_assert(start < end && (end / sizeof(uint32_t)) < s->otp->data_size);
    for (unsigned ix = start; ix < end; ix++) {
        unsigned err = 0;
        uint32_t *word = &s->otp->data[ix];
        uint16_t ecc = ((const uint16_t *)s->otp->ecc)[ix];
        *word = ot_otp_engine_verify_ecc(s, *word, (uint32_t)ecc, &err);
        if (err) {
            OtOTPError otp_err = (err > 1) ? OTP_MACRO_ECC_UNCORR_ERROR :
                                             OTP_MACRO_ECC_CORR_ERROR;
            /*
             *  Note: need to check if any caller could override the error/state
             * in this case
             */
            ot_otp_engine_set_error(s, part_ix, otp_err);
            if (err > 1) {
                trace_ot_otp_ecc_init_error(s->ot_id,
                                            ot_otp_engine_part_name(s, part_ix),
                                            part_ix, ix << 2u, *word, ecc);
                s->part_ctrls[part_ix].failed = true;
                return -1;
            }
        }
    }

    return 0;
}

static uint64_t
ot_otp_engine_get_part_digest(OtOTPEngineState *s, unsigned part_ix)
{
    g_assert(!ot_otp_engine_is_buffered(s, part_ix));

    uint16_t offset = s->part_descs[part_ix].digest_offset;

    if (offset == UINT16_MAX) {
        return 0u;
    }

    const uint8_t *data = (const uint8_t *)s->otp->data;
    uint64_t digest = ldq_le_p(data + offset);

    if (s->part_descs[part_ix].integrity && ot_otp_engine_is_ecc_enabled(s)) {
        unsigned waddr = offset >> 2u;
        unsigned ewaddr = waddr >> 1u;
        g_assert(ewaddr < s->otp->ecc_size);
        uint32_t ecc = s->otp->ecc[ewaddr];
        digest = ot_otp_engine_apply_digest_ecc(s, part_ix, digest, ecc);
    }

    return digest;
}

static uint32_t
ot_otp_engine_get_part_digest_reg(OtOTPEngineState *s, uint32_t offset)
{
    /*
     * Offset is the register offset from the first read 32-bit digest register.
     * All digests are 64-bits, which means each partition have two 32-bit
     * registers to expose their digest.
     *
     * Not all partitions have digests, which means the offset argument is the
     * relative partition offset for those partitions that features a digest, as
     * there is no defined digest access registers defined for partitions that
     * do not have a digest.
     *
     * Need to traverse the partition table to only account for those partitions
     * with a digest to match the proper offset.
     */

    /*
     * part_look_ix is the index of the partition in the contiguous array of
     * digest registers, which would be equivalent as the index of the partition
     * that would exist in a virtual partition-with-digest array.
     */
    unsigned part_look_ix = (unsigned)(offset / NUM_DIGEST_WORDS);
    /* whether to retrieve the top most 32-bit of the digest or not */
    bool hi = (bool)(offset & 0x1u);

    /* part_ix: the partition number in the global partition array */
    unsigned part_ix = 0;
    /* traverse the partition array and count each partition with a digest */
    for (unsigned part_dig_ix = 0; part_ix < s->part_count; part_ix++) {
        if (ot_otp_engine_has_digest(s, part_ix)) {
            /*
             * stop searching if we've found the part-with-digest defined from
             * the offset argument. Otherwise, increment the part-with-digest
             * index and continue.
             */
            if (part_dig_ix == part_look_ix) {
                break;
            }
            part_dig_ix++;
        }
    }

    /*
     * If the part_ix as reached the latest partition, there is something wrong
     * with the partition table or the register definitions, as it is assumed
     * that LifeCycle partition is the last partition.
     */
    g_assert(s->part_lc_num == s->part_count - 1u);
    g_assert(part_ix < s->part_count);

    const OtOTPPartController *pctrl = &s->part_ctrls[part_ix];
    uint64_t digest = pctrl->digest;

    if (hi) {
        digest >>= 32u;
    }

    return (uint32_t)digest;
}

static uint32_t
ot_otp_engine_get_sw_readlock(const OtOTPEngineState *s, unsigned rdlk_ix)
{
    uint32_t reg = s->reg_offset.read_lock_base + rdlk_ix;

    return (bool)SHARED_FIELD_EX32(s->regs[reg], READ_LOCK);
}

static bool ot_otp_engine_is_readable(const OtOTPEngineState *s,
                                      unsigned part_ix)
{
    g_assert(part_ix < s->part_count);

    const OtOTPPartDesc *pdesc = &s->part_descs[part_ix];
    const OtOTPPartController *pctrl = &s->part_ctrls[part_ix];

    if (pdesc->secret) {
        /* secret partitions are only readable if digest is not yet set. */
        return pctrl->digest == 0u;
    }

    if (!pdesc->read_lock_csr) {
        if (!pdesc->read_lock) {
            /* read lock is not supported for this partition */
            return true;
        }

        /* hw read lock, not locked */
        return !pctrl->read_lock;
    }

    unsigned roffset = 0;
    unsigned pix;
    for (pix = 0; pix < s->part_count; pix++) {
        if (pix == part_ix) {
            break;
        }
        if (pdesc->read_lock_csr) {
            roffset++;
        }
    }
    /*
     * know for sure last partition is the life cycle one, which never
     * support a read_lock_csr. Ideally this g_assert should be a
     * static_assert, but C being C, constants are not defined as such
     * at build time...
     */
    g_assert(!s->part_descs[s->part_lc_num].read_lock_csr);

    /*
     * If the previous loop reached the last partition, something
     * seriously wrong occurred. Use this feature as a sanity check
     */
    g_assert(pix < s->part_lc_num);

    /*
     * now that the count of read_lock_csr is known, use it to access
     * the register for the selected partition
     */
    return ot_otp_engine_get_sw_readlock(s, roffset);
}

static void ot_otp_engine_dai_change_state_line(OtOTPEngineState *s,
                                                OtOTPDAIState state, int line)
{
    trace_ot_otp_dai_change_state(s->ot_id, line, DAI_STATE_NAME(s->dai->state),
                                  s->dai->state, DAI_STATE_NAME(state), state);

    s->dai->state = state;
}

static void ot_otp_engine_lci_change_state_line(OtOTPEngineState *s,
                                                OtOTPLCIState state, int line)
{
    trace_ot_otp_lci_change_state(s->ot_id, line, LCI_STATE_NAME(s->lci->state),
                                  s->lci->state, LCI_STATE_NAME(state), state);

    s->lci->state = state;
}

static void ot_otp_engine_lc_broadcast_recv(void *opaque, int n, int level)
{
    OtOTPEngineState *s = opaque;
    OtOTPLcBroadcast *bcast = &s->lc_broadcast;

    g_assert((unsigned)n < OT_OTP_LC_BROADCAST_COUNT);

    uint16_t bit = 1u << (unsigned)n;
    bcast->signal |= bit;
    /*
     * as these signals are only used to change permissions, it is valid to
     * override a signal value that has not been processed yet
     */
    if (level) {
        bcast->level |= bit;
    } else {
        bcast->level &= ~bit;
    }

    /* use a BH to decouple IRQ signaling from actual handling */
    qemu_bh_schedule(s->lc_broadcast.bh);
}

static void ot_otp_engine_lc_broadcast_bh(void *opaque)
{
    OtOTPEngineState *s = opaque;
    OtOTPLcBroadcast *bcast = &s->lc_broadcast;

    /* handle all flagged signals */
    while (bcast->signal) {
        /* pick and clear */
        unsigned sig = ctz16(bcast->signal);
        uint16_t bit = 1u << (unsigned)sig;
        bcast->signal &= ~bit;
        bcast->current_level =
            (bcast->current_level & ~bit) | (bcast->level & bit);
        bool level = (bool)(bcast->current_level & bit);

        trace_ot_otp_lc_broadcast(s->ot_id, sig, level);

        switch ((int)sig) {
        case OT_OTP_LC_DFT_EN:
            qemu_log_mask(LOG_UNIMP, "%s: %s: DFT feature not supported\n",
                          __func__, s->ot_id);
            break;
        case OT_OTP_LC_ESCALATE_EN:
            if (level) {
                DAI_CHANGE_STATE(s, OTP_DAI_ERROR);
                LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
                /* @todo manage other FSMs */
                qemu_log_mask(LOG_UNIMP,
                              "%s: %s: ESCALATE partially implemented\n",
                              __func__, s->ot_id);
                if (s->fatal_escalate) {
                    error_setg(&error_fatal, "%s: OTP LC escalate", s->ot_id);
                }
            }
            break;
        case OT_OTP_LC_CHECK_BYP_EN:
            qemu_log_mask(LOG_UNIMP, "%s: %s: bypass is ignored\n", __func__,
                          s->ot_id);
            break;
        case OT_OTP_LC_CREATOR_SEED_SW_RW_EN:
            for (unsigned ix = 0; ix < s->part_count; ix++) {
                if (s->part_descs[ix].iskeymgr_creator) {
                    s->part_ctrls[ix].read_lock = !level;
                    s->part_ctrls[ix].write_lock = !level;
                }
            }
            break;
        case OT_OTP_LC_OWNER_SEED_SW_RW_EN:
            for (unsigned ix = 0; ix < s->part_count; ix++) {
                if (s->part_descs[ix].iskeymgr_owner) {
                    s->part_ctrls[ix].read_lock = !level;
                    s->part_ctrls[ix].write_lock = !level;
                }
            }
            break;
        case OT_OTP_LC_SEED_HW_RD_EN:
            /* nothing to do here, SEED_HW_RD_EN flag is in current_level */
            break;
        default:
            error_setg(&error_fatal, "%s: %s: unexpected LC broadcast %d\n",
                       __func__, s->ot_id, sig);
            g_assert_not_reached();
            break;
        }
    }
}

static uint64_t ot_otp_engine_compute_partition_digest(
    OtOTPEngineState *s, const uint8_t *base, unsigned size)
{
    OtPresentState *ps = ot_present_new();

    g_assert((size & (sizeof(uint64_t) - 1u)) == 0);

    uint8_t buf[sizeof(uint64_t) * 2u];
    uint64_t state = s->digest_iv;
    uint64_t out;
    for (unsigned off = 0; off < size; off += sizeof(buf)) {
        memcpy(buf, base + off, sizeof(uint64_t));
        if (off + sizeof(uint64_t) != size) {
            memcpy(&buf[sizeof(uint64_t)], base + off + sizeof(uint64_t),
                   sizeof(uint64_t));
        } else {
            /* special case, duplicate last block if block number is odd */
            memcpy(&buf[sizeof(uint64_t)], base + off, sizeof(uint64_t));
        }

        ot_present_init(ps, buf);
        ot_present_encrypt(ps, state, &out);
        state ^= out;
    }

    ot_present_init(ps, s->digest_const);
    ot_present_encrypt(ps, state, &out);
    state ^= out;

    ot_present_free(ps);

    return state;
}

static uint64_t
ot_otp_engine_load_partition_digest(OtOTPEngineState *s, unsigned partition)
{
    unsigned digoff = (unsigned)s->part_descs[partition].digest_offset;

    if ((digoff + sizeof(uint64_t)) > s->otp->data_size) {
        error_setg(&error_fatal, "%s: partition located outside storage?",
                   s->ot_id);
        /* linter doest not know the above call never returns */
        return 0u;
    }

    const uint8_t *data = (const uint8_t *)s->otp->data;
    uint64_t digest = ldq_le_p(data + digoff);

    if (ot_otp_engine_is_ecc_enabled(s)) {
        unsigned ewaddr = (digoff >> 3u);
        g_assert(ewaddr < s->otp->ecc_size);
        uint32_t ecc = s->otp->ecc[ewaddr];
        digest = ot_otp_engine_apply_digest_ecc(s, partition, digest, ecc);
    }

    return digest;
}

static void
ot_otp_engine_unscramble_partition(OtOTPEngineState *s, unsigned part_ix)
{
    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];

    unsigned offset = (unsigned)s->part_descs[part_ix].offset;
    unsigned part_size = ot_otp_engine_part_data_byte_size(s, part_ix);

    /* part_size should be a multiple of PRESENT block size */
    g_assert((part_size & (sizeof(uint64_t) - 1u)) == 0u);
    unsigned dword_count = part_size / sizeof(uint64_t);

    const uint8_t *base = (const uint8_t *)s->otp->data;
    base += offset;

    /* source address should be aligned to 64-bit boundary */
    g_assert(((uintptr_t)base & (sizeof(uint64_t) - 1u)) == 0u);
    const uint64_t *scrambled = (const uint64_t *)base;

    /* destination address should be aligned to 64-bit boundary */
    g_assert(pctrl->buffer.data != NULL);
    uint64_t *clear = (uint64_t *)pctrl->buffer.data;

    const uint8_t *scrambling_key = s->otp_scramble_keys[part_ix];
    g_assert(scrambling_key);

    OtPresentState *ps = ot_present_new();
    ot_present_init(ps, scrambling_key);

    trace_ot_otp_unscramble_partition(s->ot_id,
                                      ot_otp_engine_part_name(s, part_ix),
                                      part_ix, part_size);
    /* neither the digest block nor the zeroizable block are scrambled */
    for (unsigned dix = 0u; dix < dword_count; dix++) {
        ot_present_decrypt(ps, scrambled[dix], &clear[dix]);
    }

    ot_present_free(ps);
}

static void
ot_otp_engine_bufferize_partition(OtOTPEngineState *s, unsigned part_ix)
{
    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];

    g_assert(pctrl->buffer.data != NULL);

    if (s->part_descs[part_ix].hw_digest) {
        pctrl->digest = ot_otp_engine_load_partition_digest(s, part_ix);
    } else {
        pctrl->digest = 0;
    }

    if (s->part_descs[part_ix].secret) {
        /* secret partitions need to be unscrambled */
        if (s->blk) {
            /*
             * nothing to unscramble if no OTP data is loaded
             * scrambling keys in this case may not be known
             */
            ot_otp_engine_unscramble_partition(s, part_ix);
        }
    } else {
        unsigned offset = (unsigned)s->part_descs[part_ix].offset;
        unsigned part_size = ot_otp_engine_part_data_byte_size(s, part_ix);

        const uint8_t *base = (const uint8_t *)s->otp->data;
        base += offset;

        memcpy(pctrl->buffer.data, base, part_size);
    }
}

static void ot_otp_engine_check_buffered_partition_integrity(
    OtOTPEngineState *s, unsigned part_ix)
{
    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];

    if (pctrl->digest == 0) {
        trace_ot_otp_skip_digest(s->ot_id, ot_otp_engine_part_name(s, part_ix),
                                 part_ix);
        pctrl->locked = false;
        return;
    }

    pctrl->locked = true;

    /*
     * digests are always calculated over the original data (scrambled or not)
     */
    const uint8_t *part_data = ((const uint8_t *)s->otp->data) +
                               ot_otp_engine_part_data_offset(s, part_ix);
    unsigned part_size = ot_otp_engine_part_data_byte_size(s, part_ix);

    uint64_t digest =
        ot_otp_engine_compute_partition_digest(s, part_data, part_size);

    if (digest != pctrl->digest) {
        trace_ot_otp_mismatch_digest(s->ot_id,
                                     ot_otp_engine_part_name(s, part_ix),
                                     part_ix, digest, pctrl->digest);

        TRACE_OTP("compute digest of %s: %016" PRIx64 " from %s\n",
                  ot_otp_engine_part_name(s, part_ix), digest,
                  ot_otp_hexdump(s, part_data, part_size));

        pctrl->failed = true;
        /* this is a fatal error */
        ot_otp_engine_set_error(s, part_ix, OTP_CHECK_FAIL_ERROR);
        /* @todo revert buffered part to default */
    } else {
        trace_ot_otp_integrity_report(s->ot_id,
                                      ot_otp_engine_part_name(s, part_ix),
                                      part_ix, "digest OK");
    }
}

static bool ot_otp_engine_is_backend_writable(OtOTPEngineState *s)
{
    return (s->blk != NULL) && blk_is_writable(s->blk);
}

static inline int ot_otp_engine_write_backend(
    OtOTPEngineState *s, const void *buffer, unsigned offset, size_t size)
{
    /*
     * the blk_pwrite API is awful, isolate it so that linter exceptions are
     * are not repeated over and over
     */
    g_assert(offset + size <= s->otp->size);

    /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
    return blk_pwrite(s->blk, (int64_t)(intptr_t)offset, (int64_t)size, buffer,
                      /* a bitfield of enum is not an enum item */
                      (BdrvRequestFlags)0);
    /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
}

static void ot_otp_engine_dai_init(OtOTPEngineState *s)
{
    DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
}

static void ot_otp_engine_dai_set_error(OtOTPEngineState *s, OtOTPError err)
{
    ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s), err);

    switch (err) {
    case OTP_FSM_STATE_ERROR:
    case OTP_MACRO_ERROR:
    case OTP_MACRO_ECC_UNCORR_ERROR:
        DAI_CHANGE_STATE(s, OTP_DAI_ERROR);
        break;
    default:
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
        break;
    }
}

static void ot_otp_engine_dai_clear_error(OtOTPEngineState *s)
{
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);

    ic->update_status_error(OT_OTP_IMPL_IF(s), OT_OTP_STATUS_DAI, false);

    ERR_CODE_PART_REG(s, OTP_ENTRY_DAI(s)) = 0u;
}

static void ot_otp_engine_dai_read(OtOTPEngineState *s)
{
    if (ot_otp_engine_dai_is_busy(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: DAI controller busy: %s\n",
                      __func__, s->ot_id, DAI_STATE_NAME(s->dai->state));
        return;
    }

    ot_otp_engine_dai_clear_error(s);

    DAI_CHANGE_STATE(s, OTP_DAI_READ);

    unsigned address = DIRECT_ACCESS_REG(s, ADDRESS);

    int partition = ot_otp_engine_get_part_from_address(s, address);

    if (partition < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: invalid partition address 0x%x\n", __func__,
                      s->ot_id, address);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    unsigned part_ix = (unsigned)partition;
    if (part_ix >= s->part_lc_num) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: %s: life cycle partition cannot be accessed from DAI\n",
            __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    const OtOTPPartController *pctrl = &s->part_ctrls[part_ix];
    if (pctrl->failed) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: partition %s is disabled\n",
                      __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix));
        return;
    }

    bool is_readable = ot_otp_engine_is_readable(s, part_ix);
    bool is_buffered = ot_otp_engine_is_buffered(s, part_ix);
    bool is_secret = ot_otp_engine_is_secret(s, part_ix);
    bool is_digest = ot_otp_engine_is_part_digest_offset(s, part_ix, address);
    bool is_zer = ot_otp_engine_is_part_zer_offset(s, part_ix, address);

    /* in all partitions, the digest and zer fields are always readable. */
    if (!is_digest && !is_zer && !is_readable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: partition %s @ 0x%04x not readable\n", __func__,
                      s->ot_id, ot_otp_engine_part_name(s, part_ix), address);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    unsigned part_offset = address - ot_otp_engine_part_data_offset(s, part_ix);
    unsigned part_waddr = part_offset >> 2u;
    bool do_ecc =
        s->part_descs[part_ix].integrity && ot_otp_engine_is_ecc_enabled(s);

    DAI_CHANGE_STATE(s, OTP_DAI_READ_WAIT);

    uint32_t data_lo, data_hi;
    unsigned err = 0;
    unsigned cell_count = sizeof(uint32_t) + (do_ecc ? sizeof(uint16_t) : 0);

    const uint32_t *data = (const uint32_t *)s->otp->data;
    /* parenthesis inform the C linter sizeof() call is valid with 'data */
    data += (ot_otp_engine_part_data_offset(s, part_ix) / sizeof(uint32_t));

    bool is_wide = is_digest || is_zer || is_secret;
    if (is_wide) {
        /* 64-bit requests */
        part_waddr &= ~0b1u;

        g_assert((part_waddr + 1u) * sizeof(uint32_t) <
                 s->part_descs[part_ix].size);

        data_lo = data[part_waddr];
        data_hi = data[part_waddr + 1u];

        if (do_ecc) {
            unsigned ewaddr = address >> 3u;
            g_assert(ewaddr < s->otp->ecc_size);
            uint32_t ecc = s->otp->ecc[ewaddr];
            if (ot_otp_engine_is_ecc_enabled(s)) {
                data_lo =
                    ot_otp_engine_verify_ecc(s, data_lo, ecc & 0xffffu, &err);
                data_hi =
                    ot_otp_engine_verify_ecc(s, data_hi, ecc >> 16u, &err);
            }
        }

        cell_count *= 2u;
    } else {
        /* 32-bit request */
        g_assert(part_waddr * sizeof(uint32_t) < s->part_descs[part_ix].size);

        data_lo = data[part_waddr];
        data_hi = 0u;

        if (do_ecc) {
            unsigned ewaddr = address >> 3u;
            g_assert(ewaddr < s->otp->ecc_size);
            uint32_t ecc = s->otp->ecc[ewaddr];
            if ((address >> 2u) & 1u) {
                ecc >>= 16u;
            }
            if (ot_otp_engine_is_ecc_enabled(s)) {
                data_lo =
                    ot_otp_engine_verify_ecc(s, data_lo, ecc & 0xffffu, &err);
            }
            cell_count = 4u + 2u;
        } else {
            cell_count = 4u;
        }
    }

    if (is_secret && !(is_zer || is_digest)) {
        /*
         * if the partition is a secret partition, OTP storage is scrambled
         * except the digest and the zeroification fields
         */
        const uint8_t *scrambling_key = s->otp_scramble_keys[part_ix];
        g_assert(scrambling_key);
        uint64_t tmp_data = ((uint64_t)data_hi << 32u) | data_lo;
        OtPresentState *ps = ot_present_new();
        ot_present_init(ps, scrambling_key);
        ot_present_decrypt(ps, tmp_data, &tmp_data);
        ot_present_free(ps);
        data_lo = (uint32_t)tmp_data;
        data_hi = (uint32_t)(tmp_data >> 32u);
    }

    DIRECT_ACCESS_REG(s, RDATA_0) = data_lo;
    DIRECT_ACCESS_REG(s, RDATA_1) = data_hi;

    if (err) {
        OtOTPError otp_err =
            (err > 1) ? OTP_MACRO_ECC_UNCORR_ERROR : OTP_MACRO_ECC_CORR_ERROR;
        ot_otp_engine_dai_set_error(s, otp_err);
        return;
    }

    s->dai->partition = partition;

    if (!is_buffered) {
        /* fake slow access to OTP cell */
        unsigned access_time = s->be_chars.timings.read_ns * cell_count;
        timer_mod(s->dai->delay,
                  qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + access_time);
    } else {
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
    }
}

static int ot_otp_engine_dai_write_u64(OtOTPEngineState *s, unsigned address)
{
    unsigned waddr = (address / sizeof(uint32_t)) & ~1u;
    uint32_t *dst = &s->otp->data[waddr];

    uint32_t dst_lo = dst[0u];
    uint32_t dst_hi = dst[1u];

    uint32_t lo = DIRECT_ACCESS_REG(s, WDATA_0);
    uint32_t hi = DIRECT_ACCESS_REG(s, WDATA_1);

    unsigned part_ix = (unsigned)s->dai->partition;
    bool is_secret = ot_otp_engine_is_secret(s, part_ix);
    bool is_zer = ot_otp_engine_is_part_zer_offset(s, part_ix, address);

    if (is_secret && !is_zer) {
        const uint8_t *scrambling_key = s->otp_scramble_keys[part_ix];
        uint64_t data = ((uint64_t)hi << 32u) | lo;
        g_assert(scrambling_key);
        OtPresentState *ps = ot_present_new();
        ot_present_init(ps, scrambling_key);
        ot_present_encrypt(ps, data, &data);
        lo = (uint32_t)data;
        hi = (uint32_t)(data >> 32u);
    }

    if ((dst_lo & ~lo) || (dst_hi & ~hi)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Cannot clear OTP bits\n",
                      __func__, s->ot_id);
        ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s),
                                OTP_MACRO_WRITE_BLANK_ERROR);
    }

    dst[0u] |= lo;
    dst[1u] |= hi;

    uintptr_t offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
    if (ot_otp_engine_write_backend(s, dst,
                                    (unsigned)(offset +
                                               waddr * sizeof(uint32_t)),
                                    sizeof(uint64_t))) {
        error_report("%s: %s: cannot update OTP backend", __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
        return -1;
    }

    if (ot_otp_engine_is_ecc_enabled(s)) {
        unsigned ewaddr = waddr >> 1u;
        g_assert(ewaddr < s->otp->ecc_size);
        uint32_t *edst = &s->otp->ecc[ewaddr];

        uint32_t ecc_lo = (uint32_t)ot_otp_engine_compute_ecc_u32(lo);
        uint32_t ecc_hi = (uint32_t)ot_otp_engine_compute_ecc_u32(hi);
        uint32_t ecc = (ecc_hi << 16u) | ecc_lo;

        if (*edst & ~ecc) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Cannot clear OTP ECC bits\n", __func__,
                          s->ot_id);
            ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s),
                                    OTP_MACRO_WRITE_BLANK_ERROR);
        }
        *edst |= ecc;

        offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
        if (ot_otp_engine_write_backend(s, edst,
                                        (unsigned)(offset + (waddr << 1u)),
                                        sizeof(uint32_t))) {
            error_report("%s: %s: cannot update OTP backend", __func__,
                         s->ot_id);
            ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
            return -1;
        }

        trace_ot_otp_dai_new_dword_ecc(s->ot_id,
                                       ot_otp_engine_part_name(s,
                                                               (unsigned)s->dai
                                                                   ->partition),
                                       s->dai->partition, *dst, *edst);
    }

    return 0;
}

static int ot_otp_engine_dai_write_u32(OtOTPEngineState *s, unsigned address)
{
    unsigned waddr = address / sizeof(uint32_t);
    uint32_t *dst = &s->otp->data[waddr];
    uint32_t data = DIRECT_ACCESS_REG(s, WDATA_0);

    if (*dst & ~data) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: cannot clear OTP bits\n",
                      __func__, s->ot_id);
        ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s),
                                OTP_MACRO_WRITE_BLANK_ERROR);
    }

    *dst |= data;

    uintptr_t offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
    if (ot_otp_engine_write_backend(s, dst,
                                    (unsigned)(offset +
                                               waddr * sizeof(uint32_t)),
                                    sizeof(uint32_t))) {
        error_report("%s: %s: cannot update OTP backend", __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
        return -1;
    }

    if (ot_otp_engine_is_ecc_enabled(s)) {
        g_assert((waddr >> 1u) < s->otp->ecc_size);
        uint16_t *edst = &((uint16_t *)s->otp->ecc)[waddr];
        uint16_t ecc = ot_otp_engine_compute_ecc_u32(*dst);

        if (*edst & ~ecc) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: cannot clear OTP ECC bits\n", __func__,
                          s->ot_id);
            ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s),
                                    OTP_MACRO_WRITE_BLANK_ERROR);
        }
        *edst |= ecc;

        offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
        if (ot_otp_engine_write_backend(s, edst,
                                        (unsigned)(offset + (address >> 1u)),
                                        sizeof(uint16_t))) {
            error_report("%s: %s: cannot update OTP backend", __func__,
                         s->ot_id);
            ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
            return -1;
        }

        trace_ot_otp_dai_new_word_ecc(s->ot_id,
                                      ot_otp_engine_part_name(s,
                                                              (unsigned)s->dai
                                                                  ->partition),
                                      s->dai->partition, *dst, *edst);
    }

    return 0;
}

static void ot_otp_engine_dai_write(OtOTPEngineState *s)
{
    if (ot_otp_engine_dai_is_busy(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: DAI controller busy: %s\n",
                      __func__, s->ot_id, DAI_STATE_NAME(s->dai->state));
        return;
    }

    if (!ot_otp_engine_is_backend_writable(s)) {
        /* OTP backend missing or read-only; reject any write request */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: OTP backend file is missing or R/O\n", __func__,
                      s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE);

    ot_otp_engine_dai_clear_error(s);

    unsigned address = DIRECT_ACCESS_REG(s, ADDRESS);

    int partition = ot_otp_engine_get_part_from_address(s, address);

    if (partition < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: invalid partition address 0x%x\n", __func__,
                      s->ot_id, address);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    unsigned part_ix = (unsigned)partition;

    if (part_ix >= s->part_lc_num) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: %s: Life cycle partition cannot be accessed from DAI\n",
            __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];

    if (pctrl->failed) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: partition %s is disabled\n",
                      __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix));
        return;
    }

    if (pctrl->locked) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: partition %s (%u) is locked\n",
                      __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix),
                      part_ix);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (pctrl->write_lock) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: artition %s (%u) is write locked\n", __func__,
                      s->ot_id, ot_otp_engine_part_name(s, part_ix), part_ix);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    bool is_digest = ot_otp_engine_is_part_digest_offset(s, part_ix, address);

    if (is_digest) {
        if (s->part_descs[part_ix].hw_digest) {
            /* should have been a Digest command, not a Write command */
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: %s: partition %s (%u) HW digest cannot be directly "
                "written\n",
                __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix),
                part_ix);
            ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
            return;
        }
    }

    s->dai->partition = partition;

    bool do_ecc = ot_otp_engine_is_ecc_enabled(s);
    unsigned cell_count = sizeof(uint32_t);

    bool is_secret = ot_otp_engine_is_secret(s, part_ix);
    bool is_zer = ot_otp_engine_is_part_zer_offset(s, part_ix, address);

    bool is_wide = is_secret || is_digest || is_zer;
    if (is_wide) {
        if (ot_otp_engine_dai_write_u64(s, address)) {
            return;
        }
        cell_count *= 2u;
    } else {
        if (ot_otp_engine_dai_write_u32(s, address)) {
            return;
        }
    }

    if (do_ecc) {
        cell_count += cell_count / 2u;
    };

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE_WAIT);

    /* fake slow update of OTP cell */
    unsigned update_time = s->be_chars.timings.write_ns * cell_count;
    timer_mod(s->dai->delay, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + update_time);
}

static void ot_otp_engine_dai_digest(OtOTPEngineState *s)
{
    if (ot_otp_engine_dai_is_busy(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: DAI controller busy: %s\n",
                      __func__, s->ot_id, DAI_STATE_NAME(s->dai->state));
        return;
    }

    if (!ot_otp_engine_is_backend_writable(s)) {
        /* OTP backend missing or read-only; reject any write request */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: OTP backend file is missing or R/O\n", __func__,
                      s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    DAI_CHANGE_STATE(s, OTP_DAI_DIG_CLR);

    ot_otp_engine_dai_clear_error(s);

    unsigned address = DIRECT_ACCESS_REG(s, ADDRESS);

    int partition = ot_otp_engine_get_part_from_address(s, address);

    if (partition < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid partition address 0x%x\n", __func__,
                      s->ot_id, address);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    unsigned part_ix = (unsigned)partition;

    if (part_ix >= s->part_lc_num) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "%s: %s: Life cycle partition cannot be accessed from DAI\n",
            __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (!s->part_descs[part_ix].hw_digest) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid partition, no HW digest on %s (#%u)\n",
                      __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix),
                      part_ix);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];

    if (pctrl->failed) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: partition %s is disabled\n",
                      __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix));
        return;
    }

    if (pctrl->locked) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Partition %s (%u) is locked\n",
                      __func__, s->ot_id, ot_otp_engine_part_name(s, part_ix),
                      part_ix);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    if (pctrl->write_lock) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Partition %s (%u) is write locked\n", __func__,
                      s->ot_id, ot_otp_engine_part_name(s, part_ix), part_ix);
        ot_otp_engine_dai_set_error(s, OTP_ACCESS_ERROR);
        return;
    }

    DAI_CHANGE_STATE(s, OTP_DAI_DIG_READ);

    const uint8_t *data = ((const uint8_t *)s->otp->data) +
                          ot_otp_engine_part_data_offset(s, part_ix);
    unsigned part_size = ot_otp_engine_part_data_byte_size(s, part_ix);

    DAI_CHANGE_STATE(s, OTP_DAI_DIG);

    pctrl->buffer.next_digest =
        ot_otp_engine_compute_partition_digest(s, data, part_size);
    s->dai->partition = partition;

    TRACE_OTP("%s: %s: next digest %016" PRIx64 " from %s\n", __func__,
              s->ot_id, pctrl->buffer.next_digest,
              ot_otp_hexdump(s, data, part_size));

    DAI_CHANGE_STATE(s, OTP_DAI_DIG_WAIT);

    /* fake slow update of OTP cell */
    timer_mod(s->dai->delay,
              qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + DAI_DIGEST_DELAY_NS);
}

static void ot_otp_engine_dai_write_digest(void *opaque)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(opaque);

    g_assert((s->dai->partition >= 0) && (s->dai->partition < s->part_count));

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE);

    unsigned part_ix = (unsigned)s->dai->partition;
    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];
    unsigned address = s->part_descs[part_ix].digest_offset;
    unsigned dwaddr = address / sizeof(uint64_t);
    uint64_t *dst = &((uint64_t *)s->otp->data)[dwaddr];
    uint64_t data = pctrl->buffer.next_digest;
    pctrl->buffer.next_digest = 0;

    if (*dst & ~data) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: cannot clear OTP data bits\n",
                      __func__, s->ot_id);
        ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s),
                                OTP_MACRO_WRITE_BLANK_ERROR);
    }
    *dst |= data;

    uintptr_t offset;
    offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
    if (ot_otp_engine_write_backend(s, dst, (unsigned)(offset + address),
                                    sizeof(uint64_t))) {
        error_report("%s: %s: cannot update OTP backend", __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    uint32_t ecc = ot_otp_engine_compute_ecc_u64(data);

    /* dwaddr is 64-bit based, convert it to 32-bit base for ECC */
    unsigned ewaddr = (dwaddr << 1u) / s->otp->ecc_granule;
    g_assert(ewaddr < s->otp->ecc_size);
    uint32_t *edst = &s->otp->ecc[ewaddr];

    if (*edst & ~ecc) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: cannot clear OTP ECC bits\n",
                      __func__, s->ot_id);
        ot_otp_engine_set_error(s, OTP_ENTRY_DAI(s),
                                OTP_MACRO_WRITE_BLANK_ERROR);
    }
    *edst |= ecc;

    offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
    if (ot_otp_engine_write_backend(s, edst,
                                    (unsigned)(offset + (ewaddr << 2u)),
                                    sizeof(uint32_t))) {
        error_report("%s: %s: cannot update OTP backend", __func__, s->ot_id);
        ot_otp_engine_dai_set_error(s, OTP_MACRO_ERROR);
        return;
    }

    trace_ot_otp_dai_new_digest_ecc(s->ot_id,
                                    ot_otp_engine_part_name(s, part_ix),
                                    part_ix, *dst, *edst);

    DAI_CHANGE_STATE(s, OTP_DAI_WRITE_WAIT);

    /* fake slow update of OTP cell */
    unsigned cell_count = sizeof(uint64_t) + sizeof(uint32_t);
    unsigned update_time = s->be_chars.timings.write_ns * cell_count;
    timer_mod(s->dai->delay, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) + update_time);
}

static void ot_otp_engine_dai_complete(void *opaque)
{
    OtOTPEngineState *s = opaque;

    switch (s->dai->state) {
    case OTP_DAI_READ_WAIT:
        g_assert(s->dai->partition >= 0);
        trace_ot_otp_dai_read(s->ot_id,
                              ot_otp_engine_part_name(s, (unsigned)
                                                             s->dai->partition),
                              (unsigned)s->dai->partition,
                              DIRECT_ACCESS_REG(s, RDATA_0),
                              DIRECT_ACCESS_REG(s, RDATA_1));
        s->dai->partition = -1;
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
        break;
    case OTP_DAI_WRITE_WAIT:
        g_assert(s->dai->partition >= 0);
        s->regs[R_INTR_STATE] |= INTR_OTP_OPERATION_DONE_MASK;
        s->dai->partition = -1;
        DAI_CHANGE_STATE(s, OTP_DAI_IDLE);
        break;
    case OTP_DAI_DIG_WAIT:
        g_assert(s->dai->partition >= 0);
        qemu_bh_schedule(s->dai->digest_bh);
        break;
    case OTP_DAI_ERROR:
        break;
    default:
        g_assert_not_reached();
        break;
    };
}

static void ot_otp_engine_lci_init(OtOTPEngineState *s)
{
    LCI_CHANGE_STATE(s, OTP_LCI_IDLE);
}

static const OtOTPHWCfg *ot_otp_engine_get_hw_cfg(const OtOTPIf *dev)
{
    const OtOTPEngineState *s = OT_OTP_ENGINE(dev);

    return (const OtOTPHWCfg *)s->hw_cfg;
}

static void ot_otp_engine_request_entropy_bh(void *opaque)
{
    OtOTPEngineState *s = opaque;

    /*
     * Use a BH as entropy should be filled in as soon as possible after reset.
     * However, as the EDN / OTP reset order is unknown, this initial request
     * can only be performed once the reset sequence is over.
     */
    if (!s->keygen->edn_sched) {
        int rc = ot_edn_request_entropy(s->edn, s->edn_ep);
        g_assert(rc == 0);
        s->keygen->edn_sched = true;
    }
}

static void
ot_otp_engine_keygen_push_entropy(void *opaque, uint32_t bits, bool fips)
{
    OtOTPEngineState *s = opaque;
    (void)fips;

    s->keygen->edn_sched = false;

    if (!ot_fifo32_is_full(&s->keygen->entropy_buf)) {
        ot_fifo32_push(&s->keygen->entropy_buf, bits);
    }

    bool resched = !ot_fifo32_is_full(&s->keygen->entropy_buf);

    trace_ot_otp_keygen_entropy(s->ot_id,
                                ot_fifo32_num_used(&s->keygen->entropy_buf),
                                resched);

    if (resched && !s->keygen->edn_sched) {
        qemu_bh_schedule(s->keygen->entropy_bh);
    }
}

static void ot_otp_engine_fake_entropy(OtOTPEngineState *s, unsigned count)
{
    /*
     * This part departs from real HW: OTP needs to have bufferized enough
     * entropy for any SRAM OTP key request to be successfully completed.
     * On real HW, entropy is requested on demand, but in QEMU this very API
     * (#get_otp_key) needs to be synchronous, as it should be able to complete
     * on SRAM controller I/O request, which is itself fully synchronous.
     * When not enough entropy has been initiatially collected, this function
     * adds some fake entropy to entropy buffer. The main use case is to enable
     * SRAM initialization with random values and does not need to be truly
     * secure, while limiting emulation code size and complexity.
     */

    OtOTPKeyGen *kgen = s->keygen;
    while (count-- && !ot_fifo32_is_full(&kgen->entropy_buf)) {
        ot_fifo32_push(&kgen->entropy_buf, ot_prng_random_u32(kgen->prng));
    }
}

/*
 * See
 * https://opentitan.org/book/hw/top_earlgrey/ip_autogen/otp_ctrl/doc/
 * theory_of_operation.html#scrambling-datapath
 *
 * The `fetch_nonce_entropy` field refers to the fetching of additional
 * entropy for the nonce output.
 *
 * The `ingest_entropy` field indicates whether an additional 128 bit entropy
 * block should be ingested after the seed. That is, `true` will
 * derive an ephemeral scrambling key (path C) and `false` will derive a static
 * scrambling key (path D).
 *
 * Will fake entropy if there is not enough available, rather than waiting.
 */
static void ot_otp_engine_generate_scrambling_key(
    OtOTPEngineState *s, OtOTPKey *key, OtOTPKeyType type, uint64_t k_iv,
    const uint8_t *k_const, bool fetch_nonce_entropy, bool ingest_entropy)
{
    g_assert(type < OTP_KEY_COUNT);
    g_assert(key->seed_size < OT_OTP_SEED_MAX_SIZE);
    g_assert(key->nonce_size < OT_OTP_NONCE_MAX_SIZE);

    g_assert(key->seed_size % sizeof(uint32_t) == 0u);
    g_assert(key->nonce_size % sizeof(uint32_t) == 0u);
    unsigned seed_words = key->seed_size / sizeof(uint32_t);
    unsigned nonce_words = key->nonce_size / sizeof(uint32_t);
    unsigned scramble_blocks = key->seed_size / sizeof(uint64_t);

    OtFifo32 *entropy = &s->keygen->entropy_buf;

    /* for QEMU emulation, fake entropy instead of waiting */
    unsigned avail_entropy = ot_fifo32_num_used(entropy);
    unsigned needed_entropy = 0u;
    needed_entropy += fetch_nonce_entropy ? nonce_words : 0u;
    needed_entropy += ingest_entropy ? (seed_words * scramble_blocks) : 0u;
    if (avail_entropy < needed_entropy) {
        unsigned count = needed_entropy - avail_entropy;
        error_report("%s: %s: not enough entropy for key %d, fake %u words",
                     __func__, s->ot_id, type, count);
        ot_otp_engine_fake_entropy(s, count);
    }

    if (fetch_nonce_entropy) {
        /* fill in the nonce using entropy */
        g_assert(ot_fifo32_num_used(entropy) >= nonce_words);
        for (unsigned ix = 0; ix < nonce_words; ix++) {
            stl_le_p(&key->nonce[ix * sizeof(uint32_t)],
                     ot_fifo32_pop(entropy));
        }
    }

    OtPresentState *ps = s->keygen->present;

    /* obtain the key seed from the OTP SECRET partition(s) */
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);
    g_assert(ic->key_seeds);
    unsigned part_ix = ic->key_seeds[type].partition;
    /*
     * assume the key seeds are never stored in the first partition.
     * if partition is zero, the slot contains no key seed.
     * there is no reason for this API client to request a key seed which is
     * not available on the current Top.
     */
    g_assert(ic->key_seeds[type].partition);
    unsigned key_offset = ic->key_seeds[type].offset;
    g_assert(part_ix < s->part_count);
    g_assert(key_offset <= s->part_descs[part_ix].size - key->seed_size);

    OtOTPPartController *pctrl = &s->part_ctrls[part_ix];
    g_assert(ot_otp_engine_is_buffered(s, part_ix));
    const uint32_t *key_seed = &pctrl->buffer.data[key_offset];

    /* check the key seed's validity */
    key->seed_valid = pctrl->locked && !pctrl->failed;

    uint32_t *ephemeral_entropy = g_new0(uint32_t, seed_words);
    for (unsigned rix = 0; rix < scramble_blocks; rix++) {
        /* compress the IV state with the OTP key seed */
        uint64_t data = k_iv;
        ot_present_init(ps, (const uint8_t *)key_seed);
        ot_present_encrypt(ps, data, &data);

        if (ingest_entropy) {
            /* ephemeral keys ingest different entropy each round */
            g_assert(ot_fifo32_num_used(entropy) >= seed_words);
            for (unsigned ix = 0; ix < seed_words; ix++) {
                ephemeral_entropy[ix] = ot_fifo32_pop(entropy);
            }

            ot_present_init(ps, (uint8_t *)&ephemeral_entropy[0]);
            ot_present_encrypt(ps, data, &data);
        }

        /* compress with the finalization constant*/
        ot_present_init(ps, k_const);
        ot_present_encrypt(ps, data, &data);

        /* write back to the key */
        for (unsigned ix = 0; ix < sizeof(uint64_t); ix++) {
            unsigned seed_byte = rix * sizeof(uint64_t) + ix;
            key->seed[seed_byte] = (uint8_t)(data >> (ix * 8u));
        }
    }
    g_free(ephemeral_entropy);

    trace_ot_otp_key_generated(s->ot_id, type);

    if (needed_entropy) {
        /* some entropy bits have been used, refill the buffer */
        qemu_bh_schedule(s->keygen->entropy_bh);
    }
}

static void ot_otp_engine_get_otp_key(OtOTPIf *dev, OtOTPKeyType type,
                                      OtOTPKey *key)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(dev);
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);

    trace_ot_otp_get_otp_key(s->ot_id, type);

    /* reference: req_bundles in OpenTitan rtl/otp_ctrl_kdi.sv */
    const uint64_t *iv;
    const uint8_t *constant;
    bool ingest_entropy;
    switch (type) {
    case OTP_KEY_FLASH_DATA:
        if (!ic->has_flash_support) {
            iv = NULL;
            break;
        }
        key->seed_size = (uint8_t)ic->key_seeds[type].size;
        key->nonce_size = key->seed_size;
        iv = &s->flash_data_iv;
        constant = s->flash_data_const;
        ingest_entropy = false;
        return;
    case OTP_KEY_FLASH_ADDR:
        if (!ic->has_flash_support) {
            iv = NULL;
            break;
        }
        key->seed_size = (uint8_t)ic->key_seeds[type].size;
        key->nonce_size = 0u;
        iv = &s->flash_addr_iv;
        constant = s->flash_addr_const;
        ingest_entropy = false;
        return;
    case OTP_KEY_OTBN:
        key->seed_size = (uint8_t)ic->key_seeds[type].size;
        key->nonce_size = key->seed_size / 2u;
        iv = &s->sram_iv;
        constant = s->sram_const;
        ingest_entropy = true;
        return;
    case OTP_KEY_SRAM:
        key->seed_size = (uint8_t)ic->key_seeds[type].size;
        key->nonce_size = key->seed_size;
        iv = &s->sram_iv;
        constant = s->sram_const;
        ingest_entropy = true;
        return;
    default:
        iv = NULL;
        ingest_entropy = false;
        break;
    }

    if (!iv) {
        error_report("%s: %s: invalid OTP key type: %d", __func__, s->ot_id,
                     type);
        g_assert_not_reached();
    }

    g_assert(key->seed_size <= sizeof(s->scrmbl_key_init->key));
    g_assert(key->nonce_size <= sizeof(s->scrmbl_key_init->nonce));
    memcpy(key->seed, s->scrmbl_key_init->key, key->seed_size);
    memcpy(key->nonce, s->scrmbl_key_init->nonce, key->nonce_size);
    key->seed_valid = false;
    ot_otp_engine_generate_scrambling_key(s, key, type, *iv, constant, true,
                                          ingest_entropy);
}

static bool ot_otp_engine_program_req(OtOTPIf *dev, const uint16_t *lc_tcount,
                                      const uint16_t *lc_state,
                                      ot_otp_program_ack_fn ack, void *opaque)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(dev);
    OtOTPLCIController *lci = s->lci;

    switch (lci->state) {
    case OTP_LCI_IDLE:
    case OTP_LCI_ERROR:
        /* error case is handled asynchronously */
        g_assert(!(lci->ack_fn || lci->ack_data));
        break;
    case OTP_LCI_WRITE:
    case OTP_LCI_WRITE_WAIT:
        /* another LC programming request is on-going */
        return false;
    case OTP_LCI_RESET:
        /* cannot reach this point if PwrMgr init has been executed */
    default:
        g_assert_not_reached();
        break;
    }

    lci->ack_fn = ack;
    lci->ack_data = opaque;

    if (lci->state == OTP_LCI_IDLE) {
        unsigned hpos = 0;
        memcpy(&lci->data[hpos], lc_tcount, LC_TRANSITION_CNT_SIZE);
        hpos += LC_TRANSITION_CNT_SIZE / sizeof(uint16_t);
        memcpy(&lci->data[hpos], lc_state, LC_STATE_SIZE);
        hpos += LC_STATE_SIZE / sizeof(uint16_t);
        g_assert(hpos == s->part_descs[s->part_lc_num].size / sizeof(uint16_t));

        /* current position in LC buffer to write to backend */
        lci->hpos = 0u;
    }

    /*
     * schedule even if LCI FSM is already in error to report the issue
     * asynchronously
     */
    timer_mod(lci->prog_delay,
              qemu_clock_get_ns(OT_OTP_HW_CLOCK) + LCI_PROG_SCHED_NS);

    return true;
}

static void ot_otp_engine_lci_write_complete(OtOTPEngineState *s, bool success)
{
    OtOTPLCIController *lci = s->lci;

    if (lci->hpos) {
        /*
         * if the LC partition has been modified somehow, even if the request
         * has failed, update the backend file
         */
        const OtOTPPartDesc *lcdesc = &s->part_descs[s->part_lc_num];
        unsigned lc_off = lcdesc->offset / sizeof(uint32_t);
        uintptr_t offset = (uintptr_t)s->otp->data - (uintptr_t)s->otp->storage;
        if (ot_otp_engine_write_backend(s, &s->otp->data[lc_off],
                                        (unsigned)(offset + lcdesc->offset),
                                        lcdesc->size)) {
            error_report("%s: %s: cannot update OTP backend", __func__,
                         s->ot_id);
            if (lci->error == OTP_NO_ERROR) {
                lci->error = OTP_MACRO_ERROR;
                LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
            }
        }
        if (ot_otp_engine_is_ecc_enabled(s)) {
            offset = (uintptr_t)s->otp->ecc - (uintptr_t)s->otp->storage;
            if (ot_otp_engine_write_backend(s,
                                            &((uint16_t *)s->otp->ecc)[lc_off],
                                            (unsigned)(offset +
                                                       (lcdesc->offset >> 1u)),
                                            lcdesc->size >> 1u)) {
                error_report("%s: %s: cannot update OTP backend", __func__,
                             s->ot_id);
                if (lci->error == OTP_NO_ERROR) {
                    lci->error = OTP_MACRO_ERROR;
                    LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
                }
            }
        }
    }

    g_assert(lci->ack_fn);
    ot_otp_program_ack_fn ack_fn = lci->ack_fn;
    void *ack_data = lci->ack_data;
    lci->ack_fn = NULL;
    lci->ack_data = NULL;
    lci->hpos = 0u;

    if (!success && lci->error != OTP_NO_ERROR) {
        ot_otp_engine_set_error(s, s->part_lc_num, lci->error);
    }

    (*ack_fn)(ack_data, success);
}

static void ot_otp_engine_lci_write_word(void *opaque)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(opaque);
    OtOTPLCIController *lci = s->lci;
    const OtOTPPartDesc *lcdesc = &s->part_descs[s->part_lc_num];

    /* should not be called if already in error */
    if (lci->state == OTP_LCI_ERROR) {
        lci->error = OTP_FSM_STATE_ERROR;
        ot_otp_engine_lci_write_complete(s, false);
        return;
    }

    if (!ot_otp_engine_is_backend_writable(s)) {
        /* OTP backend missing or read-only; reject any write request */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: OTP backend file is missing or R/O\n", __func__,
                      s->ot_id);
        lci->error = OTP_MACRO_ERROR;
        LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
        ot_otp_engine_lci_write_complete(s, false);
        /* abort immediately */
        return;
    }

    if (lci->hpos >= lcdesc->size / sizeof(uint16_t)) {
        /* the whole LC partition has been updated */
        if (lci->error == OTP_NO_ERROR) {
            LCI_CHANGE_STATE(s, OTP_LCI_IDLE);
            ot_otp_engine_lci_write_complete(s, true);
        } else {
            LCI_CHANGE_STATE(s, OTP_LCI_ERROR);
            ot_otp_engine_lci_write_complete(s, false);
        }
        return;
    }

    LCI_CHANGE_STATE(s, OTP_LCI_WRITE);

    uint16_t *lc_dst =
        (uint16_t *)&s->otp->data[lcdesc->offset / sizeof(uint32_t)];

    uint16_t cur_val = lc_dst[lci->hpos];
    uint16_t new_val = lci->data[lci->hpos];

    trace_ot_otp_lci_write(s->ot_id, lci->hpos, cur_val, new_val);

    if (cur_val & ~new_val) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: cannot clear OTP bits @ %u: 0x%04x / 0x%04x\n",
                      __func__, s->ot_id, lci->hpos, cur_val, new_val);
        if (lci->error == OTP_NO_ERROR) {
            lci->error = OTP_MACRO_WRITE_BLANK_ERROR;
        }
        /*
         * "Note that if errors occur, we aggregate the error code but still
         *  attempt to program all remaining words. This is done to ensure that
         *  a life cycle state with ECC correctable errors in some words can
         *  still be scrapped."
         */
    }

    lc_dst[lci->hpos] |= new_val;

    if (ot_otp_engine_is_ecc_enabled(s)) {
        uint8_t *lc_edst =
            (uint8_t *)&s->otp->ecc[lcdesc->offset / (2u * sizeof(uint32_t))];
        uint8_t cur_ecc = lc_edst[lci->hpos];
        uint8_t new_ecc = ot_otp_engine_compute_ecc_u16(lc_dst[lci->hpos]);

        trace_ot_otp_lci_write_ecc(s->ot_id, lci->hpos, cur_ecc, new_ecc);

        if (cur_ecc & ~new_ecc) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: %s: cannot clear OTP ECC @ %u: 0x%02x / 0x%02x\n",
                __func__, s->ot_id, lci->hpos, cur_ecc, new_ecc);
            if (lci->error == OTP_NO_ERROR) {
                lci->error = OTP_MACRO_WRITE_BLANK_ERROR;
            }
        }

        lc_edst[lci->hpos] |= new_ecc;
    }

    lci->hpos += 1u;

    unsigned update_time = s->be_chars.timings.write_ns * sizeof(uint16_t);
    timer_mod(lci->prog_delay,
              qemu_clock_get_ns(OT_OTP_HW_CLOCK) + update_time);

    LCI_CHANGE_STATE(s, OTP_LCI_WRITE_WAIT);
}

static void ot_otp_engine_pwr_otp_req(void *opaque, int n, int level)
{
    OtOTPEngineState *s = opaque;

    g_assert(n == 0);

    if (level) {
        trace_ot_otp_pwr_otp_req(s->ot_id, "signaled");
        qemu_bh_schedule(s->pwr_otp_bh);
    }
}

static void ot_otp_engine_pwr_load(OtOTPEngineState *s)
{
    /*
     * HEADER_FORMAT
     *
     *  | magic    |     4 char | "vOFTP"                                |
     *  | hlength  |   uint32_t | count of header bytes after this point |
     *  | version  |   uint32_t | version of the header (v2)             |
     *  | eccbits  |   uint16_t | ECC size in bits                       |
     *  | eccgran  |   uint16_t | ECC granule                            |
     *  | dlength  |   uint32_t | count of data bytes (% uint64_t)       |
     *  | elength  |   uint32_t | count of ecc bytes (% uint64_t)        |
     *  | -------- | ---------- | only in V2                             |
     *  | dig_iv   |  8 uint8_t | Present digest initialization vector   |
     *  | dig_iv   | 16 uint8_t | Present digest initialization vector   |
     */

    struct otp_header {
        char magic[4];
        uint32_t hlength;
        uint32_t version;
        uint16_t eccbits;
        uint16_t eccgran;
        uint32_t data_len;
        uint32_t ecc_len;
        /* added in V2 */
        uint8_t digest_iv[8u];
        uint8_t digest_constant[16u];
    };

    static_assert(sizeof(struct otp_header) == 48u, "Invalid header size");

    /* data following header should always be 64-bit aligned */
    static_assert((sizeof(struct otp_header) % sizeof(uint64_t)) == 0,
                  "invalid header definition");

    size_t header_size = sizeof(struct otp_header);
    size_t data_size = 0u;
    size_t ecc_size = 0u;

    for (unsigned part_ix = 0u; part_ix < s->part_count; part_ix++) {
        size_t psize = (size_t)s->part_descs[part_ix].size;
        size_t dsize = ROUND_UP(psize, sizeof(uint64_t));
        data_size += dsize;
        /* up to 1 ECC byte for 2 data bytes */
        ecc_size += DIV_ROUND_UP(dsize, 2u);
    }
    size_t otp_size = header_size + data_size + ecc_size;

    otp_size = ROUND_UP(otp_size, 4096u);

    OtOTPStorage *otp = s->otp;

    /* always allocates the requested size even if blk is NULL */
    if (!otp->storage) {
        /* only allocated once on PoR */
        otp->storage = blk_blockalign(s->blk, otp_size);
    }

    uintptr_t base = (uintptr_t)otp->storage;
    g_assert(!(base & (sizeof(uint64_t) - 1u)));

    memset(otp->storage, 0, otp_size);

    otp->data = (uint32_t *)(base + sizeof(struct otp_header));
    otp->ecc = (uint32_t *)(base + sizeof(struct otp_header) + data_size);
    otp->ecc_bit_count = 0u;
    otp->ecc_granule = 0u;

    if (s->blk) {
        /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
        int rc = blk_pread(s->blk, 0, (int64_t)otp_size, otp->storage,
                           (BdrvRequestFlags)0);
        /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
        if (rc < 0) {
            error_setg(&error_fatal,
                       "%s: failed to read the initial OTP content %zu bytes: "
                       "%d",
                       s->ot_id, otp_size, rc);
            return;
        }

        const struct otp_header *otp_hdr = (const struct otp_header *)base;

        if (memcmp(otp_hdr->magic, "vOTP", sizeof(otp_hdr->magic)) != 0) {
            error_setg(&error_fatal, "%s: OTP file is not a valid OTP backend",
                       s->ot_id);
            return;
        }
        if (otp_hdr->version != 1u && otp_hdr->version != 2u) {
            error_setg(&error_fatal, "%s: OTP file version %u is not supported",
                       s->ot_id, otp_hdr->version);
            return;
        }

        uintptr_t data_offset = otp_hdr->hlength + 8u; /* magic & length */
        uintptr_t ecc_offset = data_offset + otp_hdr->data_len;

        otp->data = (uint32_t *)(base + data_offset);
        otp->ecc = (uint32_t *)(base + ecc_offset);
        otp->ecc_bit_count = otp_hdr->eccbits;
        otp->ecc_granule = otp_hdr->eccgran;

        if (otp->ecc_bit_count != 6u || !ot_otp_engine_is_ecc_enabled(s)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: support for ECC %u/%u not implemented\n",
                          __func__, s->ot_id, otp->ecc_granule,
                          otp->ecc_bit_count);
        }

        bool write = blk_supports_write_perm(s->blk);
        trace_ot_otp_load_backend(s->ot_id, otp_hdr->version,
                                  write ? "R/W" : "R/O", otp->ecc_bit_count,
                                  otp->ecc_granule);

        if (otp_hdr->version == 2u) {
            /*
             * Version 2 is deprecated and digest const/IV are now ignored.
             * Nonetheless, keep checking for inconsistencies.
             */
            if (s->digest_iv != ldq_le_p(otp_hdr->digest_iv)) {
                error_report("%s: %s: OTP file digest IV mismatch", __func__,
                             s->ot_id);
            }
            if (memcmp(s->digest_const, otp_hdr->digest_constant,
                       sizeof(s->digest_const)) != 0) {
                error_report("%s: %s: OTP file digest const mismatch", __func__,
                             s->ot_id);
            }
        }
    }

    otp->data_size = data_size;
    otp->ecc_size = ecc_size;
    otp->size = otp_size;
}
static void ot_otp_engine_pwr_initialize_partitions(OtOTPEngineState *s)
{
    for (unsigned ix = 0; ix < s->part_count; ix++) {
        /* sanity check: all secret partitions are also buffered */
        g_assert(!s->part_descs[ix].secret || s->part_descs[ix].buffered);

        if (ot_otp_engine_is_ecc_enabled(s) && s->part_descs[ix].integrity) {
            if (ot_otp_engine_apply_ecc(s, ix)) {
                continue;
            }
        }

        if (s->part_descs[ix].sw_digest) {
            s->part_ctrls[ix].digest = ot_otp_engine_get_part_digest(s, ix);
            s->part_ctrls[ix].locked = s->part_ctrls[ix].digest != 0;
            continue;
        }

        if (s->part_descs[ix].buffered) {
            ot_otp_engine_bufferize_partition(s, ix);
            if (s->part_descs[ix].hw_digest) {
                ot_otp_engine_check_buffered_partition_integrity(s, ix);
            }
            continue;
        }
    }
}

static void ot_otp_engine_pwr_otp_bh(void *opaque)
{
    OtOTPEngineState *s = opaque;
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);

    /*
     * This sequence is triggered from the Power Manager, in the early boot
     * sequence while the OT IPs are maintained in reset.
     * This means that all ot_otp_engine_pwr_* functions are called before the
     * OTP IP is released from reset.
     *
     * The QEMU reset is not a 1:1 mapping to the actual HW.
     */
    trace_ot_otp_pwr_otp_req(s->ot_id, "initialize");

    /* load OTP data from OTP back-end file */
    ot_otp_engine_pwr_load(s);
    /* check ECC, digests, configure locks and bufferize partitions */
    ot_otp_engine_pwr_initialize_partitions(s);


    if (ic->signal_pwr_sequence) {
        ic->signal_pwr_sequence(OT_OTP_IMPL_IF(s));
    }

    /* initialize direct access interface */
    ot_otp_engine_dai_init(s);
    /* initialize LC controller interface */
    ot_otp_engine_lci_init(s);

    trace_ot_otp_pwr_otp_req(s->ot_id, "done");

    /* toggle OTP completion to signal the power manager OTP init is complete */
    ibex_irq_set(&s->pwc_otp_rsp, 1);
    ibex_irq_set(&s->pwc_otp_rsp, 0);
}

static void ot_otp_engine_configure_scrmbl_key(OtOTPEngineState *s)
{
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);

    if (!s->scrmbl_key_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "scrmbl_key");
        return;
    }

    size_t sram_key_bytes = SRAM_KEY_BYTES(ic);
    size_t sram_nonce_bytes = SRAM_NONCE_BYTES(ic);
    size_t len = strlen(s->scrmbl_key_xstr);
    if (len != (size_t)(sram_key_bytes + sram_nonce_bytes) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid scrmbl_key length\n", __func__,
                   s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(s->scrmbl_key_init->key,
                                 &s->scrmbl_key_xstr[0], sram_key_bytes, false,
                                 false)) {
        error_setg(&error_fatal, "%s: %s unable to parse scrmbl_key\n",
                   __func__, s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(s->scrmbl_key_init->nonce,
                                 &s->scrmbl_key_xstr[sram_key_bytes * 2u],
                                 sram_nonce_bytes, false, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse scrmbl_key\n",
                   __func__, s->ot_id);
        return;
    }
}

static void ot_otp_engine_configure_digest(OtOTPEngineState *s)
{
    memset(s->digest_const, 0, sizeof(s->digest_const));
    s->digest_iv = 0ull;

    if (!s->digest_const_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "digest_const");
        return;
    }

    if (!s->digest_iv_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "digest_iv");
        return;
    }

    size_t len;

    len = strlen(s->digest_const_xstr);
    if (len != sizeof(s->digest_const) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid digest_const length\n",
                   __func__, s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(s->digest_const, s->digest_const_xstr,
                                 sizeof(s->digest_const), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse digest_const\n",
                   __func__, s->ot_id);
        return;
    }

    uint8_t digest_iv[sizeof(uint64_t)];

    len = strlen(s->digest_iv_xstr);
    if (len != sizeof(digest_iv) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid digest_iv length\n", __func__,
                   s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(digest_iv, s->digest_iv_xstr,
                                 sizeof(digest_iv), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse digest_iv\n", __func__,
                   s->ot_id);
        return;
    }

    s->digest_iv = ldq_le_p(digest_iv);
}

static void ot_otp_engine_configure_flash(OtOTPEngineState *s)
{
    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(s);

    if (!ic->has_flash_support) {
        return;
    }

    memset(s->flash_data_const, 0, sizeof(s->flash_data_const));
    memset(s->flash_addr_const, 0, sizeof(s->flash_addr_const));
    s->flash_data_iv = 0ull;
    s->flash_addr_iv = 0ull;

    if (!s->flash_data_const_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "flash_data_const");
        return;
    }
    if (!s->flash_addr_const_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "flash_addr_const");
        return;
    }
    if (!s->flash_data_iv_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "flash_data_iv");
        return;
    }
    if (!s->flash_addr_iv_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "flash_addr_iv");
        return;
    }

    size_t len;

    len = strlen(s->flash_data_const_xstr);
    if (len != sizeof(s->flash_data_const) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid flash_data_const length\n",
                   __func__, s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(s->flash_data_const, s->flash_data_const_xstr,
                                 sizeof(s->flash_data_const), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse flash_data_const\n",
                   __func__, s->ot_id);
        return;
    }

    len = strlen(s->flash_addr_const_xstr);
    if (len != sizeof(s->flash_addr_const) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid flash_addr_const length\n",
                   __func__, s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(s->flash_addr_const, s->flash_addr_const_xstr,
                                 sizeof(s->flash_addr_const), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse flash_addr_const\n",
                   __func__, s->ot_id);
        return;
    }

    uint8_t flash_data_iv[sizeof(uint64_t)];

    len = strlen(s->flash_data_iv_xstr);
    if (len != sizeof(flash_data_iv) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid flash_data_iv length\n",
                   __func__, s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(flash_data_iv, s->flash_data_iv_xstr,
                                 sizeof(flash_data_iv), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse flash_data_iv\n",
                   __func__, s->ot_id);
        return;
    }

    s->flash_data_iv = ldq_le_p(flash_data_iv);

    uint8_t flash_addr_iv[sizeof(uint64_t)];

    len = strlen(s->flash_addr_iv_xstr);
    if (len != sizeof(flash_addr_iv) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid flash_addr_iv length\n",
                   __func__, s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(flash_addr_iv, s->flash_addr_iv_xstr,
                                 sizeof(flash_addr_iv), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse flash_addr_iv\n",
                   __func__, s->ot_id);
        return;
    }

    s->flash_addr_iv = ldq_le_p(flash_addr_iv);
}

static void ot_otp_engine_configure_sram(OtOTPEngineState *s)
{
    memset(s->sram_const, 0, sizeof(s->sram_const));
    s->sram_iv = 0ull;

    if (!s->sram_const_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "sram_const");
        return;
    }

    if (!s->sram_iv_xstr) {
        trace_ot_otp_configure_missing(s->ot_id, "sram_iv");
        return;
    }

    size_t len;

    len = strlen(s->sram_const_xstr);
    if (len != sizeof(s->sram_const) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid sram_const length\n", __func__,
                   s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(s->sram_const, s->sram_const_xstr,
                                 sizeof(s->sram_const), true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse sram_const\n",
                   __func__, s->ot_id);
        return;
    }

    uint8_t sram_iv[sizeof(uint64_t)];

    len = strlen(s->sram_iv_xstr);
    if (len != sizeof(sram_iv) * 2u) {
        error_setg(&error_fatal, "%s: %s invalid sram_iv length\n", __func__,
                   s->ot_id);
        return;
    }

    if (ot_common_parse_hexa_str(sram_iv, s->sram_iv_xstr, sizeof(sram_iv),
                                 true, true)) {
        error_setg(&error_fatal, "%s: %s unable to parse sram_iv\n", __func__,
                   s->ot_id);
        return;
    }

    s->sram_iv = ldq_le_p(sram_iv);
}

static void ot_otp_engine_configure_part_scramble_keys(OtOTPEngineState *s)
{
    g_assert(s->part_count);

    for (unsigned part_ix = 0u; part_ix < s->part_count; part_ix++) {
        if (!s->part_descs[part_ix].secret) {
            continue;
        }

        if (!s->otp_scramble_key_xstrs[part_ix]) {
            /* if OTP data is loaded, unscrambling keys are mandatory */
            if (s->blk) {
                error_setg(&error_fatal,
                           "%s: %s Missing OTP scrambling key for part %s (%u)",
                           __func__, s->ot_id,
                           ot_otp_engine_part_name(s, part_ix), part_ix);
                return;
            }
            continue;
        }

        size_t len = strlen(s->otp_scramble_key_xstrs[part_ix]);
        if (len != OTP_SCRAMBLING_KEY_BYTES * 2u) {
            error_setg(
                &error_fatal,
                "%s: %s Invalid OTP scrambling key length %zu for part %s (%u)",
                __func__, s->ot_id, len, ot_otp_engine_part_name(s, part_ix),
                part_ix);
            return;
        }

        g_assert(!s->otp_scramble_keys[part_ix]);

        s->otp_scramble_keys[part_ix] =
            g_new0(uint8_t, OTP_SCRAMBLING_KEY_BYTES);
        if (ot_common_parse_hexa_str(s->otp_scramble_keys[part_ix],
                                     s->otp_scramble_key_xstrs[part_ix],
                                     OTP_SCRAMBLING_KEY_BYTES, true, true)) {
            error_setg(&error_fatal,
                       "%s: %s unable to parse otp_scramble_keys[%u] for %s",
                       __func__, s->ot_id, part_ix,
                       ot_otp_engine_part_name(s, part_ix));
            return;
        }

        TRACE_OTP("otp_scramble_keys[%s] %s",
                  ot_otp_engine_part_name(s, part_ix),
                  ot_otp_hexdump(s, s->otp_scramble_keys[part_ix],
                                 OTP_SCRAMBLING_KEY_BYTES));
    }
}

static void ot_otp_engine_add_scramble_key_props(OtOTPEngineState *s)
{
    g_assert(s->part_count);

    /*
     * @todo we know the number of secret partitions, so use it rather than
     * whole partition count
     */
    s->otp_scramble_keys = g_new0(uint8_t *, s->part_count);
    s->otp_scramble_key_xstrs = g_new0(char *, s->part_count);

    unsigned secret_ix = 0u;
    for (unsigned part_ix = 0u; part_ix < s->part_count; part_ix++) {
        if (!s->part_descs[part_ix].secret) {
            continue;
        }

        Property *prop = g_new0(Property, 1u);

        /*
         * Assumes secret partitions are sequentially ordered and named
         * SECRET0, SECRET1, SECRET2, ...
         */
        prop->name = g_strdup_printf("secret%u_scramble_key", secret_ix++);
        prop->info = &qdev_prop_string;
        /*
         * Property stores the address of the stored string as a relative offset
         * from the parent address
         */
        prop->offset =
            (intptr_t)&s->otp_scramble_key_xstrs[part_ix] - (intptr_t)s;

        object_property_add(OBJECT(s), prop->name, prop->info->name,
                            prop->info->get, prop->info->set,
                            prop->info->release, prop);
    }
}

static void ot_otp_engine_configure_inv_default_parts(OtOTPEngineState *s)
{
    g_assert(s->part_count);

    for (unsigned part_ix = 0; part_ix < s->part_count; part_ix++) {
        if (!s->inv_default_part_xstrs[part_ix]) {
            continue;
        }

        const OtOTPPartDesc *part = &s->part_descs[part_ix];

        size_t len;

        len = strlen(s->inv_default_part_xstrs[part_ix]);
        if (len != part->size * 2u) {
            error_setg(&error_fatal,
                       "%s: %s invalid inv_default_part[%u] length\n", __func__,
                       s->ot_id, part_ix);
            return;
        }

        g_assert(!s->inv_default_parts[part_ix]);

        s->inv_default_parts[part_ix] = g_new0(uint8_t, part->size + 1u);
        if (ot_common_parse_hexa_str(s->inv_default_parts[part_ix],
                                     s->inv_default_part_xstrs[part_ix],
                                     part->size, false, true)) {
            error_setg(&error_fatal,
                       "%s: %s unable to parse inv_default_part[%u]\n",
                       __func__, s->ot_id, part_ix);
            return;
        }

        TRACE_OTP("inv_default_part[%s] %s",
                  ot_otp_engine_part_name(s, part_ix),
                  ot_otp_hexdump(s, s->inv_default_parts[part_ix], part->size));
    }
}

static void ot_otp_engine_add_inv_def_props(OtOTPEngineState *s)
{
    g_assert(s->part_count);

    s->inv_default_parts = g_new0(uint8_t *, s->part_count);
    s->inv_default_part_xstrs = g_new0(char *, s->part_count);

    for (unsigned part_ix = 0; part_ix < s->part_count; part_ix++) {
        if (!s->part_descs[part_ix].buffered) {
            continue;
        }

        Property *prop = g_new0(Property, 1u);

        prop->name = g_strdup_printf("inv_default_part_%u", part_ix);
        prop->info = &qdev_prop_string;
        /*
         * Property stores the address of the stored string as a relative offset
         * from the parent address
         */
        prop->offset =
            (intptr_t)&s->inv_default_part_xstrs[part_ix] - (intptr_t)s;

        object_property_add(OBJECT(s), prop->name, prop->info->name,
                            prop->info->get, prop->info->set,
                            prop->info->release, prop);
    }
}

static Property ot_otp_engine_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtOTPEngineState, ot_id),
    DEFINE_PROP_DRIVE("drive", OtOTPEngineState, blk),
    DEFINE_PROP_LINK("backend", OtOTPEngineState, otp_backend,
                     TYPE_OT_OTP_BE_IF, OtOtpBeIf *),
    DEFINE_PROP_LINK("edn", OtOTPEngineState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_UINT8("edn-ep", OtOTPEngineState, edn_ep, UINT8_MAX),
    DEFINE_PROP_STRING("scrmbl_key", OtOTPEngineState, scrmbl_key_xstr),
    DEFINE_PROP_STRING("digest_const", OtOTPEngineState, digest_const_xstr),
    DEFINE_PROP_STRING("digest_iv", OtOTPEngineState, digest_iv_xstr),
    DEFINE_PROP_STRING("sram_const", OtOTPEngineState, sram_const_xstr),
    DEFINE_PROP_STRING("sram_iv", OtOTPEngineState, sram_iv_xstr),
    DEFINE_PROP_STRING("flash_data_const", OtOTPEngineState,
                       flash_data_const_xstr),
    DEFINE_PROP_STRING("flash_data_iv", OtOTPEngineState, flash_data_iv_xstr),
    DEFINE_PROP_STRING("flash_addr_const", OtOTPEngineState,
                       flash_addr_const_xstr),
    DEFINE_PROP_STRING("flash_addr_iv", OtOTPEngineState, flash_addr_iv_xstr),
    DEFINE_PROP_BOOL("fatal_escalate", OtOTPEngineState, fatal_escalate, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_otp_engine_reset_enter(Object *obj, ResetType type)
{
    OtOTPEngineClass *c = OT_OTP_ENGINE_GET_CLASS(obj);
    OtOTPEngineState *s = OT_OTP_ENGINE(obj);

    /*
     * Note: beware of the special reset sequence for the OTP controller,
     * see comments from ot_otp_engine_pwr_otp_bh, as this very QEMU reset may
     * be called after ot_otp_engine_pwr_otp_bh is invoked, hereby changing the
     * usual realize-reset sequence.
     *
     * File back-end storage (loading) is processed from
     * the ot_otp_engine_pwr_otp_bh handler, to ensure data is reloaded from the
     * backend on each reset, prior to this very reset function. This reset
     * function should not alter the storage content.
     *
     * Ideally the OTP reset functions should be decoupled from the regular
     * IP reset, which are exercised automatically from the SoC, since all the
     * OT SysBysDevice IPs are connected to the private system bus of the Ibex.
     * This is by-design in QEMU. The reset management is already far too
     * complex to create a special case for the OTP. Keep in mind that the OTP
     * reset_enter/reset_exit functions are QEMU regular reset functions called
     * as part of the private bus reset and do not represent the actual OTP HW
     * reset. Part of this reset is handled in the Power Manager handler.
     */
    trace_ot_otp_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    qemu_bh_cancel(s->dai->digest_bh);
    qemu_bh_cancel(s->lc_broadcast.bh);
    qemu_bh_cancel(s->pwr_otp_bh);

    timer_del(s->dai->delay);
    timer_del(s->lci->prog_delay);
    qemu_bh_cancel(s->keygen->entropy_bh);
    s->keygen->edn_sched = false;

    memset(s->hw_cfg, 0, sizeof(*s->hw_cfg));

    /* ensure OTP implementation has defined all the required values */
    g_assert(s->reg_offset.dai_base > R_ALERT_TEST);
    g_assert(s->reg_offset.err_code_base > R_ALERT_TEST);
    g_assert(s->reg_offset.read_lock_base > R_ALERT_TEST);

    s->alert_bm = 0;

    s->lc_broadcast.current_level = 0u;
    s->lc_broadcast.level = 0u;
    s->lc_broadcast.signal = 0u;

    ot_otp_engine_update_irqs(s);
    ot_otp_engine_update_alerts(s);
    ibex_irq_set(&s->pwc_otp_rsp, 0);

    for (unsigned part_ix = 0; part_ix < s->part_count; part_ix++) {
        /* @todo initialize with actual default partition data once known */
        if (s->part_descs[part_ix].buffered) {
            s->part_ctrls[part_ix].state.b = OTP_BUF_IDLE;
        } else {
            s->part_ctrls[part_ix].state.u = OTP_UNBUF_IDLE;
            continue;
        }
        unsigned part_size = ot_otp_engine_part_data_byte_size(s, part_ix);
        memset(s->part_ctrls[part_ix].buffer.data, 0, part_size);
        s->part_ctrls[part_ix].digest = 0;
        if (s->part_descs[part_ix].iskeymgr_creator ||
            s->part_descs[part_ix].iskeymgr_owner) {
            s->part_ctrls[part_ix].read_lock = true;
            s->part_ctrls[part_ix].write_lock = true;
        }
    }
    DAI_CHANGE_STATE(s, OTP_DAI_RESET);
    LCI_CHANGE_STATE(s, OTP_LCI_RESET);
}

static void ot_otp_engine_reset_exit(Object *obj, ResetType type)
{
    OtOTPEngineClass *c = OT_OTP_ENGINE_GET_CLASS(obj);
    OtOTPEngineState *s = OT_OTP_ENGINE(obj);

    trace_ot_otp_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    OtOtpBeIfClass *bec = OT_OTP_BE_IF_GET_CLASS(s->otp_backend);
    memcpy(&s->be_chars, bec->get_characteristics(s->otp_backend),
           sizeof(OtOtpBeCharacteristics));

    ot_edn_connect_endpoint(s->edn, s->edn_ep,
                            &ot_otp_engine_keygen_push_entropy, s);

    qemu_bh_schedule(s->keygen->entropy_bh);
}

static void ot_otp_engine_realize(DeviceState *dev, Error **errp)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(dev);
    (void)errp;

    g_assert(s->ot_id);
    g_assert(s->otp_backend);

    /* ensure OTP implementation has initialized these values */
    g_assert(s->regs != NULL);

    /*
     * Set the OTP drive's permissions now during realization. We can't leave it
     * until reset because QEMU might have `-daemonize`d and changed directory,
     * invalidating the filesystem path to the OTP image.
     */
    if (s->blk) {
        bool write = blk_supports_write_perm(s->blk);
        uint64_t perm = BLK_PERM_CONSISTENT_READ | (write ? BLK_PERM_WRITE : 0);
        if (blk_set_perm(s->blk, perm, perm, &error_fatal)) {
            warn_report("%s: %s: OTP backend is R/O", __func__, s->ot_id);
        }
    }

    ot_otp_engine_configure_scrmbl_key(s);
    ot_otp_engine_configure_digest(s);
    ot_otp_engine_configure_sram(s);
    ot_otp_engine_configure_flash(s);
    ot_otp_engine_configure_part_scramble_keys(s);
    ot_otp_engine_configure_inv_default_parts(s);
}

static void ot_otp_engine_init(Object *obj)
{
    OtOTPEngineState *s = OT_OTP_ENGINE(obj);

    OtOTPImplIfClass *ic = OT_OTP_IMPL_IF_GET_CLASS(obj);

    g_assert(ic->update_status_error != NULL);

    g_assert(ic->part_descs != NULL);
    g_assert(ic->part_count > 1u);
    g_assert(ic->part_lc_num > 0u && ic->part_lc_num < ic->part_count);
    g_assert(ic->sram_key_req_slot_count > 0u);

    /*
     * The following members are constant values, and are used very often in
     * this implementation. Add them to the Engine instance to avoid querying
     * them every time from the concrete class object
     */
    s->part_descs = ic->part_descs;
    s->part_count = ic->part_count;
    s->part_lc_num = ic->part_lc_num;

    ibex_qdev_init_irq(obj, &s->pwc_otp_rsp, OT_PWRMGR_OTP_RSP);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_otp_engine_pwr_otp_req,
                            OT_PWRMGR_OTP_REQ, 1);

    for (unsigned ix = 0; ix < ARRAY_SIZE(s->irqs); ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < ARRAY_SIZE(s->alerts); ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    qdev_init_gpio_in_named(DEVICE(obj), &ot_otp_engine_lc_broadcast_recv,
                            OT_LC_BROADCAST, OT_OTP_LC_BROADCAST_COUNT);

    s->part_ctrls = g_new0(OtOTPPartController, s->part_count);
    s->hw_cfg = g_new0(OtOTPHWCfg, 1u);
    s->tokens = g_new0(OtOTPTokens, 1u);
    s->dai = g_new0(OtOTPDAIController, 1u);
    s->lci = g_new0(OtOTPLCIController, 1u);
    s->keygen = g_new0(OtOTPKeyGen, 1u);
    s->otp = g_new0(OtOTPStorage, 1u);
    s->scrmbl_key_init = g_new0(OtOTPScrmblKeyInit, 1u);
    s->lci->data =
        g_new0(uint16_t, s->part_descs[s->part_lc_num].size / sizeof(uint16_t));

    ot_fifo32_create(&s->keygen->entropy_buf, OTP_ENTROPY_BUF_COUNT(ic));
    s->keygen->present = ot_present_new();
    s->keygen->prng = ot_prng_allocate();

    s->dai->delay =
        timer_new_ns(OT_VIRTUAL_CLOCK, &ot_otp_engine_dai_complete, s);
    s->dai->digest_bh = qemu_bh_new(&ot_otp_engine_dai_write_digest, s);
    s->lci->prog_delay =
        timer_new_ns(OT_OTP_HW_CLOCK, &ot_otp_engine_lci_write_word, s);
    s->pwr_otp_bh = qemu_bh_new(&ot_otp_engine_pwr_otp_bh, s);
    s->lc_broadcast.bh = qemu_bh_new(&ot_otp_engine_lc_broadcast_bh, s);
    s->keygen->entropy_bh = qemu_bh_new(&ot_otp_engine_request_entropy_bh, s);

    for (unsigned part_ix = 0; part_ix < s->part_count; part_ix++) {
        if (!s->part_descs[part_ix].buffered) {
            continue;
        }
        size_t part_words =
            ot_otp_engine_part_data_byte_size(s, part_ix) / sizeof(uint32_t);
        s->part_ctrls[part_ix].buffer.data = g_new0(uint32_t, part_words);
    }

    ot_otp_engine_add_scramble_key_props(s);
    ot_otp_engine_add_inv_def_props(s);

    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    ot_prng_reseed(s->keygen->prng, (uint32_t)now);

#ifdef OT_OTP_DEBUG
    s->hexstr = g_new0(char, OT_OTP_HEXSTR_SIZE);
#endif
}

static void ot_otp_engine_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_otp_engine_realize;
    device_class_set_props(dc, ot_otp_engine_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtOTPEngineClass *ec = OT_OTP_ENGINE_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_otp_engine_reset_enter, NULL,
                                       &ot_otp_engine_reset_exit,
                                       &ec->parent_phases);

    ec->update_irqs = &ot_otp_engine_update_irqs;
    ec->update_alerts = &ot_otp_engine_update_alerts;
    ec->get_part_from_address = &ot_otp_engine_get_part_from_address;
    ec->get_part_digest_reg = &ot_otp_engine_get_part_digest_reg;
    ec->is_readable = &ot_otp_engine_is_readable;
    ec->set_error = &ot_otp_engine_set_error;
    ec->dai_read = &ot_otp_engine_dai_read;
    ec->dai_write = &ot_otp_engine_dai_write;
    ec->dai_digest = &ot_otp_engine_dai_digest;
    ec->get_hw_cfg = &ot_otp_engine_get_hw_cfg;
    ec->get_otp_key = &ot_otp_engine_get_otp_key;
    ec->program_req = &ot_otp_engine_program_req;
}

static const TypeInfo ot_otp_engine_info = {
    .name = TYPE_OT_OTP_ENGINE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtOTPEngineState),
    .instance_init = &ot_otp_engine_init,
    .class_size = sizeof(OtOTPEngineClass),
    .class_init = &ot_otp_engine_class_init,
    .abstract = true,
};

static void ot_otp_engine_register_types(void)
{
    type_register_static(&ot_otp_engine_info);
}

type_init(ot_otp_engine_register_types);
