/*
 * QEMU OpenTitan SRAM controller
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
 * Note: most units are based on 32-bit words as it eases alignment and
 * management, and best fit with 32/7 ECC.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_otp.h"
#include "hw/opentitan/ot_prng.h"
#include "hw/opentitan/ot_sram_ctrl.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#undef OT_SRAM_CTRL_DEBUG

#define PARAM_NUM_ALERTS 1

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_ERROR, 0u, 1u)
REG32(STATUS, 0x4u)
    FIELD(STATUS, BUS_INTEG_ERROR, 0u, 1u)
    FIELD(STATUS, INIT_ERROR, 1u, 1u)
    FIELD(STATUS, ESCALATED, 2u, 1u)
    FIELD(STATUS, SCR_KEY_VALID, 3u, 1u)
    FIELD(STATUS, SCR_KEY_SEED_VALID, 4u, 1u)
    FIELD(STATUS, INIT_DONE, 5u, 1u)
REG32(EXEC_REGWEN, 0x8u)
    FIELD(EXEC_REGWEN, EN, 0u, 1u)
REG32(EXEC, 0xcu)
    FIELD(EXEC, EN, 0u, 4u)
REG32(CTRL_REGWEN, 0x10u)
    FIELD(CTRL_REGWEN_CTRL, REGWEN, 0u, 1u)
REG32(CTRL, 0x14u)
    FIELD(CTRL, RENEW_SCR_KEY, 0u, 1u)
    FIELD(CTRL, INIT, 1u, 1u)
REG32(SCR_KEY_ROTATED, 0x1cu)
    FIELD(SCR_KEY_ROTATED, SUCCESS, 0u, 4u)

/* clang-format on */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_SCR_KEY_ROTATED)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define INIT_TIMER_CHUNK_NS    100000 /* 100 us */
#define INIT_TIMER_CHUNK_SIZE  4096u /* 4 KB */
#define INIT_TIMER_CHUNK_WORDS ((INIT_TIMER_CHUNK_SIZE) / sizeof(uint32_t))

/* clang-format off */
#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(STATUS),
    REG_NAME_ENTRY(EXEC_REGWEN),
    REG_NAME_ENTRY(EXEC),
    REG_NAME_ENTRY(CTRL_REGWEN),
    REG_NAME_ENTRY(CTRL),
    REG_NAME_ENTRY(SCR_KEY_ROTATED),
};
#undef REG_NAME_ENTRY
/* clang-format on */

typedef struct {
    MemoryRegion alias; /* SRAM alias on one of the following */
    MemoryRegion sram; /* SRAM memory (runtime) */
    MemoryRegion init; /* SRAM memory (not yet initialized) */
} OtSramCtrlMem;

struct OtSramCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion mmio; /* SRAM controller registers */
    OtSramCtrlMem *mem; /* SRAM memory */
    IbexIRQ alert;
    QEMUBH *switch_mr_bh; /* switch memory region */
    QEMUTimer *init_timer; /* SRAM initialization timer */

    uint64_t *init_sram_bm; /* initialization bitmap */
    uint64_t *init_slot_bm; /* initialization bitmap shortcut */
    OtPrngState *prng; /* simplified PRNG, does not match OT's */
    OtOTPKey *otp_key;
    uint32_t regs[REGS_COUNT];
    unsigned init_slot_count; /* count of init_slot_bm */
    unsigned init_slot_pos; /* current SRAM cell (word-sized) for init. */
    unsigned wsize; /* size of RAM in words */
    bool initialized; /* SRAM has been fully initialized at least once */
    bool initializing; /* CTRL.INIT has been requested */
    bool otp_ifetch;
    bool cfg_ifetch;

    char *ot_id;
    OtOTPState *otp_ctrl; /* optional */
    uint32_t size; /* in bytes */
    bool ifetch; /* only used when no otp_ctrl is defined */
    bool noinit; /* discard initialization emulation feature */
};

#ifdef OT_SRAM_CTRL_DEBUG
#define TRACE_SRAM_CTRL(msg, ...) \
    qemu_log("%s: " msg "\n", __func__, ##__VA_ARGS__);
static char hexbuf[256u];
static const char *ot_sram_ctrl_hexdump(const void *data, size_t size)
{
    static const char _hex[] = "0123456789abcdef";
    const uint8_t *buf = (const uint8_t *)data;

    if (size > ((sizeof(hexbuf) / 2u) - 2u)) {
        size = sizeof(hexbuf) / 2u - 2u;
    }

    char *hexstr = hexbuf;
    for (size_t ix = 0; ix < size; ix++) {
        hexstr[(ix * 2u)] = _hex[(buf[ix] >> 4u) & 0xfu];
        hexstr[(ix * 2u) + 1u] = _hex[buf[ix] & 0xfu];
    }
    hexstr[size * 2u] = '\0';
    return hexbuf;
}
#else
#define TRACE_SRAM_CTRL(msg, ...)
#endif

static inline unsigned ot_sram_ctrl_get_u64_slot(unsigned idx)
{
    return idx >> 6; /* init_sram_bm is 64 bit wide */
}

static inline unsigned ot_sram_ctrl_get_u64_offset(unsigned idx)
{
    return idx & ((1u << 6u) - 1u); /* init_sram_bm is 64 bit wide */
}

static inline size_t ot_sram_ctrl_get_slot_count(size_t wsize)
{
    return ot_sram_ctrl_get_u64_slot(wsize);
}

static bool ot_sram_ctrl_mem_is_fully_initialized(const OtSramCtrlState *s)
{
    for (unsigned ix = 0; ix < s->init_slot_count; ix++) {
        if (s->init_slot_bm[ix]) {
            trace_ot_sram_ctrl_mem_not_initialized(s->ot_id, ix,
                                                   s->init_slot_bm[ix]);
            return false;
        }
    }

    return true;
}

static bool ot_sram_ctrl_initialize(OtSramCtrlState *s, unsigned count)
{
    unsigned end = s->init_slot_pos + count;

    g_assert(end <= s->wsize);

    trace_ot_sram_ctrl_initialize(s->ot_id, s->init_slot_pos * sizeof(uint32_t),
                                  end * sizeof(uint32_t));

    uint32_t *mem = memory_region_get_ram_ptr(&s->mem->sram);
    mem += s->init_slot_pos;

    ot_prng_random_u32_array(s->prng, mem, count);

    memory_region_set_dirty(&s->mem->sram, s->init_slot_pos * sizeof(uint32_t),
                            count * sizeof(uint32_t));

    s->init_slot_pos = end;

    if (s->init_slot_pos >= s->wsize) {
        /* init has been completed */
        s->regs[R_STATUS] |= R_STATUS_INIT_DONE_MASK;
        /* enable new request for initialization */
        s->regs[R_CTRL] &= ~R_CTRL_INIT_MASK;

        s->initializing = false;
        s->initialized = true; /* never reset */

        /* clear out all dirty cell bitmaps */
        size_t cell_slot_count = ot_sram_ctrl_get_slot_count(s->wsize);
        memset(s->init_sram_bm, 0, cell_slot_count * sizeof(uint64_t));
        memset(s->init_slot_bm, 0, s->init_slot_count * sizeof(uint64_t));

        s->init_slot_bm = g_new0(uint64_t, s->init_slot_count);

        /* switch memory to SRAM */
        trace_ot_sram_ctrl_initialization_complete(s->ot_id, "ctrl");

        qemu_bh_schedule(s->switch_mr_bh);

        return true;
    }

    trace_ot_sram_ctrl_schedule_init(s->ot_id);

    /* schedule a new initialization chunk */
    uint64_t now = qemu_clock_get_ns(OT_VIRTUAL_CLOCK);
    timer_mod(s->init_timer, (int64_t)(now + INIT_TIMER_CHUNK_NS));

    return false;
}

static void ot_sram_ctrl_reseed(OtSramCtrlState *s)
{
    s->regs[R_STATUS] &=
        ~(R_STATUS_SCR_KEY_VALID_MASK | R_STATUS_SCR_KEY_SEED_VALID_MASK);

    if (!s->otp_ctrl) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s RESEED w/o OTP: stall bus\n",
                      __func__, s->ot_id);
        /* never returns, to simulate the bus stall */
        for (;;) {
            sleep(1);
        }
    }

    trace_ot_sram_ctrl_reseed(s->ot_id);

    /*
     * Note: in order to keep the implementation simple, the full OT HW behavior
     *       is not reproced here (with CPU cycle delays to obtain the key, etc.
     *       Tke key retrieval is therefore synchronous, which does not
     *       precisely emulate the HW.
     *       Moreover the scrambling is highly simplified, as for now there is
     *       neither PRINCE block cipher nor shallow substitution-permutation.
     *       Seed and Nonce are combined to initialize a QEMU PRNG instance.
     */
    OtOTPStateClass *oc =
        OBJECT_GET_CLASS(OtOTPStateClass, s->otp_ctrl, TYPE_OT_OTP);
    if (!oc->get_otp_key) {
        /* on EarlGrey, OTP key handing has not been implemented */
        qemu_log_mask(LOG_UNIMP, "%s: %s OTP does not support key generation\n",
                      __func__, s->ot_id);
    } else {
        oc->get_otp_key(s->otp_ctrl, OTP_KEY_SRAM, s->otp_key);

        TRACE_SRAM_CTRL("Scrambing seed:  %s (valid: %u)",
                        ot_sram_ctrl_hexdump(s->otp_key->seed,
                                             s->otp_key->seed_size),
                        s->otp_key->seed_valid);
        TRACE_SRAM_CTRL("Scrambing nonce: %s",
                        ot_sram_ctrl_hexdump(s->otp_key->nonce,
                                             s->otp_key->nonce_size));

        if (s->otp_key->seed_valid) {
            s->regs[R_STATUS] |= R_STATUS_SCR_KEY_SEED_VALID_MASK;
        }

        trace_ot_sram_ctrl_seed_status(s->ot_id, s->otp_key->seed_valid);

        g_assert(s->otp_key->seed_size <= OT_OTP_SEED_MAX_SIZE);
        g_assert(s->otp_key->nonce_size <= OT_OTP_NONCE_MAX_SIZE);
        uint32_t buffer[(OT_OTP_SEED_MAX_SIZE + OT_OTP_NONCE_MAX_SIZE) /
                        sizeof(uint32_t)];
        uint8_t *buf = (uint8_t *)&buffer[0];
        memcpy(buf, s->otp_key->seed, s->otp_key->seed_size);
        memcpy(&buf[s->otp_key->seed_size], s->otp_key->nonce,
               s->otp_key->nonce_size);
        ot_prng_reseed_array(s->prng, buffer,
                             (s->otp_key->seed_size + s->otp_key->nonce_size) /
                                 sizeof(uint32_t));
    }

    s->regs[R_CTRL_REGWEN] &= ~R_CTRL_RENEW_SCR_KEY_MASK;
    s->regs[R_STATUS] |= R_STATUS_SCR_KEY_VALID_MASK;
}

static void ot_sram_ctrl_start_initialization(OtSramCtrlState *s)
{
    timer_del(s->init_timer);

    s->regs[R_STATUS] &= ~R_STATUS_INIT_DONE_MASK;

    s->initializing = true;

    trace_ot_sram_ctrl_request_hw_init(s->ot_id);

    if (s->mem->alias.alias != &s->mem->init) {
        memory_region_transaction_begin();
        memory_region_set_enabled(&s->mem->init, true);
        memory_region_set_enabled(&s->mem->sram, false);
        s->mem->alias.alias = &s->mem->init;
        memory_region_transaction_commit();
    }

    s->init_slot_pos = 0;

    unsigned count = MIN(s->wsize, INIT_TIMER_CHUNK_WORDS);

    ot_sram_ctrl_initialize(s, count);
}

static uint64_t ot_sram_ctrl_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtSramCtrlState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_STATUS:
    case R_EXEC_REGWEN:
    case R_EXEC:
    case R_CTRL_REGWEN:
    case R_CTRL:
    case R_SCR_KEY_ROTATED:
        val32 = s->regs[reg];
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s W/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_sram_ctrl_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                   val32, pc);

    return (uint64_t)val32;
};

static void ot_sram_ctrl_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                    unsigned size)
{
    OtSramCtrlState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_sram_ctrl_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                pc);

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_ERROR_MASK;
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    case R_EXEC_REGWEN:
        val32 &= R_EXEC_REGWEN_EN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_EXEC:
        if (s->regs[R_EXEC_REGWEN]) {
            val32 &= R_EXEC_EN_MASK;
            s->regs[reg] = val32;
            if ((s->regs[reg] == OT_MULTIBITBOOL4_TRUE) && s->otp_ifetch) {
                s->cfg_ifetch = true;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s R_EXEC protected w/ REGWEN\n", __func__,
                          s->ot_id);
        }
        break;
    case R_CTRL_REGWEN:
        val32 &= R_CTRL_REGWEN_CTRL_REGWEN_MASK;
        s->regs[reg] &= val32; /* RW0C */
        break;
    case R_CTRL:
        if (s->regs[R_CTRL_REGWEN]) { /* WO */
            val32 &= R_CTRL_INIT_MASK | R_CTRL_RENEW_SCR_KEY_MASK;
            uint32_t trig = (val32 ^ s->regs[reg]) & val32;
            /* storing value prevents from trigerring again before completion */
            s->regs[reg] = val32;
            if (trig & R_CTRL_RENEW_SCR_KEY_MASK) {
                ot_sram_ctrl_reseed(s);
            }
            if (trig & R_CTRL_INIT_MASK) {
                if (s->noinit) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "%s: %s initialization support disabled\n",
                                  __func__, s->ot_id);
                } else {
                    ot_sram_ctrl_start_initialization(s);
                }
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: %s R_CTRL protected w/ REGWEN\n", __func__,
                          s->ot_id);
        }
        break;
    case R_SCR_KEY_ROTATED:
        /* this register has been deprecated on Darjeeling */
        qemu_log_mask(LOG_UNIMP, "%s: %s R_SCR_KEY_ROTATED\n", __func__,
                      s->ot_id);
        break;
    case R_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s R/O register 0x%02" HWADDR_PRIx " (%s)\n",
                      __func__, s->ot_id, addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        break;
    }
};

static void ot_sram_ctrl_mem_switch_to_ram_fn(void *opaque)
{
    OtSramCtrlState *s = opaque;

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->mem->init, false);
    memory_region_set_enabled(&s->mem->sram, true);
    s->mem->alias.alias = &s->mem->sram;
    memory_region_transaction_commit();
    memory_region_set_dirty(&s->mem->sram, 0, s->size);

    trace_ot_sram_ctrl_switch_mem(s->ot_id, "ram");
}

static void ot_sram_ctrl_init_chunk_fn(void *opaque)
{
    OtSramCtrlState *s = opaque;

    unsigned count = s->wsize - s->init_slot_pos;
    count = MIN(count, INIT_TIMER_CHUNK_WORDS);

    trace_ot_sram_ctrl_timed_init(s->ot_id);

    (void)ot_sram_ctrl_initialize(s, count);
}

static MemTxResult ot_sram_ctrl_mem_init_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    OtSramCtrlState *s = opaque;
    (void)size;
    (void)attrs;

    uint32_t pc = ibex_get_current_pc();
    trace_ot_sram_ctrl_mem_io_readi(s->ot_id, (uint32_t)addr, size, pc);

    unsigned cell = addr >> 2u;
    unsigned addr_offset = (addr & 3u);
    g_assert(addr_offset + size <= 4u);

    if (s->initializing) {
        /*
         * SRAM is being initialized. Should to release bus access till init has
         * been completed. There is no direct way to implement this with QEMU.
         * Instead, complete the initialization immediately so that this
         * function only returns when init is over
         */
        trace_ot_sram_ctrl_expediate_init(s->ot_id, "read");

        timer_del(s->init_timer);
        unsigned count = s->wsize - s->init_slot_pos;
        /* this function also take care of scheduling memory region swap */
        bool done = ot_sram_ctrl_initialize(s, count);
        g_assert(done);
    }

    if (!s->initialized) {
        /*
         * the whole RAM is not fully initialized, check if this cell has been
         * initialized
        */
        unsigned slot = ot_sram_ctrl_get_u64_slot(cell);
        unsigned offset = ot_sram_ctrl_get_u64_offset(cell);

        if (s->init_sram_bm[slot] & (1ull << offset)) {
            /* cell still flagged, i.e. not yet initialized */
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: %s: attempt to read from uninitialized cell @ 0x%08x\n",
                __func__, s->ot_id, (uint32_t)addr);

            return MEMTX_ERROR;
        }
    }

    /* retrieve the value from the final SRAM region */
    uint32_t *mem = memory_region_get_ram_ptr(&s->mem->sram);
    uint32_t val32 = mem[cell];
    val32 >>= addr_offset << 3u;
    *val64 = (uint64_t)val32;

    trace_ot_sram_ctrl_mem_io_reado(s->ot_id, (uint32_t)addr, size, val32, pc);

    return MEMTX_OK;
}

static MemTxResult ot_sram_ctrl_mem_init_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    OtSramCtrlState *s = opaque;
    (void)attrs;

    uint32_t pc = ibex_get_current_pc();
    trace_ot_sram_ctrl_mem_io_write(s->ot_id, (uint32_t)addr, size,
                                    (uint32_t)val64, pc);

    unsigned cell = addr >> 2u;
    unsigned addr_offset = (addr & 3u);
    g_assert(addr_offset + size <= 4u);

    bool skip_bm_update = s->initializing;

    if (s->initializing) {
        /*
         * SRAM is being initialized. Should to release bus access till init has
         * been completed. There is no direct way to implement this with QEMU.
         * Instead, complete the initialization immediately so that this
         * function only returns when init is over
         */
        trace_ot_sram_ctrl_expediate_init(s->ot_id, "write");

        timer_del(s->init_timer);
        unsigned count = s->wsize - s->init_slot_pos;
        /* this function also take care of scheduling memory region swap */
        bool done = ot_sram_ctrl_initialize(s, count);
        g_assert(done);
    }

    /* store the value into the final SRAM region */
    uint32_t *mem = memory_region_get_ram_ptr(&s->mem->sram);

    addr_offset <<= 3u; /* byte to bit */

    uint32_t mask = (uint32_t)((1ull << (size << 3u)) - 1u);
    uint32_t nval = (((uint32_t)val64) & mask) << addr_offset;
    uint32_t word = mem[cell];
    word &= ~(mask << addr_offset);
    word |= nval;
    mem[cell] = word;

    if (skip_bm_update) {
        return MEMTX_OK;
    }

    unsigned idx = addr / sizeof(uint32_t);
    unsigned slot = ot_sram_ctrl_get_u64_slot(idx);
    unsigned offset = ot_sram_ctrl_get_u64_offset(idx);

    s->init_sram_bm[slot] &= ~(1ull << offset);

    if (!s->init_sram_bm[slot]) {
        offset = ot_sram_ctrl_get_u64_offset(slot);
        slot = ot_sram_ctrl_get_u64_slot(slot);
        s->init_slot_bm[slot] &= ~(1ull << offset);

        if (!s->init_slot_bm[slot]) {
            if (ot_sram_ctrl_mem_is_fully_initialized(s)) {
                s->initialized = true;
                /*
                 * perform the memory switch in a BH so that the current mr
                 * is not in use when switching
                 */
                trace_ot_sram_ctrl_initialization_complete(s->ot_id, "write");

                qemu_bh_schedule(s->switch_mr_bh);
            }
        }
    }

    return MEMTX_OK;
}

static Property ot_sram_ctrl_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtSramCtrlState, ot_id),
    DEFINE_PROP_LINK("otp_ctrl", OtSramCtrlState, otp_ctrl, TYPE_OT_OTP,
                     OtOTPState *),
    DEFINE_PROP_UINT32("size", OtSramCtrlState, size, 0u),
    DEFINE_PROP_BOOL("ifetch", OtSramCtrlState, ifetch, false),
    DEFINE_PROP_BOOL("noinit", OtSramCtrlState, noinit, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_sram_ctrl_regs_ops = {
    .read = &ot_sram_ctrl_regs_read,
    .write = &ot_sram_ctrl_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static const MemoryRegionOps ot_sram_ctrl_mem_init_ops = {
    .read_with_attrs = &ot_sram_ctrl_mem_init_read_with_attrs,
    .write_with_attrs = &ot_sram_ctrl_mem_init_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1u,
    .impl.max_access_size = 4u,
};

static void ot_sram_ctrl_reset(DeviceState *dev)
{
    OtSramCtrlState *s = OT_SRAM_CTRL(dev);

    g_assert(s->ot_id);

    memset(s->regs, 0, REGS_SIZE);

    /* note: SRAM storage is -not- reset */

    s->regs[R_EXEC_REGWEN] = 0x1u;
    s->regs[R_EXEC] = OT_MULTIBITBOOL4_FALSE;
    s->regs[R_CTRL_REGWEN] = 0x1u;
    s->regs[R_SCR_KEY_ROTATED] = OT_MULTIBITBOOL4_FALSE;

    if (s->otp_ctrl) {
        OtOTPStateClass *oc =
            OBJECT_GET_CLASS(OtOTPStateClass, s->otp_ctrl, TYPE_OT_OTP);
        s->otp_ifetch = oc->get_hw_cfg(s->otp_ctrl)->en_sram_ifetch ==
                        OT_MULTIBITBOOL8_TRUE;
    } else {
        s->otp_ifetch = s->ifetch;
    }
    s->cfg_ifetch = 0u; /* not used for now */

    ibex_irq_set(&s->alert, (int)(bool)s->regs[R_ALERT_TEST]);

    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    ot_prng_reseed(s->prng, (uint32_t)now);
}

static void ot_sram_ctrl_realize(DeviceState *dev, Error **errp)
{
    OtSramCtrlState *s = OT_SRAM_CTRL(dev);

    g_assert(s->size);

    s->wsize = DIV_ROUND_UP(s->size, sizeof(uint32_t));
    unsigned size = s->wsize * sizeof(uint32_t);

    if (s->noinit) {
        /*
         * when initialization feature is disabled, simply map the final memory
         * region as the memory backend. Init-related arrays are left
         * uninitialized and should not be used.
         */
        memory_region_init_ram_nomigrate(&s->mem->sram, OBJECT(dev),
                                         TYPE_OT_SRAM_CTRL ".mem", size, errp);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem->sram);

        return;
    }

    /*
     * Use two 64-bit bitmap array to track which SRAM address have been
     * initialized. Only consider 32-bit memory slots (which differ from HW,
     * but should be sufficient to track common initialization): any write to
     * a single byte of a 4-byte memory cell is considered as if the whole
     * cell has been updated. Each 4-byte memory cell is tracked with a single
     * bit in the init_sram_bm bitmap array, where 1 means unitialized, i.e.
     * a fully zeroed array means that all cells have been written at least
     * once.
     * To avoid looping on too large arrays, use a seconday 64-bit bitmap array,
     * namely init_slot_bm, where each bit entry tracks a 64-bit slot of the
     * init_sram_bm array. Same logic applies for this array: once all bits are
     * cleared, all memory cells have been written at least once.
     * On such a condition, switch the I/O mapped memory to a RAM memory to
     * avoid performance bottleneck - which is used when accessing I/O rather
     * than host-backed memory.
     */
    size_t cell_slot_count = ot_sram_ctrl_get_slot_count(s->wsize);
    s->init_sram_bm = g_new0(uint64_t, cell_slot_count);
    memset(s->init_sram_bm, 0xff, cell_slot_count * sizeof(uint64_t));

    s->init_slot_count = ot_sram_ctrl_get_u64_slot(cell_slot_count + 64u - 1u);
    s->init_slot_bm = g_new0(uint64_t, s->init_slot_count);
    memset(s->init_slot_bm, 0xff, s->init_slot_count * sizeof(uint64_t));
    unsigned slot_offset = ot_sram_ctrl_get_u64_offset(cell_slot_count);
    if (slot_offset) {
        s->init_slot_bm[s->init_slot_count - 1u] = (1ull << slot_offset) - 1u;
    }

    memory_region_init_io(&s->mem->init, OBJECT(dev),
                          &ot_sram_ctrl_mem_init_ops, s,
                          TYPE_OT_SRAM_CTRL ".mem.init", size);
    memory_region_init_ram_nomigrate(&s->mem->sram, OBJECT(dev),
                                     TYPE_OT_SRAM_CTRL ".mem.sram", size, errp);

    /*
     * use an alias than points to the currently selected RAM backend, either
     * I/O for controlling access but really slow or host RAM backend for speed
     * but no fined-grained control, rather than directly swapping sysbus device
     * MMIO entry on initialization status changes. The alias enables decoupling
     * the internal implementation from the SRAM "clients" that may hold a
     * reference of the SRAM memory region, and may not signalled when the
     * backend is swapped. The alias enables to expose the same MemoryRegion
     * object while changing its actual backend on initialization demand.
     */
    memory_region_init_alias(&s->mem->alias, OBJECT(dev),
                             TYPE_OT_SRAM_CTRL ".mem", &s->mem->init, 0, size);
    /*
     * at start up, the SRAM memory is aliased to the I/O backend, so that
     * access can be controlled
     */
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem->alias);
}

static void ot_sram_ctrl_init(Object *obj)
{
    OtSramCtrlState *s = OT_SRAM_CTRL(obj);

    memory_region_init_io(&s->mmio, obj, &ot_sram_ctrl_regs_ops, s,
                          TYPE_OT_SRAM_CTRL ".regs", REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);

    s->mem = g_new0(OtSramCtrlMem, 1u);
    s->switch_mr_bh = qemu_bh_new(&ot_sram_ctrl_mem_switch_to_ram_fn, s);
    s->init_timer =
        timer_new_ns(OT_VIRTUAL_CLOCK, &ot_sram_ctrl_init_chunk_fn, s);
    s->prng = ot_prng_allocate();
    s->otp_key = g_new0(OtOTPKey, 1u);
}

static void ot_sram_ctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_sram_ctrl_reset;
    dc->realize = &ot_sram_ctrl_realize;
    device_class_set_props(dc, ot_sram_ctrl_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_sram_ctrl_info = {
    .name = TYPE_OT_SRAM_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtSramCtrlState),
    .instance_init = &ot_sram_ctrl_init,
    .class_init = &ot_sram_ctrl_class_init,
};

static void ot_sram_ctrl_register_types(void)
{
    type_register_static(&ot_sram_ctrl_info);
}

type_init(ot_sram_ctrl_register_types);
