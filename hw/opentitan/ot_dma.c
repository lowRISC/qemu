/*
 * QEMU OpenTitan DMA device
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
 * Note: for now, only a minimalist subset of Analog Sensor Top device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_address_space.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_dma.h"
#include "hw/qdev-properties-system.h"
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
// are those registers updated while DMA is processing?
REG32(SRC_ADDR_LO, 0x10u)
REG32(SRC_ADDR_HI, 0x14u)
REG32(DEST_ADDR_LO, 0x18u)
REG32(DEST_ADDR_HI, 0x1cu)
REG32(ADDR_SPACE_ID, 0x20u)
    FIELD(ADDR_SPACE_ID, SRC, 0u, 4u)
    FIELD(ADDR_SPACE_ID, DEST, 4u, 4u)
REG32(ENABLED_MEMORY_RANGE_BASE, 0x24u)
REG32(ENABLED_MEMORY_RANGE_LIMIT, 0x28u)
// can we use lock rather than unlock to match other devices?
REG32(RANGE_UNLOCK_REGWEN, 0x2cu)
    FIELD(RANGE_UNLOCK_REGWEN, EN, 0u, 4u)
REG32(TOTAL_DATA_SIZE, 0x30u)
// why is it not part on CONTROL?
REG32(TRANSFER_WIDTH, 0x34u)
    FIELD(TRANSFER_WIDTH, WIDTH, 0u, 2u)
// what does that means when DEST is a FIFO, with auto inc is 0?
REG32(DEST_ADDR_LIMIT_LO, 0x38u)
REG32(DEST_ADDR_LIMIT_HI, 0x3cu)
REG32(DEST_ADDR_THRESHOLD_LO, 0x40u)
REG32(DEST_ADDR_THRESHOLD_HI, 0x44u)
REG32(CONTROL, 0x48u)
    // b0..b1 empty?
    FIELD(CONTROL, OPCODE, 2u, 4u)
    FIELD(CONTROL, HW_HANDSHAKE_EN, 6u, 1u)
    // why not SRC & DEST rather than MEM and FIFO?
    FIELD(CONTROL, MEM_BUF_AUTO_INC_EN, 7u, 1u)
    // Maybe something else than a FIFO, and doc keeps referring to LSIO FIFO
    FIELD(CONTROL, FIFO_AUTO_INC_EN, 8u, 1u)
    // Change this and MEM/FIFO mess with SRC/DEST auto increment
    FIELD(CONTROL, DATA_DIR, 9u, 1u)
    // what does actually happends on abort, what is the state of the DMA
    // controller and more important, any connected, DMA-driver HW?
    FIELD(CONTROL, ABORT, 27u, 1u)
    // what happens when GO is updated and already set?
    // what happens when ABORT & GO are set together?
    // is is possible to GO if ERROR or ERROR_CODE is set?
    // if the above holds true, when are these bits cleared in the sequence?
    FIELD(CONTROL, GO, 31u, 1u)
REG32(STATUS, 0x4cu)
    // what happens on error? ERROR + DONE (as other OT devices)?
    FIELD(STATUS, BUSY, 0u, 1u)
    FIELD(STATUS, DONE, 1u, 1u)
    FIELD(STATUS, ABORTED, 2u, 1u)
    FIELD(STATUS, ERROR, 3u, 1u)
    FIELD(STATUS, ERROR_CODE, 4u, 8u)
// this seems to be non OT way of mixing existing features into a all-in-one reg.
REG32(CLEAR_STATE, 0x50u)
    FIELD(CLEAR_STATE, CLEAR, 0u, 1u)
REG32(HANDSHAKE_INTR_ENABLE, 0x54u)
// INT_SRC likely needs renaming
// Why is INT_SRC isolated from other INT_SRC registers?
REG32(INT_SRC, 0x58u)
    FIELD(INT_SRC, CLEAR_EN, 0u, PARAM_NUM_INT_CLEAR_SRCS)
REG32(SHA2_DIGEST_0, 0x5cu)
REG32(SHA2_DIGEST_1, 0x60u)
REG32(SHA2_DIGEST_2, 0x64u)
REG32(SHA2_DIGEST_3, 0x68u)
REG32(SHA2_DIGEST_4, 0x6cu)
REG32(SHA2_DIGEST_5, 0x70u)
REG32(SHA2_DIGEST_6, 0x74u)
REG32(SHA2_DIGEST_7, 0x78u)
REG32(SHA2_DIGEST_8, 0x7cu)
REG32(SHA2_DIGEST_9, 0x80u)
REG32(SHA2_DIGEST_10, 0x84u)
REG32(SHA2_DIGEST_11, 0x88u)
REG32(SHA2_DIGEST_12, 0x8cu)
REG32(SHA2_DIGEST_13, 0x90u)
REG32(SHA2_DIGEST_14, 0x94u)
REG32(SHA2_DIGEST_15, 0x98u)
REG32(INT_SRC_BUS_SEL, 0x9cu)
    FIELD(INT_SRC_BUS_SEL, INTERNAL, 0u, PARAM_NUM_INT_CLEAR_SRCS)
// general issue with OT: should be aligned so when CLEAR_SRCS increase to 32,
// register map does not change
REG32(INT_SRC_ADDR_0, 0xa0u)
REG32(INT_SRC_ADDR_1, 0xa4u)
REG32(INT_SRC_ADDR_2, 0xa8u)
REG32(INT_SRC_ADDR_3, 0xacu)
REG32(INT_SRC_ADDR_4, 0xb0u)
REG32(INT_SRC_ADDR_5, 0xb4u)
REG32(INT_SRC_ADDR_6, 0xb8u)
REG32(INT_SRC_ADDR_7, 0xbcu)
REG32(INT_SRC_ADDR_8, 0xc0u)
REG32(INT_SRC_ADDR_9, 0xc4u)
REG32(INT_SRC_ADDR_10, 0xc8u)
REG32(INT_SRC_VAL_0, 0xccu)
REG32(INT_SRC_VAL_1, 0xd0u)
REG32(INT_SRC_VAL_2, 0xd4u)
REG32(INT_SRC_VAL_3, 0xd8u)
REG32(INT_SRC_VAL_4, 0xdcu)
REG32(INT_SRC_VAL_5, 0xe0u)
REG32(INT_SRC_VAL_6, 0xe4u)
REG32(INT_SRC_VAL_7, 0xe8u)
REG32(INT_SRC_VAL_8, 0xecu)
REG32(INT_SRC_VAL_9, 0xf0u)
REG32(INT_SRC_VAL_10, 0xf4u)

#if defined(MEMTXATTRS_HAS_ROLE) && (MEMTXATTRS_HAS_ROLE != 0)
#define OT_DMA_HAS_ROLE
#else
#undef OT_DMA_HAS_ROLE
#endif

typedef enum {
    TRANSACTION_WIDTH_BYTE = 0x0,
    TRANSACTION_WIDTH_HALF = 0x1,
    TRANSACTION_WIDTH_ERROR = 0x2,
    // why not 2 so it is only a matters of 1<<x?
    TRANSACTION_WIDTH_WORD = 0x3,
} OtDMATransferWidth;

typedef enum {
    CONTROL_OPCODE_COPY = 0x0,
    CONTROL_OPCODE_SHA256 = 0x1,
    CONTROL_OPCODE_SHA384 = 0x2,
    CONTROL_OPCODE_SHA512 = 0x3,
} OtDMAControlOpcode;

typedef enum {
    ERR_SRC_ADDR,
    ERR_DEST_ADDR,
    ERR_OPCODE,
    // what triggers this?
    ERR_SIZE,
    ERR_COMPLETION,
    // what triggers this?
    ERR_MEM_CONFIG,
    // what triggers this?
    ERR_UNLOCKED_CONFIG,
    ERR_ASID,
    _ERR_COUNT
} OtDMAError;

typedef enum {
    // need renaming
    ASID_OT = 0x7,
    ASID_CTRL = 0xa,
    ASID_SYS = 0x9,
    ASID_FLASH = 0xc,
} OtDMAAddrSpaceId;

typedef enum {
    AS_OT,
    AS_CTN,
    AS_SYS,
    _AS_COUNT
} OtDMAAddrSpace;

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
    AddressSpace *ases[_AS_COUNT];
    QEMUTimer *timer;

    OtDMAOp op;
    OtDMASHA sha;
    uint32_t *regs;

    char *ot_as_name; /* private AS unique name */
    char *ctn_as_name; /* externel port AS unique name */
    char *sys_as_name; /* external system AS unique name */
    char *dma_id;
#ifdef OT_DMA_HAS_ROLE
    uint8_t role;
#endif
};

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_INT_SRC_VAL_10)
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
     R_CONTROL_DATA_DIR_MASK | R_CONTROL_ABORT_MASK | R_CONTROL_GO_MASK)
#define STATUS_MASK \
    (R_STATUS_BUSY_MASK | R_STATUS_DONE_MASK | R_STATUS_ABORTED_MASK | \
     R_STATUS_ERROR_MASK | R_STATUS_ERROR_CODE_MASK)

#define DMA_ERROR_MASK   (((1u << R_STATUS_ERROR_CODE_LENGTH)) - 1u)
#define DMA_ERROR(_err_) (1u << ((_err_)&DMA_ERROR_MASK))

/* the following values are arbitrary end may be changed if needed */
#define DMA_PACE_NS             10000u /* 10us to slow down DMA and handle aborts */
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
    REG_NAME_ENTRY(RANGE_UNLOCK_REGWEN),
    REG_NAME_ENTRY(TOTAL_DATA_SIZE),
    REG_NAME_ENTRY(TRANSFER_WIDTH),
    REG_NAME_ENTRY(DEST_ADDR_LIMIT_LO),
    REG_NAME_ENTRY(DEST_ADDR_LIMIT_HI),
    REG_NAME_ENTRY(DEST_ADDR_THRESHOLD_LO),
    REG_NAME_ENTRY(DEST_ADDR_THRESHOLD_HI),
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(CLEAR_STATE),
    REG_NAME_ENTRY(HANDSHAKE_INTR_ENABLE),
    REG_NAME_ENTRY(INT_SRC),
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
    REG_NAME_ENTRY(INT_SRC_BUS_SEL),
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
    REG_NAME_ENTRY(INT_SRC_VAL_0),
    REG_NAME_ENTRY(INT_SRC_VAL_1),
    REG_NAME_ENTRY(INT_SRC_VAL_2),
    REG_NAME_ENTRY(INT_SRC_VAL_3),
    REG_NAME_ENTRY(INT_SRC_VAL_4),
    REG_NAME_ENTRY(INT_SRC_VAL_5),
    REG_NAME_ENTRY(INT_SRC_VAL_6),
    REG_NAME_ENTRY(INT_SRC_VAL_7),
    REG_NAME_ENTRY(INT_SRC_VAL_8),
    REG_NAME_ENTRY(INT_SRC_VAL_9),
    REG_NAME_ENTRY(INT_SRC_VAL_10),
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

#define ot_dma_set_xerror(_s_, _err_) \
    trace_ot_dma_set_error((_s_)->dma_id, __func__, __LINE__, \
                           DMA_ERROR(_err_)); \
    ot_dma_set_error(_s_, (_err_))


/* -------------------------------------------------------------------------- */
/* Private implementation */
/* -------------------------------------------------------------------------- */

static void ot_dma_update_irqs(OtDMAState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];
    trace_ot_csrng_irqs(s->regs[R_INTR_STATE], s->regs[R_INTR_ENABLE], level);
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

static bool ot_dma_is_range_is_locked(const OtDMAState *s)
{
    return s->regs[R_RANGE_UNLOCK_REGWEN] != OT_MULTIBITBOOL4_TRUE;
}

static bool ot_dma_is_busy(const OtDMAState *s)
{
    return (bool)(s->regs[R_STATUS] & R_STATUS_BUSY_MASK);
}

static void ot_dma_set_error(OtDMAState *s, unsigned err)
{
    g_assert(err < _ERR_COUNT);

    s->regs[R_STATUS] |= R_STATUS_ERROR_MASK;
    s->regs[R_STATUS] |= DMA_ERROR(err) << R_STATUS_ERROR_SHIFT;
    s->regs[R_INTR_STATE] |= INTR_DMA_ERROR_MASK;
    ot_dma_update_irqs(s);
}

static bool ot_dma_check_ot_range(OtDMAState *s, bool d_or_s)
{
    uint32_t lstart = s->regs[R_ENABLED_MEMORY_RANGE_BASE];
    uint32_t lend = s->regs[R_ENABLED_MEMORY_RANGE_LIMIT];

    uint32_t tsize = s->regs[R_TOTAL_DATA_SIZE];
    /* *_ADDR_HI ignored here, SBZ */
    uint32_t tstart = s->regs[d_or_s ? R_DEST_ADDR_LO : R_SRC_ADDR_LO];
    uint32_t tend = tstart + tsize;

    if (tend < tstart) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: DMA end overflow\n", __func__,
                      s->dma_id);
        ot_dma_set_xerror(s, ERR_SIZE);
        return false;
    }

    if (tstart < lstart) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: DMA starts in prohibited region "
                      "0x%08x < 0x%08x\n",
                      __func__, s->dma_id, tstart, lstart);
        ot_dma_set_xerror(s, d_or_s ? ERR_DEST_ADDR : ERR_SRC_ADDR);
        return false;
    }

    if (tend > lend) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: DMA ends in prohibited region "
                      "0x%08x > 0x%08x\n",
                      __func__, s->dma_id, tend, lend);
        ot_dma_set_xerror(s, d_or_s ? ERR_DEST_ADDR : ERR_SRC_ADDR);
        return false;
    }

    return true;
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
    case ASID_FLASH:
    default:
        return _AS_COUNT;
    }
}

static MemoryRegion *
ot_dma_check_device(OtDMAState *s, bool d_or_s, OtDMAAddrSpace *asix,
                    bool *is_dev, hwaddr *offset)
{
    OtDMAAddrSpace aix = ot_dma_get_asid(s, d_or_s);
    if (aix >= _AS_COUNT) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid address space\n",
                      __func__, s->dma_id);
        ot_dma_set_xerror(s, ERR_ASID);
        return NULL;
    }

    if (!s->ases[aix]) {
        error_setg(&error_fatal, "%s: %s: %s address space not configured",
                   __func__, s->dma_id, AS_NAME(aix));
        return NULL;
    }

    AddressSpace *as = s->ases[aix];

    hwaddr start =
        d_or_s ? ot_dma_get_dest_address(s) : ot_dma_get_src_address(s);
    hwaddr size = s->regs[R_TOTAL_DATA_SIZE];

    MemoryRegionSection mrs = { 0 };
    mrs = memory_region_find(as->root, start, size);

    if (!mrs.mr || !mrs.size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid %s address as:%s "
                      "addr: 0x%" HWADDR_PRIx " size: 0x%" HWADDR_PRIx "\n",
                      __func__, s->dma_id, d_or_s ? "dest" : "src",
                      AS_NAME(aix), start, size);
        ot_dma_set_xerror(s, d_or_s ? ERR_DEST_ADDR : ERR_SRC_ADDR);
        return NULL;
    }

    if (mrs.offset_within_region + mrs.size < size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Invalid size\n", __func__,
                      s->dma_id);
        ot_dma_set_xerror(s, ERR_SIZE);
        memory_region_unref(mrs.mr);
        return NULL;
    }

    trace_ot_dma_check_device(s->dma_id, d_or_s ? "Dest" : "Src", AS_NAME(aix),
                              start, size, mrs.mr->name, mrs.mr->ram);
    *asix = aix;
    *is_dev = !mrs.mr->ram;
    *offset = mrs.offset_within_region;

    /* caller should invoke memory_region_unref(mrs.mr) once done with mr */
    return mrs.mr;
}

static bool ot_dma_go(OtDMAState *s)
{
    if (FIELD_EX32(s->regs[R_CONTROL], CONTROL, HW_HANDSHAKE_EN)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: Handshake mode is not supported\n",
                      __func__, s->dma_id);
        ot_dma_set_xerror(s, ERR_MEM_CONFIG);
        return false;
    }
    /*
    if (!FIELD_EX32(s->regs[R_CONTROL], CONTROL, FIFO_AUTO_INC_EN)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: FIFO no inc mode is not supported\n",
                      __func__, s->dma_id);
        ot_dma_set_xerror(s, ERR_MEM_CONFIG);
        return false;
    }
    */
    if (!FIELD_EX32(s->regs[R_CONTROL], CONTROL, MEM_BUF_AUTO_INC_EN)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: MEM no inc mode is not supported\n",
                      __func__, s->dma_id);
        ot_dma_set_xerror(s, ERR_MEM_CONFIG);
        return false;
    }

    MemoryRegion *smr;
    hwaddr soffset;
    OtDMAAddrSpace sasix;
    bool sdev;
    smr = ot_dma_check_device(s, false, &sasix, &sdev, &soffset);
    if (!smr) {
        return false;
    }

    MemoryRegion *dmr;
    hwaddr doffset;
    OtDMAAddrSpace dasix;
    bool ddev;
    dmr = ot_dma_check_device(s, true, &dasix, &ddev, &doffset);
    if (!dmr) {
        memory_region_unref(smr);
        return false;
    }

    /* some src/dest combinations are not supported for now */
    if ((sasix == AS_SYS) && (dasix == AS_SYS)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s: SYS-to-SYS is not supported\n",
                      __func__, s->dma_id);
        ot_dma_set_xerror(s, ERR_ASID);
        return false;
    }

    if (sasix == AS_OT) {
        if (!ot_dma_check_ot_range(s, false)) {
            return false;
        }
    }

    if (dasix == AS_OT) {
        if (!ot_dma_check_ot_range(s, true)) {
            return false;
        }
    }

    if (sdev && ddev) {
        /* could be done w/ an intermediate buffer, but likely useless */
        qemu_log_mask(LOG_UNIMP, "%s: %s: DEV-to-DEV is not supported\n",
                      __func__, s->dma_id);
        ot_dma_set_xerror(s, ERR_COMPLETION);
        return false;
    }

    unsigned sha_mode = FIELD_EX32(s->regs[R_CONTROL], CONTROL, OPCODE);
    OtDMASHA *sha = &s->sha;
    switch (sha_mode) {
    case CONTROL_OPCODE_COPY:
        sha->desc = 0;
        break;
    case CONTROL_OPCODE_SHA256:
        sha->desc = &sha256_desc;
        break;
    case CONTROL_OPCODE_SHA384:
        sha->desc = &sha384_desc;
        break;
    case CONTROL_OPCODE_SHA512:
        sha->desc = &sha512_desc;
        break;
    default:
        g_assert_not_reached();
    }

    if (sha->desc) {
        int res = sha->desc->init(&sha->state);
        g_assert(res == CRYPT_OK);
    }

    OtDMAOp *op = &s->op;

    op->attrs.unspecified = false;
#ifdef OT_DMA_HAS_ROLE
    op->attrs.role = (unsigned)s->role;
#endif
    op->size = s->regs[R_TOTAL_DATA_SIZE];

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
    int twidth = (int)s->regs[R_TRANSFER_WIDTH];
    if (sdev) {
        /* check first and last slots of the requested transfer */
        int swidth = memory_access_size(smr, sizeof(uint32_t), soffset);
        int ewidth = memory_access_size(smr, sizeof(uint32_t),
                                        soffset + op->size - sizeof(uint32_t));
        int dwidth = MIN(swidth, ewidth);
        if (dwidth < twidth) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: Src device does not supported "
                          "requested width: %d, max %d\n",
                          __func__, s->dma_id, twidth, dwidth);
            ot_dma_set_xerror(s, ERR_MEM_CONFIG);
        }
    }
    if (ddev) {
        /* check first and last slots of the requested transfer */
        int swidth = memory_access_size(smr, sizeof(uint32_t), soffset);
        int ewidth = memory_access_size(smr, sizeof(uint32_t),
                                        soffset + op->size - sizeof(uint32_t));
        int dwidth = MIN(swidth, ewidth);
        if (dwidth < twidth) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: %s: Dest device does not supported "
                          "requested width: %d, max %d\n",
                          __func__, s->dma_id, twidth, dwidth);
            ot_dma_set_xerror(s, ERR_MEM_CONFIG);
        }
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

    trace_ot_dma_new_op(s->dma_id, op->write ? "write" : "read",
                        AS_NAME(op->asix), op->mr->name, op->addr, op->size);

    /* explicitly clear all errors, not sure this is realistic */
    s->regs[R_STATUS] = R_STATUS_BUSY_MASK;

    timer_del(s->timer);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->timer, (int64_t)(now + DMA_PACE_NS));

    return true;
}

static void ot_dma_abort(OtDMAState *s)
{
    if (!ot_dma_is_busy(s)) {
        /* nothing to do, but ABORTED be signaled? */
        return;
    }

    trace_ot_dma_abort(s->dma_id);

    s->regs[R_CONTROL] |= R_CONTROL_ABORT_MASK;

    /* simulate a delayed response */
    timer_del(s->timer);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
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

        trace_ot_dma_complete(s->dma_id, -1);
    } else if (s->regs[R_CONTROL] & R_CONTROL_GO_MASK) {
        if (!FIELD_EX32(s->regs[R_CONTROL], CONTROL, HW_HANDSHAKE_EN)) {
            s->regs[R_CONTROL] &= ~R_CONTROL_GO_MASK;
        }

        trace_ot_dma_complete(s->dma_id, (int)op->res);

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
            ot_dma_set_xerror(s, ERR_COMPLETION);
            return;
        /* nothing at that address */
        case MEMTX_DECODE_ERROR:
            ot_dma_set_xerror(s, op->write ? ERR_DEST_ADDR : ERR_SRC_ADDR);
            return;
        /* access denied */
        case MEMTX_ACCESS_ERROR:
            ot_dma_set_xerror(s, ERR_MEM_CONFIG);
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
        }

        s->regs[R_INTR_STATE] |= INTR_DMA_DONE_MASK;
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

        trace_ot_dma_transfer(s->dma_id, op->write ? "write" : "read",
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
                uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                timer_mod(s->timer, (int64_t)(now + DMA_PACE_NS));
                return;
            }
        }
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
    case R_RANGE_UNLOCK_REGWEN:
    case R_TOTAL_DATA_SIZE:
    case R_TRANSFER_WIDTH:
    case R_DEST_ADDR_LIMIT_LO:
    case R_DEST_ADDR_LIMIT_HI:
    case R_DEST_ADDR_THRESHOLD_LO:
    case R_DEST_ADDR_THRESHOLD_HI:
    case R_STATUS:
    case R_CLEAR_STATE:
    case R_HANDSHAKE_INTR_ENABLE:
    case R_INT_SRC:
    case R_SHA2_DIGEST_0 ... R_SHA2_DIGEST_15:
    case R_INT_SRC_BUS_SEL:
    case R_INT_SRC_ADDR_0 ... R_INT_SRC_ADDR_10:
    case R_INT_SRC_VAL_0 ... R_INT_SRC_VAL_10:
        val32 = s->regs[reg];
        break;
    case R_CONTROL:
        val32 = s->regs[reg] & ~R_CONTROL_ABORT_MASK; /* W/O */
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Bad offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->dma_id, addr);
        val32 = 0;
        break;
    }

    uint64_t pc = ibex_get_current_pc();
    trace_ot_dma_io_read_out(s->dma_id, (unsigned)addr, REG_NAME(reg),
                             (uint64_t)val32, pc);

    return (uint64_t)val32;
};

static void ot_dma_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned size)
{
    OtDMAState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint64_t pc = ibex_get_current_pc();
    trace_ot_dma_io_write(s->dma_id, (unsigned)addr, REG_NAME(reg), val64, pc);

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
        if (val32) {
            for (unsigned ix = 0; ix < PARAM_NUM_ALERTS; ix++) {
                ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
            }
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
                      __func__, s->dma_id, addr, REG_NAME(reg));
        s->regs[reg] = val32;
        break;
    case R_ENABLED_MEMORY_RANGE_BASE:
    case R_ENABLED_MEMORY_RANGE_LIMIT:
        if (!ot_dma_is_range_is_locked(s)) {
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s: 0x%02" HWADDR_PRIx " (%s) is locked\n",
                          __func__, s->dma_id, addr, REG_NAME(reg));
            // not sure to understand what to do here, should we trigger an
            // interrupt on reg write?
            ot_dma_set_xerror(s, ERR_UNLOCKED_CONFIG);
        }
        break;
    case R_TOTAL_DATA_SIZE:
        s->regs[reg] = val32;
        break;
    case R_HANDSHAKE_INTR_ENABLE:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: Handshake reg 0x%02" HWADDR_PRIx " (%s) is not "
                      "supported\n",
                      __func__, s->dma_id, addr, REG_NAME(reg));
        s->regs[reg] = val32;
        break;
    case R_ADDR_SPACE_ID:
        val32 &= R_ADDR_SPACE_ID_SRC_MASK | R_ADDR_SPACE_ID_DEST_MASK;
        s->regs[reg] = val32;
        break;
    case R_RANGE_UNLOCK_REGWEN:
        val32 &= R_RANGE_UNLOCK_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_TRANSFER_WIDTH:
        val32 &= R_TRANSFER_WIDTH_WIDTH_MASK;
        s->regs[reg] = val32;
        break;
    case R_CONTROL: {
        val32 &= CONTROL_MASK;
        uint32_t change = s->regs[reg] ^ val32;
        s->regs[reg] = val32;
        if (change & val32 & R_CONTROL_ABORT_MASK) {
            ot_dma_abort(s);
        } else if (change & val32 & R_CONTROL_GO_MASK) {
            // todo: should go allowed if ERROR or ERROR_CODE set?
            ot_dma_go(s);
        }
    } break;
    case R_STATUS:
        val32 &= STATUS_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        break;
    case R_CLEAR_STATE:
        // this register has a non-standard OT behavior.
        (void)val32; // yeap, value is don't care
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: you should not use this dodgy reg\n", __func__,
                      s->dma_id);
        s->regs[R_STATUS] &= ~(R_STATUS_DONE_MASK | R_STATUS_ERROR_MASK |
                               R_STATUS_ERROR_CODE_MASK);
        // clear the status & error, but not the error interrupt, only the done
        // one?
        s->regs[R_INTR_STATE] &= ~INTR_DMA_DONE_MASK;
        ot_dma_update_irqs(s);
        break;
    /* NOLINTNEXTLINE */
    case R_INT_SRC:
        val32 &= (1u << PARAM_NUM_INT_CLEAR_SRCS) - 1u;
        s->regs[reg] = val32;
        break;
    case R_INT_SRC_BUS_SEL:
        /* each bit: 0: CTN/system, 1: OT-internal */
        val32 &= (1u << PARAM_NUM_INT_CLEAR_SRCS) - 1u;
        s->regs[reg] = val32;
        break;
    case R_INT_SRC_ADDR_0 ... R_INT_SRC_ADDR_10:
    case R_INT_SRC_VAL_0 ... R_INT_SRC_VAL_10:
        s->regs[reg] = val32;
        break;
    case R_SHA2_DIGEST_0 ... R_SHA2_DIGEST_15:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->dma_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->dma_id, addr);
        break;
    }
};

static Property ot_dma_properties[] = {
    DEFINE_PROP_STRING("ot_as_name", OtDMAState, ot_as_name),
    DEFINE_PROP_STRING("ctn_as_name", OtDMAState, ctn_as_name),
    DEFINE_PROP_STRING("sys_as_name", OtDMAState, sys_as_name),
#ifdef OT_DMA_HAS_ROLE
    DEFINE_PROP_UINT8("role", OtDMAState, role, UINT8_MAX),
#endif
    DEFINE_PROP_STRING("id", OtDMAState, dma_id),
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

    timer_del(s->timer);

    Object *soc = OBJECT(dev)->parent;

    Object *obj;
    OtAddressSpaceState *oas;

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
    s->regs[R_RANGE_UNLOCK_REGWEN] = OT_MULTIBITBOOL4_TRUE;
    s->regs[R_TRANSFER_WIDTH] = TRANSACTION_WIDTH_WORD;
    s->regs[R_HANDSHAKE_INTR_ENABLE] = 0xffffu; // why?

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
        ibex_qdev_init_irq(obj, &s->alerts[ix], OPENTITAN_DEVICE_ALERT);
    }

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &ot_dma_transfer, s);
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

type_init(ot_dma_register_types)
