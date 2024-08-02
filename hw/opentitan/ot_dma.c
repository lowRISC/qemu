/*
 * QEMU OpenTitan DMA device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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
 * Limitations: only MEM-to-MEM operations (including SHA hashing) are
 *              supported. "Handshake" (i.e. DEVICE/FIFO operations) are not
 *              supported, nor planned.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_address_space.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_dma.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/dma.h"
#include "tomcrypt.h"
#include "trace.h"

#define PARAM_NUM_INT_CLEAR_SRCS 11u
#define PARAM_NUM_IRQS           3u
#define PARAM_NUM_ALERTS         1u

/* clang-format off */

REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(INTR_DMA_DONE, 0u, 1u)
    SHARED_FIELD(INTR_DMA_ERROR, 1u, 1u)
    SHARED_FIELD(INTR_DMA_MEM_BUF_LIMIT, 2u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(SRC_ADDR_LO, 0x10u)
REG32(SRC_ADDR_HI, 0x14u)
REG32(DEST_ADDR_LO, 0x18u)
REG32(DEST_ADDR_HI, 0x1cu)
REG32(ADDR_SPACE_ID, 0x20u)
    FIELD(ADDR_SPACE_ID, SRC, 0u, 4u)
    FIELD(ADDR_SPACE_ID, DEST, 4u, 4u)
REG32(ENABLED_MEMORY_RANGE_BASE, 0x24u)
REG32(ENABLED_MEMORY_RANGE_LIMIT, 0x28u)
REG32(RANGE_VALID, 0x2cu)
    FIELD(RANGE_VALID, VALID, 0u, 1u)
REG32(RANGE_REGWEN, 0x30u)
    FIELD(RANGE_REGWEN, EN, 0u, 4u)
REG32(CFG_REGWEN, 0x34u)
    FIELD(CFG_REGWEN, EN, 0u, 4u)
REG32(TOTAL_DATA_SIZE, 0x38u)
REG32(CHUNK_DATA_SIZE, 0x3cu)
REG32(TRANSFER_WIDTH, 0x40u)
    FIELD(TRANSFER_WIDTH, WIDTH, 0u, 2u)
REG32(DEST_ADDR_LIMIT_LO, 0x44u)
REG32(DEST_ADDR_LIMIT_HI, 0x48u)
REG32(DEST_ADDR_THRESHOLD_LO, 0x4cu)
REG32(DEST_ADDR_THRESHOLD_HI, 0x50u)
REG32(CONTROL, 0x54u)
    FIELD(CONTROL, OPCODE, 0u, 4u)
    FIELD(CONTROL, HW_HANDSHAKE_EN, 4u, 1u)
    FIELD(CONTROL, MEM_BUF_AUTO_INC_EN, 5u, 1u)
    FIELD(CONTROL, FIFO_AUTO_INC_EN, 6u, 1u)
    FIELD(CONTROL, DATA_DIR, 7u, 1u)
    FIELD(CONTROL, INITIAL_TRANSFER, 8u, 1u)
    FIELD(CONTROL, ABORT, 27u, 1u)
    FIELD(CONTROL, GO, 31u, 1u)
REG32(STATUS, 0x58u)
    FIELD(STATUS, BUSY, 0u, 1u)
    FIELD(STATUS, DONE, 1u, 1u)
    FIELD(STATUS, ABORTED, 2u, 1u)
    FIELD(STATUS, ERROR, 3u, 1u)
    FIELD(STATUS, SHA2_DIGEST_VALID, 4u, 1u)
REG32(ERROR_CODE, 0x5cu)
    FIELD(ERROR_CODE, SRC_ADDRESS, 0u, 1u)
    FIELD(ERROR_CODE, DST_ADDRESS, 1u, 1u)
    FIELD(ERROR_CODE, OPCODE, 2u, 1u)
    FIELD(ERROR_CODE, SIZE, 3u, 1u)
    FIELD(ERROR_CODE, BUS, 4u, 1u)
    FIELD(ERROR_CODE, BASE_LIMIT, 5u, 1u)
    FIELD(ERROR_CODE, RANGE_VALID, 6u, 1u)
    FIELD(ERROR_CODE, ASID, 7u, 1u)
REG32(SHA2_DIGEST_0, 0x60u)
REG32(SHA2_DIGEST_1, 0x64u)
REG32(SHA2_DIGEST_2, 0x68u)
REG32(SHA2_DIGEST_3, 0x6cu)
REG32(SHA2_DIGEST_4, 0x70u)
REG32(SHA2_DIGEST_5, 0x74u)
REG32(SHA2_DIGEST_6, 0x78u)
REG32(SHA2_DIGEST_7, 0x7cu)
REG32(SHA2_DIGEST_8, 0x80u)
REG32(SHA2_DIGEST_9, 0x84u)
REG32(SHA2_DIGEST_10, 0x88u)
REG32(SHA2_DIGEST_11, 0x8cu)
REG32(SHA2_DIGEST_12, 0x90u)
REG32(SHA2_DIGEST_13, 0x94u)
REG32(SHA2_DIGEST_14, 0x98u)
REG32(SHA2_DIGEST_15, 0x9cu)
REG32(HANDSHAKE_INTR, 0xa0u)
    FIELD(HANDSHAKE_INTR, ENABLE, 0, PARAM_NUM_INT_CLEAR_SRCS)
REG32(CLEAR_INT_SRC, 0xa4u)
    FIELD(CLEAR_INT_SRC, ENABLE, 0, PARAM_NUM_INT_CLEAR_SRCS)
REG32(CLEAR_INT_BUS, 0xa8u)
    FIELD(CLEAR_INT_BUS, ENABLE, 0, PARAM_NUM_INT_CLEAR_SRCS)
REG32(INT_SRC_ADDR_0, 0xacu)
REG32(INT_SRC_ADDR_1, 0xb0u)
REG32(INT_SRC_ADDR_2, 0xb4u)
REG32(INT_SRC_ADDR_3, 0xb8u)
REG32(INT_SRC_ADDR_4, 0xbcu)
REG32(INT_SRC_ADDR_5, 0xc0u)
REG32(INT_SRC_ADDR_6, 0xc4u)
REG32(INT_SRC_ADDR_7, 0xc8u)
REG32(INT_SRC_ADDR_8, 0xccu)
REG32(INT_SRC_ADDR_9, 0xd0u)
REG32(INT_SRC_ADDR_10, 0xd8u)
REG32(INT_SRC_WR_VAL_0, 0x12cu)
REG32(INT_SRC_WR_VAL_1, 0x130u)
REG32(INT_SRC_WR_VAL_2, 0x134u)
REG32(INT_SRC_WR_VAL_3, 0x138u)
REG32(INT_SRC_WR_VAL_4, 0x13cu)
REG32(INT_SRC_WR_VAL_5, 0x140u)
REG32(INT_SRC_WR_VAL_6, 0x144u)
REG32(INT_SRC_WR_VAL_7, 0x148u)
REG32(INT_SRC_WR_VAL_8, 0x14cu)
REG32(INT_SRC_WR_VAL_9, 0x150u)
REG32(INT_SRC_WR_VAL_10, 0x154u)

#if defined(MEMTXATTRS_HAS_ROLE) && (MEMTXATTRS_HAS_ROLE != 0)
#define OT_DMA_HAS_ROLE
#else
#undef OT_DMA_HAS_ROLE
#endif

typedef enum {
    TRANSACTION_WIDTH_BYTE = 0x0,
    TRANSACTION_WIDTH_HALF = 0x1,
    TRANSACTION_WIDTH_WORD = 0x2,
    TRANSACTION_WIDTH_ERROR = 0x3,
} OtDMATransferWidth;

typedef enum {
    OPCODE_COPY = 0x0,
    OPCODE_COPY_SHA256 = 0x1,
    OPCODE_COPY_SHA384 = 0x2,
    OPCODE_COPY_SHA512 = 0x3,
} OtDMAOpcode;

typedef enum {
    ASID_OT = 0x7,
    ASID_CTRL = 0xa,
    ASID_SYS = 0x9,
} OtDMAAddrSpaceId;

typedef enum {
    ERR_SRC_ADDR,
    ERR_DEST_ADDR,
    ERR_OPCODE,
    ERR_SIZE,
    ERR_BUS,
    ERR_BASE_LIMIT,
    ERR_RANGE_VALID,
    ERR_ASID,
    ERR_COUNT
} OtDMAError;

typedef enum {
    AS_OT,
    AS_CTN,
    AS_SYS,
    AS_COUNT,
    AS_INVALID = AS_COUNT
} OtDMAAddrSpace;

typedef enum {
    SM_IDLE,
    SM_CLEAR_INTR_SRC,
    SM_WAIT_INTR_SRC_RESP,
    SM_ADDR_SETUP,
    SM_SEND_READ,
    SM_WAIT_READ_RESP,
    SM_SEND_WRITE,
    SM_WAIT_WRITE_RESP,
    SM_ERROR,
    SM_SHA_FINALIZE,
    SM_SHA_WAIT,
} OtDMASM;

/* clang-format on */

typedef struct {
    AddressSpace *as;
    OtDMAAddrSpace asix;
    MemoryRegion *mr;
    void *buf;
    dma_addr_t addr;
    dma_addr_t size;
    MemTxAttrs attrs;
    MemTxResult res;
    bool write;
} OtDMAOp;

typedef struct {
    hash_state state;
    const struct ltc_hash_descriptor *desc;
} OtDMASHA;

struct OtDMAState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ irqs[PARAM_NUM_IRQS];
    IbexIRQ alerts[PARAM_NUM_ALERTS];
    AddressSpace *ases[AS_COUNT];
    QEMUTimer *timer;

    OtDMASM state;
    OtDMAOp op;
    OtDMASHA sha;
    uint32_t *regs;

    char *ot_id;
    char *ot_as_name; /* private AS unique name */
    char *ctn_as_name; /* externel port AS unique name */
    char *sys_as_name; /* external system AS unique name */
#ifdef OT_DMA_HAS_ROLE
    uint8_t role;
#endif
};

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_INT_SRC_WR_VAL_10)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define INTR_MASK \
    (INTR_DMA_DONE_MASK | INTR_DMA_ERROR_MASK | INTR_DMA_MEM_BUF_LIMIT_MASK)
#define ALERT_TEST_MASK R_ALERT_TEST_FATAL_FAULT_MASK
#define CONTROL_MASK \
    (R_CONTROL_OPCODE_MASK | R_CONTROL_HW_HANDSHAKE_EN_MASK | \
     R_CONTROL_MEM_BUF_AUTO_INC_EN_MASK | R_CONTROL_FIFO_AUTO_INC_EN_MASK | \
     R_CONTROL_DATA_DIR_MASK | R_CONTROL_INITIAL_TRANSFER_MASK | \
     R_CONTROL_ABORT_MASK | R_CONTROL_GO_MASK)

#define DMA_ERROR(_err_) (1u << (_err_))

/* the following values are arbitrary end may be changed if needed */
#define DMA_PACE_NS             10000u /* 10us: slow down DMA, handle aborts */
#define DMA_TRANSFER_BLOCK_SIZE 4096u /* size of a single DMA block */

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(SRC_ADDR_LO),
    REG_NAME_ENTRY(SRC_ADDR_HI),
    REG_NAME_ENTRY(DEST_ADDR_LO),
    REG_NAME_ENTRY(DEST_ADDR_HI),
    REG_NAME_ENTRY(ADDR_SPACE_ID),
    REG_NAME_ENTRY(ENABLED_MEMORY_RANGE_BASE),
    REG_NAME_ENTRY(ENABLED_MEMORY_RANGE_LIMIT),
    REG_NAME_ENTRY(RANGE_VALID),
    REG_NAME_ENTRY(RANGE_REGWEN),
    REG_NAME_ENTRY(CFG_REGWEN),
    REG_NAME_ENTRY(TOTAL_DATA_SIZE),
    REG_NAME_ENTRY(CHUNK_DATA_SIZE),
    REG_NAME_ENTRY(TRANSFER_WIDTH),
    REG_NAME_ENTRY(DEST_ADDR_LIMIT_LO),
    REG_NAME_ENTRY(DEST_ADDR_LIMIT_HI),
    REG_NAME_ENTRY(DEST_ADDR_THRESHOLD_LO),
    REG_NAME_ENTRY(DEST_ADDR_THRESHOLD_HI),
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(ERROR_CODE),
    REG_NAME_ENTRY(SHA2_DIGEST_0),
    REG_NAME_ENTRY(SHA2_DIGEST_1),
    REG_NAME_ENTRY(SHA2_DIGEST_2),
    REG_NAME_ENTRY(SHA2_DIGEST_3),
    REG_NAME_ENTRY(SHA2_DIGEST_4),
    REG_NAME_ENTRY(SHA2_DIGEST_5),
    REG_NAME_ENTRY(SHA2_DIGEST_6),
    REG_NAME_ENTRY(SHA2_DIGEST_7),
    REG_NAME_ENTRY(SHA2_DIGEST_8),
    REG_NAME_ENTRY(SHA2_DIGEST_9),
    REG_NAME_ENTRY(SHA2_DIGEST_10),
    REG_NAME_ENTRY(SHA2_DIGEST_11),
    REG_NAME_ENTRY(SHA2_DIGEST_12),
    REG_NAME_ENTRY(SHA2_DIGEST_13),
    REG_NAME_ENTRY(SHA2_DIGEST_14),
    REG_NAME_ENTRY(SHA2_DIGEST_15),
    REG_NAME_ENTRY(HANDSHAKE_INTR),
    REG_NAME_ENTRY(CLEAR_INT_SRC),
    REG_NAME_ENTRY(CLEAR_INT_BUS),
    REG_NAME_ENTRY(INT_SRC_ADDR_0),
    REG_NAME_ENTRY(INT_SRC_ADDR_1),
    REG_NAME_ENTRY(INT_SRC_ADDR_2),
    REG_NAME_ENTRY(INT_SRC_ADDR_3),
    REG_NAME_ENTRY(INT_SRC_ADDR_4),
    REG_NAME_ENTRY(INT_SRC_ADDR_5),
    REG_NAME_ENTRY(INT_SRC_ADDR_6),
    REG_NAME_ENTRY(INT_SRC_ADDR_7),
    REG_NAME_ENTRY(INT_SRC_ADDR_8),
    REG_NAME_ENTRY(INT_SRC_ADDR_9),
    REG_NAME_ENTRY(INT_SRC_ADDR_10),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_0),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_1),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_2),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_3),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_4),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_5),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_6),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_7),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_8),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_9),
    REG_NAME_ENTRY(INT_SRC_WR_VAL_10),
};
#undef REG_NAME_ENTRY

#define AS_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
static const char *AS_NAMES[] = {
    AS_NAME_ENTRY(AS_OT),
    AS_NAME_ENTRY(AS_CTN),
    AS_NAME_ENTRY(AS_SYS),
};
#undef AS_NAME_ENTRY
#define AS_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(AS_NAMES) ? AS_NAMES[(_st_)] : "?")

#define STATE_NAME_ENTRY(_st_) [_st_] = stringify(_st_)
/* clang-format off */
static const char *STATE_NAMES[] = {
    STATE_NAME_ENTRY(SM_IDLE),
    STATE_NAME_ENTRY(SM_CLEAR_INTR_SRC),
    STATE_NAME_ENTRY(SM_WAIT_INTR_SRC_RESP),
    STATE_NAME_ENTRY(SM_ADDR_SETUP),
    STATE_NAME_ENTRY(SM_SEND_READ),
    STATE_NAME_ENTRY(SM_WAIT_READ_RESP),
    STATE_NAME_ENTRY(SM_SEND_WRITE),
    STATE_NAME_ENTRY(SM_WAIT_WRITE_RESP),
    STATE_NAME_ENTRY(SM_ERROR),
    STATE_NAME_ENTRY(SM_SHA_FINALIZE),
    STATE_NAME_ENTRY(SM_SHA_WAIT),
};
/* clang-format on */
#undef STATE_NAME_ENTRY

#define STATE_NAME(_st_) \
    ((_st_) >= 0 && (_st_) < ARRAY_SIZE(STATE_NAMES) ? STATE_NAMES[(_st_)] : \
                                                       "?")

#define CHANGE_STATE(_f_, _sst_) \
    ot_dma_change_state_line(_f_, SM_##_sst_, __LINE__)

#define ot_dma_set_xerror(_s_, _err_) \
    trace_ot_dma_set_error((_s_)->ot_id, __func__, __LINE__, \
                           DMA_ERROR(_err_)); \
    ot_dma_set_error(_s_, (_err_))


/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static void ot_dma_change_state_line(OtDMAState *s, OtDMASM state, int line)
{
    if (s->state != state) {
        trace_ot_dma_change_state(s->ot_id, line, STATE_NAME(state), state);
        s->state = state;
    }
}

static void ot_dma_update_irqs(OtDMAState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_dma_irqs(s->ot_id, s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE],
                      level);
    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_irq_set(&s->irqs[ix], (int)((level >> ix) & 0x1u));
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

static hwaddr ot_dma_get_src_address(const OtDMAState *s)
{
    return (hwaddr)s->regs[R_SRC_ADDR_LO] |
           (((hwaddr)s->regs[R_SRC_ADDR_HI]) << 32u);
}

static hwaddr ot_dma_get_dest_address(const OtDMAState *s)
{
    return (hwaddr)s->regs[R_DEST_ADDR_LO] |
           (((hwaddr)s->regs[R_DEST_ADDR_HI]) << 32u);
}

static unsigned ot_dma_get_src_asid(const OtDMAState *s)
{
    return (unsigned)FIELD_EX32(s->regs[R_ADDR_SPACE_ID], ADDR_SPACE_ID, SRC);
}

static unsigned ot_dma_get_dest_asid(const OtDMAState *s)
{
    return (unsigned)FIELD_EX32(s->regs[R_ADDR_SPACE_ID], ADDR_SPACE_ID, DEST);
}

static hwaddr ot_dma_get_dest_limit_address(const OtDMAState *s)
{
    return (hwaddr)s->regs[R_DEST_ADDR_LIMIT_LO] |
           (((hwaddr)s->regs[R_DEST_ADDR_LIMIT_HI]) << 32u);
}

static hwaddr ot_dma_get_dest_threshold_address(const OtDMAState *s)
{
    return (hwaddr)s->regs[R_DEST_ADDR_THRESHOLD_LO] |
           (((hwaddr)s->regs[R_DEST_ADDR_THRESHOLD_HI]) << 32u);
}

static bool ot_dma_is_range_validated(const OtDMAState *s)
{
    return (bool)(s->regs[R_RANGE_VALID] & R_RANGE_VALID_VALID_MASK);
}

static bool ot_dma_is_range_locked(const OtDMAState *s)
{
    return s->regs[R_RANGE_REGWEN] != OT_MULTIBITBOOL4_TRUE;
}

static bool ot_dma_is_busy(const OtDMAState *s)
{
    return (bool)(s->regs[R_STATUS] & R_STATUS_BUSY_MASK);
}

static bool ot_dma_is_configurable(const OtDMAState *s)
{
    return !ot_dma_is_busy(s);
}

static bool ot_dma_is_on_error(const OtDMAState *s, unsigned err)
{
    g_assert(err < ERR_COUNT);

    return ((bool)(s->regs[R_STATUS] & R_STATUS_ERROR_MASK)) &&
           ((bool)(s->regs[R_ERROR_CODE] & DMA_ERROR(err)));
}

static void ot_dma_set_error(OtDMAState *s, unsigned err)
{
    g_assert(err < ERR_COUNT);

    s->regs[R_STATUS] |= R_STATUS_ERROR_MASK;
    s->regs[R_ERROR_CODE] |= DMA_ERROR(err);
    s->regs[R_INTR_STATE] |= INTR_DMA_ERROR_MASK;

    CHANGE_STATE(s, ERROR);

    ot_dma_update_irqs(s);
}

static void ot_dma_check_range(OtDMAState *s, bool d_or_s, bool cross_ot)
{
    uint32_t lstart = s->regs[R_ENABLED_MEMORY_RANGE_BASE];
    uint32_t lend = s->regs[R_ENABLED_MEMORY_RANGE_LIMIT];

    if (lstart > lend) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s DMA invalid range\n",
                      __func__, s->ot_id, d_or_s ? "Dest" : "Src");
        ot_dma_set_xerror(s, ERR_BASE_LIMIT);
        return;
    }

    uint32_t tsize = s->regs[R_TOTAL_DATA_SIZE];
    /* *_ADDR_HI ignored here, SBZ */
    uint32_t tstart = s->regs[d_or_s ? R_DEST_ADDR_LO : R_SRC_ADDR_LO];
    uint32_t tend = tstart + tsize;

    if (!cross_ot) {
        /* no check performed if transfer does not cross OT boundary */
        return;
    }

    if (tend < tstart) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s DMA end overflow\n",
                      __func__, s->ot_id, d_or_s ? "Dest" : "Src");
        ot_dma_set_xerror(s, ERR_SIZE);
        return;
    }

    if (tstart < lstart) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: %s DMA starts in prohibited region "
                      "0x%08x < 0x%08x\n",
                      __func__, s->ot_id, d_or_s ? "Dest" : "Src", tstart,
                      lstart);
        ot_dma_set_xerror(s, d_or_s ? ERR_DEST_ADDR : ERR_SRC_ADDR);
        return;
    }

    if (tend > lend) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: %s DMA ends in prohibited region "
                      "0x%08x > 0x%08x\n",
                      __func__, s->ot_id, d_or_s ? "Dest" : "Src", tend, lend);
        ot_dma_set_xerror(s, d_or_s ? ERR_DEST_ADDR : ERR_SRC_ADDR);
        return;
    }
}

static OtDMAAddrSpace ot_dma_get_asid(OtDMAState *s, bool d_or_s)
{
    uint32_t asid = d_or_s ? ot_dma_get_dest_asid(s) : ot_dma_get_src_asid(s);

    switch (asid) {
    case ASID_OT:
        return AS_OT;
    case ASID_CTRL:
        return AS_CTN;
        break;
    case ASID_SYS:
        return AS_SYS;
    default:
        return AS_COUNT;
    }
}

static MemoryRegion *
ot_dma_check_device(OtDMAState *s, bool d_or_s, OtDMAAddrSpace *asix,
                    bool *is_dev, hwaddr *offset)
{
    OtDMAAddrSpace aix = ot_dma_get_asid(s, d_or_s);
    if (aix >= AS_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid address space\n",
                      __func__, s->ot_id);
        ot_dma_set_xerror(s, ERR_ASID);
        return NULL;
    }

    if (!s->ases[aix]) {
        error_setg(&error_fatal, "%s: %s address space not configured",
                   s->ot_id, AS_NAME(aix));
        return NULL;
    }

    AddressSpace *as = s->ases[aix];

    hwaddr start =
        d_or_s ? ot_dma_get_dest_address(s) : ot_dma_get_src_address(s);
    hwaddr size = s->regs[R_TOTAL_DATA_SIZE];

    MemoryRegionSection mrs = { 0 };
    mrs = memory_region_find(as->root, start, size);

    if (!mrs.mr || !int128_getlo(mrs.size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid %s address as:%s "
                      "addr: 0x%" HWADDR_PRIx " size: 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, d_or_s ? "dest" : "src", AS_NAME(aix),
                      start, size);
        ot_dma_set_xerror(s, d_or_s ? ERR_DEST_ADDR : ERR_SRC_ADDR);
        return NULL;
    }

    if (mrs.offset_within_region + int128_getlo(mrs.size) < size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid size\n", __func__,
                      s->ot_id);
        ot_dma_set_xerror(s, ERR_SIZE);
        memory_region_unref(mrs.mr);
        return NULL;
    }

    trace_ot_dma_check_device(s->ot_id, d_or_s ? "Dest" : "Src", AS_NAME(aix),
                              start, size, mrs.mr->name, mrs.mr->ram);
    *asix = aix;
    *is_dev = !mrs.mr->ram;
    *offset = mrs.offset_within_region;

    /* caller should invoke memory_region_unref(mrs.mr) once done with mr */
    return mrs.mr;
}

static bool ot_dma_go(OtDMAState *s)
{
    /*
     * error checking follows HW: errors are accumulated, not rejected on first
     * detected one.
     */

    switch (s->regs[R_TRANSFER_WIDTH]) {
    case TRANSACTION_WIDTH_BYTE:
    case TRANSACTION_WIDTH_HALF:
    case TRANSACTION_WIDTH_WORD:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid transaction width for hashing\n",
                      __func__, s->ot_id);
        ot_dma_set_xerror(s, ERR_SIZE);
    }

    /* DEVICE mode not yet supported */
    if (FIELD_EX32(s->regs[R_CONTROL], CONTROL, HW_HANDSHAKE_EN)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: Handshake mode is not supported\n",
                      __func__, s->ot_id);
        ot_dma_set_xerror(s, ERR_BUS);
    }

    if (s->regs[R_TOTAL_DATA_SIZE] == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid total size\n", __func__,
                      s->ot_id);
        ot_dma_set_xerror(s, ERR_SIZE);
    }

    if (s->regs[R_CHUNK_DATA_SIZE] == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid chunk size\n", __func__,
                      s->ot_id);
        ot_dma_set_xerror(s, ERR_SIZE);
    }

    if (s->regs[R_TOTAL_DATA_SIZE] != s->regs[R_CHUNK_DATA_SIZE]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Chunk size differs from total size\n", __func__,
                      s->ot_id);
        ot_dma_set_xerror(s, ERR_SIZE);
    }

    bool init_tf = (bool)(s->regs[R_CONTROL] & R_CONTROL_INITIAL_TRANSFER_MASK);
    unsigned sha_mode = FIELD_EX32(s->regs[R_CONTROL], CONTROL, OPCODE);
    const struct ltc_hash_descriptor *desc;
    switch (sha_mode) {
    case OPCODE_COPY:
        trace_ot_dma_operation("copy", init_tf);
        desc = NULL;
        break;
    case OPCODE_COPY_SHA256:
        trace_ot_dma_operation("sha256", init_tf);
        desc = &sha256_desc;
        break;
    case OPCODE_COPY_SHA384:
        trace_ot_dma_operation("sha384", init_tf);
        desc = &sha384_desc;
        break;
    case OPCODE_COPY_SHA512:
        trace_ot_dma_operation("sha512", init_tf);
        desc = &sha512_desc;
        break;
    default:
        desc = NULL;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid opcode %u\n", __func__,
                      s->ot_id, sha_mode);
        ot_dma_set_xerror(s, ERR_OPCODE);
    }

    if (desc) {
        if (s->regs[R_TRANSFER_WIDTH] != TRANSACTION_WIDTH_WORD) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Invalid transaction width for hashing\n",
                          __func__, s->ot_id);
            ot_dma_set_xerror(s, ERR_SIZE);
        }
    }

    OtDMASHA *sha = &s->sha;
    if (init_tf) {
        sha->desc = desc;
        s->regs[R_STATUS] &= ~R_STATUS_SHA2_DIGEST_VALID_MASK;
        if (sha->desc) {
            int res = sha->desc->init(&sha->state);
            g_assert(res == CRYPT_OK);
        }
    } else if (sha->desc != desc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: SHA mode change w/o initial transfer\n", __func__);
    }

    MemoryRegion *smr;
    hwaddr soffset = HWADDR_MAX;
    OtDMAAddrSpace sasix = AS_INVALID;
    bool sdev = false;
    smr = ot_dma_check_device(s, false, &sasix, &sdev, &soffset);

    MemoryRegion *dmr;
    hwaddr doffset = HWADDR_MAX;
    OtDMAAddrSpace dasix = AS_INVALID;
    bool ddev = false;
    dmr = ot_dma_check_device(s, true, &dasix, &ddev, &doffset);

    /*
     * Some src/dest combinations are not supported for now.
     * Transfer from external 64-bit memory to external 64-bit memory would
     * require a more complex implementation with two transfers with QEMU APIs
     * on the 32 bit machine such as OT:
     *  -1 from the source memory to a temporary buffer
     *  -2 from the temporary buffer to the destination memory
     * The temporary buffer would be an artifact for QEMU.
     * This feature is not supported for now
     */
    if ((sasix == AS_SYS) && (dasix == AS_SYS)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: SYS-to-SYS is not supported\n",
                      __func__, s->ot_id);
        ot_dma_set_xerror(s, ERR_ASID);
    }

    ot_dma_check_range(s, false, sasix == AS_OT && dasix != AS_OT);

    ot_dma_check_range(s, true, sasix != AS_OT && dasix == AS_OT);

    if (sdev && ddev) {
        /* could be done w/ an intermediate buffer, but likely useless */
        qemu_log_mask(LOG_UNIMP, "%s: %s: DEV-to-DEV is not supported\n",
                      __func__, s->ot_id);
        ot_dma_set_xerror(s, ERR_BUS);
    }

    OtDMAOp *op = &s->op;

    if (s->state != SM_ERROR) {
        op->attrs.unspecified = false;
#ifdef OT_DMA_HAS_ROLE
        op->attrs.role = (unsigned)s->role;
#endif
        op->size = s->regs[R_TOTAL_DATA_SIZE];
    }

    /*
     * The emulation ignores the transfer width as this is already managed
     * with QEMU, see flatview_read_continue and flatview_write_continue.
     * QEMU performs the best depending on the maximum transfer width as
     * reported by the device region with is copied.
     *
     * Here nevertheless checks that requested transfer width is not larger
     * than the maximum width supported by the emulated device and rejects the
     * transfer if the requested width is not coherent with the device.
     *
     * This also means that the targetted device needs to provide the proper
     * width for DMA-able registers so that there is no aligment/stride issues.
     */
    unsigned twidth = s->regs[R_TRANSFER_WIDTH];
    uint32_t tmask = (1u << twidth) - 1u;

    if (!ot_dma_is_on_error(s, ERR_SRC_ADDR) && (soffset & tmask)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Src 0x%" HWADDR_PRIx
                      " not aligned on TRANSFER_WIDTH\n",
                      __func__, s->ot_id, soffset);
        ot_dma_set_xerror(s, ERR_SRC_ADDR);
    }

    if (!ot_dma_is_on_error(s, ERR_DEST_ADDR) && (doffset & tmask)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Dest 0x%" HWADDR_PRIx
                      " not aligned on TRANSFER_WIDTH\n",
                      __func__, s->ot_id, doffset);
        ot_dma_set_xerror(s, ERR_DEST_ADDR);
    }

    if (!ot_dma_is_on_error(s, ERR_SRC_ADDR) && (sasix != AS_SYS)) {
        if (s->regs[R_SRC_ADDR_HI] != 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Src address is too large\n",
                          __func__, s->ot_id);
            ot_dma_set_xerror(s, ERR_SRC_ADDR);
        }
    }

    if (!ot_dma_is_on_error(s, ERR_DEST_ADDR) && (dasix != AS_SYS)) {
        if (s->regs[R_DEST_ADDR_HI] != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: Dest address is too large\n", __func__,
                          s->ot_id);
            ot_dma_set_xerror(s, ERR_DEST_ADDR);
        }
    }

    if (!ot_dma_is_range_validated(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Memory range not validated\n",
                      __func__, s->ot_id);
        ot_dma_set_xerror(s, ERR_RANGE_VALID);
    }

    if (sdev && smr) {
        /* check first and last slots of the requested transfer */
        int swidth = memory_access_size(smr, sizeof(uint32_t), soffset);
        int ewidth = memory_access_size(smr, sizeof(uint32_t),
                                        soffset + op->size - sizeof(uint32_t));
        int dwidth = MIN(swidth, ewidth);
        if (!ot_dma_is_on_error(s, ERR_SRC_ADDR) && (dwidth < (int)twidth)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: Src device does not supported "
                          "requested width: %u, max %d\n",
                          __func__, s->ot_id, twidth, dwidth);
            ot_dma_set_xerror(s, ERR_SRC_ADDR);
        }
    }
    if (ddev && dmr) {
        /* check first and last slots of the requested transfer */
        int swidth = memory_access_size(dmr, sizeof(uint32_t), doffset);
        int ewidth = memory_access_size(dmr, sizeof(uint32_t),
                                        doffset + op->size - sizeof(uint32_t));
        int dwidth = MIN(swidth, ewidth);
        if (!ot_dma_is_on_error(s, ERR_DEST_ADDR) && (dwidth < (int)twidth)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: Dest device does not supported "
                          "requested width: %u, max %d\n",
                          __func__, s->ot_id, twidth, dwidth);
            ot_dma_set_xerror(s, ERR_DEST_ADDR);
        }
    }

    if (s->state == SM_ERROR) {
        if (smr) {
            memory_region_unref(smr);
        }
        if (dmr) {
            memory_region_unref(dmr);
        }
        return false;
    }

    /*
     * src = dev, dest = mem -> read dev, write mem: read
     * src = mem, dest = dev -> read mem, write dev: write
     * src = mem, dest = mem -> read mem, write mem: write
     * src = dev, dest = dev -> not yet supported
     */
    op->write = !sdev;

    if (op->write) {
        op->as = s->ases[dasix];
        op->asix = dasix;
        op->addr = ot_dma_get_dest_address(s);
        op->mr = smr;
        uintptr_t smem = (uintptr_t)memory_region_get_ram_ptr(smr);
        smem += soffset;
        op->buf = (void *)smem;
        memory_region_unref(dmr);
    } else {
        op->addr = ot_dma_get_src_address(s);
        op->as = s->ases[sasix];
        op->asix = sasix;
        op->mr = dmr;
        uintptr_t dmem = (uintptr_t)memory_region_get_ram_ptr(dmr);
        dmem += doffset;
        op->buf = (void *)dmem;
        memory_region_unref(smr);
    }

    g_assert(op->as);

    trace_ot_dma_new_op(s->ot_id, op->write ? "write" : "read",
                        AS_NAME(op->asix), op->mr->name, op->addr, op->size);

    s->regs[R_STATUS] &=
        ~(R_STATUS_DONE_MASK | R_STATUS_ABORTED_MASK | R_STATUS_ERROR_MASK);
    s->regs[R_ERROR_CODE] = 0;
    s->regs[R_STATUS] |= R_STATUS_BUSY_MASK;

    timer_del(s->timer);
    uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    timer_mod(s->timer, (int64_t)(now + DMA_PACE_NS));

    return true;
}

static void ot_dma_abort(OtDMAState *s)
{
    if (!ot_dma_is_busy(s)) {
        /* nothing to do, but ABORTED be signaled? */
        return;
    }

    trace_ot_dma_abort(s->ot_id);

    s->regs[R_CONTROL] |= R_CONTROL_ABORT_MASK;

    /* simulate a delayed response */
    timer_del(s->timer);
    uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    timer_mod(s->timer, (int64_t)(now + DMA_PACE_NS));
}

static void ot_dma_complete(OtDMAState *s)
{
    OtDMAOp *op = &s->op;

    s->regs[R_STATUS] &= ~R_STATUS_BUSY_MASK;

    if (s->regs[R_CONTROL] & R_CONTROL_ABORT_MASK) {
        s->regs[R_CONTROL] &= ~R_CONTROL_ABORT_MASK;
        s->regs[R_STATUS] |= R_STATUS_ABORTED_MASK;
        s->regs[R_INTR_STATE] |= INTR_DMA_ERROR_MASK;

        trace_ot_dma_complete(s->ot_id, -1);

        CHANGE_STATE(s, IDLE);
    } else if (s->regs[R_CONTROL] & R_CONTROL_GO_MASK) {
        if (!FIELD_EX32(s->regs[R_CONTROL], CONTROL, HW_HANDSHAKE_EN)) {
            s->regs[R_CONTROL] &= ~R_CONTROL_GO_MASK;
        }

        trace_ot_dma_complete(s->ot_id, (int)op->res);

        if (op->mr) {
            memory_region_unref(op->mr);
            op->mr = NULL;
        } else {
            g_assert_not_reached();
        }

        if (op->size) {
            g_assert_not_reached();
        }

        switch (op->res) {
        case MEMTX_OK:
            s->regs[R_STATUS] |= R_STATUS_DONE_MASK;
            break;
        /* device returned an error */
        case MEMTX_ERROR:
            ot_dma_set_xerror(s, ERR_BUS);
            return;
        /* nothing at that address */
        case MEMTX_DECODE_ERROR:
            ot_dma_set_xerror(s, op->write ? ERR_DEST_ADDR : ERR_SRC_ADDR);
            return;
        /* access denied */
        case MEMTX_ACCESS_ERROR:
            ot_dma_set_xerror(s, ERR_BUS);
            return;
        default:
            g_assert_not_reached();
        }

        OtDMASHA *sha = &s->sha;
        if (sha->desc) {
            uint32_t md[64u / sizeof(uint32_t)];
            int res = sha->desc->done(&sha->state, (uint8_t *)md);
            g_assert(res == CRYPT_OK);

            unsigned md_count = sha->desc->hashsize / sizeof(uint32_t);
            for (unsigned ix = 0; ix < md_count; ix++) {
                // it is likely some shuffling (little endian, etc) is required
                // here, but for now the bit order of the HW is not known.
                s->regs[R_SHA2_DIGEST_0 + ix] = md[ix];
            }

            s->regs[R_STATUS] |= R_STATUS_SHA2_DIGEST_VALID_MASK;

            sha->desc = NULL;
        }

        s->regs[R_INTR_STATE] |= INTR_DMA_DONE_MASK;

        CHANGE_STATE(s, IDLE);
    } else {
        g_assert_not_reached();
    }

    ot_dma_update_irqs(s);
}

static void ot_dma_transfer(void *opaque)
{
    OtDMAState *s = opaque;

    if (!(s->regs[R_CONTROL] & R_CONTROL_ABORT_MASK)) {
        OtDMAOp *op = &s->op;

        g_assert(op->mr != NULL);

        smp_mb();

        hwaddr size = MIN(op->size, DMA_TRANSFER_BLOCK_SIZE);

        trace_ot_dma_transfer(s->ot_id, op->write ? "write" : "read",
                              AS_NAME(op->asix), op->addr, size);
        op->res = address_space_rw(op->as, op->addr, op->attrs, op->buf, size,
                                   op->write);

        if (op->res == MEMTX_OK) {
            OtDMASHA *sha = &s->sha;
            if (sha->desc) {
                int res = sha->desc->process(&sha->state, op->buf, size);
                g_assert(res == CRYPT_OK);
            }

            op->size -= size;
            op->addr += size;
            op->buf += size;

            if (op->size) {
                /* schedule next block if any */
                uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
                timer_mod(s->timer, (int64_t)(now + DMA_PACE_NS));
                return;
            }
        }

        /* when DMA is over or in error, ot_dma_complete handles it */
    }

    ot_dma_complete(s);
}

#pragma GCC diagnostic pop

static uint64_t ot_dma_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtDMAState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_SRC_ADDR_LO:
    case R_SRC_ADDR_HI:
    case R_DEST_ADDR_LO:
    case R_DEST_ADDR_HI:
    case R_ADDR_SPACE_ID:
    case R_ENABLED_MEMORY_RANGE_BASE:
    case R_ENABLED_MEMORY_RANGE_LIMIT:
    case R_RANGE_REGWEN:
    case R_RANGE_VALID:
    case R_TOTAL_DATA_SIZE:
    case R_CHUNK_DATA_SIZE:
    case R_TRANSFER_WIDTH:
    case R_DEST_ADDR_LIMIT_LO:
    case R_DEST_ADDR_LIMIT_HI:
    case R_DEST_ADDR_THRESHOLD_LO:
    case R_DEST_ADDR_THRESHOLD_HI:
    case R_STATUS:
    case R_ERROR_CODE:
    case R_HANDSHAKE_INTR:
    case R_CLEAR_INT_SRC:
    case R_CLEAR_INT_BUS:
    case R_SHA2_DIGEST_0 ... R_SHA2_DIGEST_15:
    case R_INT_SRC_ADDR_0 ... R_INT_SRC_ADDR_10:
    case R_INT_SRC_WR_VAL_0 ... R_INT_SRC_WR_VAL_10:
        val32 = s->regs[reg];
        break;
    case R_CFG_REGWEN:
        val32 = ot_dma_is_configurable(s) ? OT_MULTIBITBOOL4_TRUE :
                                            OT_MULTIBITBOOL4_FALSE;
        break;
    case R_CONTROL:
        val32 = s->regs[reg] & ~R_CONTROL_ABORT_MASK; /* W/O */
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
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

    uint32_t pc = ibex_get_current_pc();
    trace_ot_dma_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                             pc);

    return (uint64_t)val32;
};

static void ot_dma_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned size)
{
    OtDMAState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_dma_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32, pc);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_INTR_TEST:
    case R_ALERT_TEST:
    case R_CONTROL:
    case R_STATUS:
    case R_RANGE_REGWEN:
    case R_RANGE_VALID:
    case R_ENABLED_MEMORY_RANGE_BASE:
    case R_ENABLED_MEMORY_RANGE_LIMIT:
        break;
    default:
        if (!ot_dma_is_configurable(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: %s (0x%03x) not configurable", __func__,
                          s->ot_id, REG_NAME(reg), (uint32_t)addr);
            return;
        }
        break;
    }

    switch (reg) {
    case R_INTR_STATE:
        val32 &= INTR_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        ot_dma_update_irqs(s);
        break;
    case R_INTR_ENABLE:
        val32 &= INTR_MASK;
        s->regs[reg] = val32;
        ot_dma_update_irqs(s);
        break;
    case R_INTR_TEST:
        val32 &= INTR_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_dma_update_irqs(s);
        break;
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
            ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
        }
        break;
    case R_SRC_ADDR_LO ... R_SRC_ADDR_HI:
    case R_DEST_ADDR_LO ... R_DEST_ADDR_HI:
        s->regs[reg] = val32;
        break;
    case R_DEST_ADDR_LIMIT_LO ... R_DEST_ADDR_LIMIT_HI:
    case R_DEST_ADDR_THRESHOLD_LO ... R_DEST_ADDR_THRESHOLD_HI:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: Limit reg 0x%02" HWADDR_PRIx " (%s) is not "
                      "supported\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        s->regs[reg] = val32;
        break;
    case R_ENABLED_MEMORY_RANGE_BASE:
    case R_ENABLED_MEMORY_RANGE_LIMIT:
        if (!ot_dma_is_range_locked(s)) {
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: 0x%02" HWADDR_PRIx " (%s) is locked\n",
                          __func__, s->ot_id, addr, REG_NAME(reg));
            /* not sure what to do here, should we set an error? */
        }
        break;
    case R_TOTAL_DATA_SIZE:
    case R_CHUNK_DATA_SIZE:
        s->regs[reg] = val32;
        break;
    case R_HANDSHAKE_INTR:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: Handshake reg 0x%02" HWADDR_PRIx " (%s) is not "
                      "supported\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 &= R_HANDSHAKE_INTR_ENABLE_MASK;
        s->regs[reg] = val32;
        break;
    case R_ADDR_SPACE_ID:
        val32 &= R_ADDR_SPACE_ID_SRC_MASK | R_ADDR_SPACE_ID_DEST_MASK;
        s->regs[reg] = val32;
        break;
    case R_RANGE_VALID:
        if (!ot_dma_is_range_locked(s)) {
            val32 &= R_RANGE_VALID_VALID_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: 0x%02" HWADDR_PRIx " (%s) is locked\n",
                          __func__, s->ot_id, addr, REG_NAME(reg));
        }
        break;
    case R_RANGE_REGWEN:
        val32 &= R_RANGE_REGWEN_EN_MASK;
        s->regs[reg] = ot_multibitbool_w0c_write(s->regs[reg], val32, 4u);
        break;
    case R_TRANSFER_WIDTH:
        val32 &= R_TRANSFER_WIDTH_WIDTH_MASK;
        s->regs[reg] = val32;
        break;
    case R_CONTROL: {
        val32 &= CONTROL_MASK;
        uint32_t change = s->regs[reg] ^ val32;
        s->regs[reg] = val32 & ~R_CONTROL_ABORT_MASK;
        if (change & val32 & R_CONTROL_ABORT_MASK) {
            ot_dma_abort(s);
        } else if (change & val32 & R_CONTROL_GO_MASK) {
            if (s->state == SM_IDLE) {
                ot_dma_go(s);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: %s: cannot start DMA from state %s\n",
                              __func__, s->ot_id, STATE_NAME(s->state));
            }
        }
    } break;
    case R_STATUS:
        val32 &=
            (R_STATUS_DONE_MASK | R_STATUS_ABORTED_MASK | R_STATUS_ERROR_MASK);
        s->regs[reg] &= ~val32; /* RW1C */
        if (val32 & R_STATUS_ERROR_MASK) {
            s->regs[R_ERROR_CODE] = 0;
        }
        break;
    /* NOLINTNEXTLINE */
    case R_CLEAR_INT_SRC:
        val32 &= (1u << PARAM_NUM_INT_CLEAR_SRCS) - 1u;
        s->regs[reg] = val32;
        break;
    case R_CLEAR_INT_BUS:
        /* each bit: 0: CTN/system, 1: OT-internal */
        val32 &= (1u << PARAM_NUM_INT_CLEAR_SRCS) - 1u;
        s->regs[reg] = val32;
        break;
    case R_INT_SRC_ADDR_0 ... R_INT_SRC_ADDR_10:
    case R_INT_SRC_WR_VAL_0 ... R_INT_SRC_WR_VAL_10:
        s->regs[reg] = val32;
        break;
    case R_CFG_REGWEN:
    case R_ERROR_CODE:
    case R_SHA2_DIGEST_0 ... R_SHA2_DIGEST_15:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        break;
    }
};

static Property ot_dma_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtDMAState, ot_id),
    DEFINE_PROP_STRING("ot_as_name", OtDMAState, ot_as_name),
    DEFINE_PROP_STRING("ctn_as_name", OtDMAState, ctn_as_name),
    DEFINE_PROP_STRING("sys_as_name", OtDMAState, sys_as_name),
#ifdef OT_DMA_HAS_ROLE
    DEFINE_PROP_UINT8("role", OtDMAState, role, UINT8_MAX),
#endif
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_dma_regs_ops = {
    .read = &ot_dma_regs_read,
    .write = &ot_dma_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_dma_reset(DeviceState *dev)
{
    OtDMAState *s = OT_DMA(dev);

    g_assert(s->ot_id);

    timer_del(s->timer);

    Object *soc = OBJECT(dev)->parent;

    Object *obj;
    OtAddressSpaceState *oas;

    CHANGE_STATE(s, IDLE);

    if (!s->ases[AS_OT]) {
        obj = object_property_get_link(soc, s->ot_as_name, &error_fatal);
        oas = OBJECT_CHECK(OtAddressSpaceState, obj, TYPE_OT_ADDRESS_SPACE);
        s->ases[AS_OT] = ot_address_space_get(oas);
    }

    if (!s->ases[AS_CTN] && s->ctn_as_name) {
        obj = object_property_get_link(soc, s->ctn_as_name, &error_fatal);
        oas = OBJECT_CHECK(OtAddressSpaceState, obj, TYPE_OT_ADDRESS_SPACE);
        s->ases[AS_CTN] = ot_address_space_get(oas);
    }

    if (!s->ases[AS_SYS] && s->sys_as_name) {
        obj = object_property_get_link(soc, s->sys_as_name, &error_fatal);
        oas = OBJECT_CHECK(OtAddressSpaceState, obj, TYPE_OT_ADDRESS_SPACE);
        s->ases[AS_SYS] = ot_address_space_get(oas);
    }

    memset(s->regs, 0, REGS_SIZE);
    memset(&s->sha, 0, sizeof(s->sha));

    s->regs[R_ADDR_SPACE_ID] = (ASID_OT << 4u) | (ASID_OT << 0u);
    s->regs[R_RANGE_REGWEN] = OT_MULTIBITBOOL4_TRUE;
    s->regs[R_CFG_REGWEN] = OT_MULTIBITBOOL4_TRUE; /* not used */
    s->regs[R_TRANSFER_WIDTH] = TRANSACTION_WIDTH_WORD;
    s->regs[R_HANDSHAKE_INTR] = (1u << PARAM_NUM_INT_CLEAR_SRCS) - 1u;

    ot_dma_update_irqs(s);
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }
}

static void ot_dma_init(Object *obj)
{
    OtDMAState *s = OT_DMA(obj);

    memory_region_init_io(&s->mmio, obj, &ot_dma_regs_ops, s, TYPE_OT_DMA,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);

    for (unsigned ix = 0; ix < PARAM_NUM_IRQS; ix++) {
        ibex_sysbus_init_irq(obj, &s->irqs[ix]);
    }
    for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    s->timer = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_dma_transfer, s);
}

static void ot_dma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_dma_reset;
    device_class_set_props(dc, ot_dma_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_dma_info = {
    .name = TYPE_OT_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtDMAState),
    .instance_init = &ot_dma_init,
    .class_init = &ot_dma_class_init,
};

static void ot_dma_register_types(void)
{
    type_register_static(&ot_dma_info);
}

type_init(ot_dma_register_types);
