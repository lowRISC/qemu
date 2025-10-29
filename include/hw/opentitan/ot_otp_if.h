/*
 * QEMU OpenTitan One Time Programmable (OTP) memory controller public interface
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
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

#ifndef HW_OPENTITAN_OT_OTP_IF_H
#define HW_OPENTITAN_OT_OTP_IF_H

#include "qom/object.h"
#include "hw/opentitan/ot_common.h"

#define TYPE_OT_OTP_IF "ot-otp_if"
typedef struct OtOTPIfClass OtOTPIfClass;
typedef struct OtOTPIf OtOTPIf;

DECLARE_CLASS_CHECKERS(OtOTPIfClass, OT_OTP_IF, TYPE_OT_OTP_IF)
#define OT_OTP_IF(_obj_) INTERFACE_CHECK(OtOTPIf, (_obj_), TYPE_OT_OTP_IF)

/* Input signals from life cycle */
typedef enum {
    /* "Enable the TL-UL access port to the proprietary OTP IP." */
    OT_OTP_LC_DFT_EN,
    /* "Move all FSMs within OTP into the error state." */
    OT_OTP_LC_ESCALATE_EN,
    /* "Bypass consistency checks during life cycle state transitions." */
    OT_OTP_LC_CHECK_BYP_EN,
    /* "Enables SW R/W to the KeyMgr material partitions", should be SECRET2 */
    OT_OTP_LC_CREATOR_SEED_SW_RW_EN,
    /* see above, should be SECRET3 */
    OT_OTP_LC_OWNER_SEED_SW_RW_EN,
    /* "Enable HW R/O to the CREATOR_ROOT_KEY_SHARE{0,1}." */
    OT_OTP_LC_SEED_HW_RD_EN,
    OT_OTP_LC_BROADCAST_COUNT,
} OtOtpLcBroadcast;

#define OT_OTP_HWCFG_DEVICE_ID_BYTES     32u
#define OT_OTP_HWCFG_MANUF_STATE_BYTES   32u
#define OT_OTP_HWCFG_SOC_DBG_STATE_BYTES 4u

/*
 * Hardware configuration (for HW_CFG partitions).
 *
 * Digests are not added to this structure since real HW does not use them
 *
 * Note that some fields may be meaningless / not initialized depending on the
 * actual OpenTitan top.
 */
typedef struct {
    uint8_t device_id[OT_OTP_HWCFG_DEVICE_ID_BYTES];
    uint8_t manuf_state[OT_OTP_HWCFG_MANUF_STATE_BYTES];
    uint8_t soc_dbg_state[OT_OTP_HWCFG_SOC_DBG_STATE_BYTES];
    ot_mb8_t en_sram_ifetch_mb8;
    ot_mb8_t en_csrng_sw_app_read_mb8;
    ot_mb_lc4_t valid_lc4; /* seems generated but no used on real HW */
} OtOTPHWCfg;

typedef enum {
    OTP_TOKEN_TEST_UNLOCK,
    OTP_TOKEN_TEST_EXIT,
    OTP_TOKEN_RMA,
    OTP_TOKEN_COUNT
} OtOTPToken;

typedef struct {
    uint64_t lo;
    uint64_t hi;
} OtOTPTokenValue;

typedef struct {
    OtOTPTokenValue values[OTP_TOKEN_COUNT];
    uint32_t valid_bm; /* OtLcCtrlToken-indexed valid bit flags */
} OtOTPTokens;

typedef enum {
    OTP_KEY_FLASH_DATA,
    OTP_KEY_FLASH_ADDR,
    OTP_KEY_OTBN,
    OTP_KEY_SRAM,
    OTP_KEY_COUNT
} OtOTPKeyType;

#define OT_OTP_SEED_MAX_SIZE  32u /* 256 bits */
#define OT_OTP_NONCE_MAX_SIZE 32u /* 256 bits */

typedef struct {
    uint8_t seed[OT_OTP_SEED_MAX_SIZE];
    uint8_t nonce[OT_OTP_NONCE_MAX_SIZE];
    uint8_t seed_size; /* size in bytes */
    uint8_t nonce_size; /* size in bytes */
    bool seed_valid; /* whether the seed is valid */
} OtOTPKey;

typedef enum {
    OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE0,
    OTP_KEYMGR_SECRET_CREATOR_ROOT_KEY_SHARE1,
    OTP_KEYMGR_SECRET_CREATOR_SEED,
    OTP_KEYMGR_SECRET_OWNER_SEED,
    OTP_KEYMGR_SECRET_COUNT
} OtOTPKeyMgrSecretType;

#define OT_OTP_KEYMGR_SECRET_SIZE 32u /* 256 bits (for both keys and seeds) */

typedef struct {
    uint8_t secret[OT_OTP_KEYMGR_SECRET_SIZE]; /* key/seed data */
    bool valid; /* whether the key/seed data is valid */
} OtOTPKeyMgrSecret;

typedef void (*ot_otp_program_ack_fn)(void *opaque, bool ack);

struct OtOTPIfClass {
    InterfaceClass parent_class;

    /*
     * Provide OTP lifecycle information.
     *
     * @dev the OTP device
     * @lc_tcount if not NULL, updated with the raw LifeCycle transition count
     *            buffer.
     * @lc_state if not NULL, updated with the raw LifeCycle state buffer.
     * @lc_valid if not NULL, update with the LC valid state (scalar)
     * @secret_valid if not NULL, update with the LC secret_valid info (scalar)
     *
     * @note: lc_valid and secret_valid use OT_MULTIBITBOOL_LC4 encoding
     */
    void (*get_lc_info)(const OtOTPIf *dev, uint16_t *lc_state,
                        uint16_t *lc_tcount, uint8_t *lc_valid,
                        uint8_t *secret_valid, const OtOTPTokens **tokens);

    /*
     * Retrieve HW configuration.
     *
     * @dev the OTP device
     * @return the HW config data (never NULL)
     */
    const OtOTPHWCfg *(*get_hw_cfg)(const OtOTPIf *dev);

    /*
     * Retrieve SRAM scrambling key.
     *
     * @dev the OTP device
     * @type the type of the key to retrieve
     * @key the key record to update
     */
    void (*get_otp_key)(OtOTPIf *dev, OtOTPKeyType type, OtOTPKey *key);

    /*
     * Retrieve Key Manager secret (key or seeds).
     *
     * @dev the OTP device
     * @type the type of secret to retrieve
     * @secret the key manager secret record to update
     */
    void (*get_keymgr_secret)(OtOTPIf *dev, OtOTPKeyMgrSecretType type,
                              OtOTPKeyMgrSecret *secret);

    /**
     * Request the OTP to program the state, transition count pair.
     * OTP only accepts one request at a time. If another program request is
     * on going, this function returns immediately and never invoke the
     * callback. Conversely, it should always invoke the callback if the request
     * is accepted.
     *
     * @dev the OTP device
     * @lc_tcount the raw LifeCycle transition count buffer
     * @lc_state the raw LifeCycle state buffer
     * @ack the callback to asynchronously invoke on OTP completion/error
     * @opaque opaque data to forward to the ot_otp_program_ack_fn function
     * @return @c true if request is accepted, @c false is rejected.
     */
    bool (*program_req)(OtOTPIf *dev, const uint16_t *lc_tcount,
                        const uint16_t *lc_state, ot_otp_program_ack_fn ack,
                        void *opaque);
};

#endif /* HW_OPENTITAN_OT_OTP_IF_H */
