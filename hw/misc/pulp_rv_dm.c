/*
 * QEMU Pulp Debug Module device
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *
 * For details check the documentation here:
 *    https://docs.opentitan.org/hw/ip/rv_dm/doc/
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
 * This file should be splitted so that RISCV info belong to target/riscv
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/bswap.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "exec/memattrs.h"
#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/irq.h"
#include "hw/jtag/tap_ctrl.h"
#include "hw/loader.h"
#include "hw/misc/pulp_rv_dm.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_otp_if.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/dm.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"


/*
 * Configuration
 */

#define DISCARD_REPEATED_IO_TRACES
#define DISTANCE_ACCESS_IO_TRACES 40u

/*
 * Register definitions
 */

/* clang-format off */

/* MMIO Regs */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(LATE_DEBUG_ENABLE_REGWEN, 0x4u)
    FIELD(LATE_DEBUG_ENABLE_REGWEN, REGWEN, 0u, 1u)
REG32(LATE_DEBUG_ENABLE, 0x8u)

/* MMIO Mem (Actions) */
REG32(HALTED, RISCV_DM_HALTED_OFFSET)
REG32(GOING, RISCV_DM_GOING_OFFSET)
REG32(RESUMING, RISCV_DM_RESUMING_OFFSET)
REG32(EXCEPTION, RISCV_DM_EXCEPTION_OFFSET)

/* Shared Mem (R/W access from debugger, R/X from Hart) */
REG32(WHERETO, 0x300u)
/*
 * Abstract cmd registers are used as a private program buffer to implement
 * abstract commands as semi-hardcoded SW, i.e. not in the debug ROM, w/
 * PULP_RV_DM_ABSTRACTCMD_COUNT slots
*/
REG32(ABSTRACTCMD_0, 0x338u)
/*
 * Program buffer registers are used to execute short code sequence and may be
 * uploaded from an external debugger, w/ PULP_RV_DM_PROGRAM_BUFFER_COUNT slots
 */
REG32(PROGRAM_BUFFER_0, PULP_RV_DM_PROGRAM_BUFFER_OFFSET)
/*
 * Data address registers is a view to the abstract data used w/ abstract
 * commands
 */
REG32(DATAADDR_0, PULP_RV_DM_DATAADDR_OFFSET)

/* MMIO mem (flags) */
REG32(FLAGS, RISCV_DM_FLAGS_OFFSET)
    FIELD(FLAGS, FLAG_GO, 0u, 1u)
    FIELD(FLAGS, FLAG_RESUME, 1u, 1u)

/* clang-format on */

/*
 * Macros
 */

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))
#define MEM_OFF(_r_) ((_r_) - R_WHERETO)

#define PULP_RV_DM_DMACT_BASE         (A_HALTED)
#define PULP_RV_DM_DMACT_SIZE         (A_EXCEPTION - A_HALTED + sizeof(uint32_t))
#define PULP_RV_DM_PROG_BASE          (A_WHERETO)
#define PULP_RV_DM_PROG_SIZE          0x100u
#define PULP_RV_DM_DMFLAG_BASE        (A_FLAGS)
#define PULP_RV_DM_DMFLAG_SIZE        (PULP_RV_DM_FLAGS_COUNT * sizeof(uint32_t))
#define PULP_RV_DM_DMFLAG_REGION_SIZE 0x400u

/*
 * Type definitions
 */

struct PulpRVDMState {
    SysBusDevice parent_obj;

    MemoryRegion regs; /* MMIO */
    MemoryRegion mem; /* Container for the following: */
    MemoryRegion unmapped_prog0; /* Unmapped 0x304..0x337 within prog */
    MemoryRegion unmapped_prog1; /* Unmapped 0x388..0x3ff within prog */
    MemoryRegion dmact; /* MMIO */
    MemoryRegion prog; /* ROM device */
    MemoryRegion dmflag; /* MMIO */
    MemoryRegion rom; /* ROM */

    qemu_irq *ack_out;
    IbexIRQ alert;

    uint32_t dmflag_regs[PULP_RV_DM_DMFLAG_SIZE / sizeof(uint32_t)];
    uint32_t late_debug_enable_regwen;
    uint32_t late_debug_enable;
    bool lc_hw_debug_en;
    bool lc_dft_en;

    unsigned hart_count;
    uint64_t idle_bm;
    uint64_t in_debug_bm;

    OtOTPIf *otp_ctrl;
};

#ifdef DISCARD_REPEATED_IO_TRACES
typedef struct {
    uint64_t pc;
    uint32_t addr;
    uint32_t value;
    size_t count;
} TraceCache;
#endif /* DISCARD_REPEATED_IO_TRACES */

/*
 * Constants
 */

#define R_ABSTRACTCMD_LAST (R_ABSTRACTCMD_0 + PULP_RV_DM_ABSTRACTCMD_COUNT - 1u)
#define R_PROGRAM_BUFFER_LAST \
    (R_PROGRAM_BUFFER_0 + PULP_RV_DM_PROGRAM_BUFFER_COUNT - 1u)
#define R_DATAADDR_LAST (R_DATAADDR_0 + PULP_RV_DM_DATA_COUNT - 1u)
#define R_FLAGS_0       R_FLAGS
#define R_FLAGS_LAST    (R_FLAGS_0 + PULP_RV_DM_FLAGS_COUNT - 1u)
#define PULP_RV_DM_MEM_WORDS \
    ((R_FLAGS_0 + PULP_RV_DM_FLAGS_COUNT) - R_WHERETO + sizeof(uint32_t))

/**
 * Default Abstract Command ROM contents at reset (dm_mem.sv p_abstract_cmd_rom
 * when cmd_i == 0 and HasSndScratch == 1).
 */
static const uint32_t DEFAULT_ABSTRACT_CMD[PULP_RV_DM_ABSTRACTCMD_COUNT] = {
    /* 338 */ 0x00000000u, /* illegal                            */
    /* 33c */ 0x00000517u, /* auipc   a0,0x0                     */
    /* 340 */ 0x00c55513u, /* srli    a0,a0,0xc                  */
    /* 344 */ 0x00c51513u, /* slli    a0,a0,0xc                  */
    /* 348 */ 0x00000013u, /* nop                                */
    /* 34c */ 0x00000013u, /* nop                                */
    /* 350 */ 0x00000013u, /* nop                                */
    /* 354 */ 0x00000013u, /* nop                                */
    /* 358 */ 0x7b302573u, /* csrr    a0,dscratch1               */
    /* 35c */ 0x00100073u, /* ebreak                             */
};

/**
 * Debug ROM blob for 2 debug scratch registers.
 *
 * Note that entry points should match ROM defined constants, namely:
 * - PULP_RV_DM_HALT_OFFSET
 * - PULP_RV_DM_RESUME_OFFSET
 * - PULP_RV_DM_EXCEPTION_OFFSET
 * - PULP_RV_DM_WHERETO_OFFSET
 */
static const uint32_t DEBUG_ROM[] = {
    /* clang-format off */
  /* entry:    HALT_OFFSET */
    /* 800 */  0x0180006fu, /* j       818 <_entry>               */
    /* 804 */  0x00000013u, /* nop                                */
  /* resume:   RESUME_OFFSET */
    /* 808 */  0x0840006fu, /* j       88c <_resume>              */
    /* 80c */  0x00000013u, /* nop                                */
  /* exception: EXCEPTION_OFFSET */
    /* 810 */  0x0500006fu, /* j       860 <_exception>           */
    /* 814 */  0x00000013u, /* nop                                */
  /* _entry: */
    /* 818 */  0x0ff0000fu, /* fence                              */
    /* 81c */  0x7b241073u, /* csrw    dscratch0,s0               */
    /* 820 */  0x7b351073u, /* csrw    dscratch1,a0               */
    /* 824 */  0x00000517u, /* auipc   a0,0x0                     */
    /* 828 */  0x00c55513u, /* srli    a0,a0,0xc                  */
    /* 82c */  0x00c51513u, /* slli    a0,a0,0xc                  */
  /* entry_loop: */
    /* 830 */  0xf1402473u, /* csrr    s0,mhartid                 */
    /* 834 */  0x10852023u, /* sw      s0,256(a0)    # HALTED     */
    /* 838 */  0x00a40433u, /* add     s0,s0,a0                   */
    /* 83c */  0x40044403u, /* lbu     s0,1024(s0)   # FLAGS      */
    /* 840 */  0x00147413u, /* andi    s0,s0,1                    */
    /* 844 */  0x02041c63u, /* bnez    s0,87c <going>             */
    /* 848 */  0xf1402473u, /* csrr    s0,mhartid                 */
    /* 84c */  0x00a40433u, /* add     s0,s0,a0                   */
    /* 850 */  0x40044403u, /* lbu     s0,1024(s0)   # FLAGS      */
    /* 854 */  0x00247413u, /* andi    s0,s0,2                    */
    /* 858 */  0xfa0418e3u, /* bnez    s0,808 <resume>            */
    /* 85c */  0xfd5ff06fu, /* j       830 <entry_loop>           */
  /* _exception: */
    /* 860 */  0x00000517u, /* auipc   a0,0x0                     */
    /* 864 */  0x00c55513u, /* srli    a0,a0,0xc                  */
    /* 868 */  0x00c51513u, /* slli    a0,a0,0xc                  */
    /* 86c */  0x10052c23u, /* sw      zero,280(a0)  # EXCEPTION  */
    /* 870 */  0x7b302573u, /* csrr    a0,dscratch1               */
    /* 874 */  0x7b202473u, /* csrr    s0,dscratch0               */
    /* 878 */  0x00100073u, /* ebreak                             */
  /* going: */
    /* 87c */  0x10052423u, /* sw      zero,264(a0)  # GOING      */
    /* 880 */  0x7b302573u, /* csrr    a0,dscratch1               */
    /* 884 */  0x7b202473u, /* csrr    s0,dscratch0               */
    /* 888 */  0xa79ff06fu, /* j       300 <whereto> # WHERETO    */
  /* _resume: */
    /* 88c */  0xf1402473u, /* csrr    s0,mhartid                 */
    /* 890 */  0x10852823u, /* sw      s0,272(a0)    # RESUMING   */
    /* 894 */  0x7b302573u, /* csrr    a0,dscratch1               */
    /* 898 */  0x7b202473u, /* csrr    s0,dscratch0               */
    /* 89c */  0x7b200073u, /* dret                               */
    /* clang-format on */
};

/*
 * Device implementation
 */

static void pulp_rv_dm_update_debug_en(PulpRVDMState *s)
{
    bool otp_dis_late_debug = false;
    if (s->otp_ctrl) {
        OtOTPIfClass *oc = OT_OTP_IF_GET_CLASS(s->otp_ctrl);
        const OtOTPHWCfg *hw_cfg = oc->get_hw_cfg(s->otp_ctrl);
        if (hw_cfg) {
            otp_dis_late_debug =
                (hw_cfg->dis_rv_dm_late_debug_mb8 == OT_MULTIBITBOOL8_TRUE);
        }
    }
    bool late_debug_en = (s->late_debug_enable == OT_MULTIBITBOOL32_TRUE);
    bool en = (otp_dis_late_debug || late_debug_en) ? s->lc_hw_debug_en :
                                                      s->lc_dft_en;
    memory_region_set_enabled(&s->mem, en);
}

static void pulp_rv_dm_load_rom(PulpRVDMState *s)
{
    /* do not use rom_add_blob_fixed_as as absolute address is not yet known */
    void *rom = memory_region_get_ram_ptr(&s->rom);
    if (!rom) {
        error_setg(&error_fatal, "cannot load debug ROM");
        /* linter may not know error_fatal never returns */
        abort();
    }
    memset(rom, 0, PULP_RV_DM_ROM_SIZE);
    memcpy(rom, DEBUG_ROM, sizeof(DEBUG_ROM));
}

/* NOLINTNEXTLINE */
static uint64_t pulp_rv_dm_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    PulpRVDMState *s = opaque;
    uint32_t val32 = 0u;
    (void)size;

    switch (R32_OFF(addr)) {
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    case R_LATE_DEBUG_ENABLE_REGWEN:
        val32 = s->late_debug_enable_regwen;
        break;
    case R_LATE_DEBUG_ENABLE:
        val32 = s->late_debug_enable;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    return val32;
};

static void pulp_rv_dm_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                  unsigned size)
{
    PulpRVDMState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    (void)size;

    switch (R32_OFF(addr)) {
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, 1);
            ibex_irq_set(&s->alert, 0);
        }
        break;
    case R_LATE_DEBUG_ENABLE_REGWEN:
        s->late_debug_enable_regwen &=
            val32 & R_LATE_DEBUG_ENABLE_REGWEN_REGWEN_MASK;
        break;
    case R_LATE_DEBUG_ENABLE:
        if (s->late_debug_enable_regwen) {
            s->late_debug_enable = val32;
            pulp_rv_dm_update_debug_en(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: LATE_DEBUG_ENABLE is locked\n",
                          __func__);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

#define RV_DM_REGS_COUNT 3u

static bool pulp_rv_dm_regs_accepts(void *opaque, hwaddr addr, unsigned size,
                                    bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)attrs;
    hwaddr reg = R32_OFF(addr);
    uint8_t permit = (reg == R_LATE_DEBUG_ENABLE) ? 0xfu : 0x1u;
    uint8_t byte_mask = (uint8_t)(((1u << size) - 1u) << (addr & 3u));
    return reg < RV_DM_REGS_COUNT && (!is_write || (permit & ~byte_mask) == 0u);
}

static MemTxResult pulp_rv_dm_dmact_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    uint32_t val32;
    MemTxResult res;
    (void)size;
    (void)opaque;
    (void)attrs;

    addr += PULP_RV_DM_DMACT_BASE;

    if (addr & 0x1u) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad alignment 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return MEMTX_ERROR;
    }

    switch (R32_OFF(addr)) {
    case R_HALTED:
    case R_GOING:
    case R_RESUMING:
    case R_EXCEPTION:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0u;
        res = MEMTX_OK;
        break;
    default:
        res = MEMTX_DECODE_ERROR;
        val32 = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    if (MEMTX_OK == res) {
        *val64 = val32;
    }

    return res;
};

static MemTxResult pulp_rv_dm_dmact_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    PulpRVDMState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    MemTxResult res;
    uint64_t pc = attrs.unspecified ? ibex_get_current_pc() : 0u;
    (void)size;

    addr += PULP_RV_DM_DMACT_BASE;

#ifdef DISCARD_REPEATED_IO_TRACES
    static TraceCache trace_cache;

    if (ABS((int)(trace_cache.pc) - (int)(pc)) >= DISTANCE_ACCESS_IO_TRACES ||
        trace_cache.addr != addr || trace_cache.value != val32) {
#endif /* DISCARD_REPEATED_IO_TRACES */
        trace_pulp_rv_dm_mem_write((unsigned int)addr, val32, pc);
#ifdef DISCARD_REPEATED_IO_TRACES
        trace_cache.count = 1;
    } else {
        trace_cache.count += 1;
    }
    trace_cache.pc = pc;
    trace_cache.addr = addr;
    trace_cache.value = val32;
#endif /* DISCARD_REPEATED_IO_TRACES */

    switch (R32_OFF(addr)) {
    case R_HALTED:
        if (val32 < s->hart_count) {
            s->in_debug_bm |= (1u << val32);
            if (!(s->idle_bm & (1u << val32))) {
                /*
                 * use a local cache to avoid flooding the DM with the park loop
                 * running crazy
                 */
                qemu_set_irq(s->ack_out[ACK_HALTED], (int)val32);
                s->idle_bm |= (1u << val32);
            } else if (!s->dmflag_regs[val32] && current_cpu) {
                /*
                 * The hart is already halted in the Debug ROM park loop and no
                 * GO/RESUME flag is pending. Park the vCPU so it becomes idle
                 * (allowing the icount warp timer to advance QEMU_CLOCK_VIRTUAL
                 * and leaving BQL/replay_mutex uncontended for JTAG and
                 * VIRTUAL_RT timers) until a DM flag or reset wakes it.
                 */
                current_cpu->stopped = true;
                cpu_exit(current_cpu);
            }
        }
        res = MEMTX_OK;
        break;
    case R_GOING:
        s->idle_bm &= ~(1u << val32);
        qemu_set_irq(s->ack_out[ACK_GOING], 1u);
        res = MEMTX_OK;
        break;
    case R_RESUMING:
        if (val32 < s->hart_count) {
            s->idle_bm &= ~(1u << val32);
            s->in_debug_bm &= ~(1u << val32);
            qemu_set_irq(s->ack_out[ACK_RESUMING], (int)val32);
        }
        res = MEMTX_OK;
        break;
    case R_EXCEPTION:
        qemu_set_irq(s->ack_out[ACK_EXCEPTION], 1u);
        res = MEMTX_OK;
        break;
    default:
        res = MEMTX_DECODE_ERROR;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    return res;
};

static bool pulp_rv_dm_prog_accepts(void *opaque, hwaddr addr, unsigned size,
                                    bool is_write, MemTxAttrs attrs)
{
    (void)attrs;
    return opaque != NULL && (!is_write || ((addr & 3u) == 0u && size == 4u));
}

static MemTxResult pulp_rv_dm_prog_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    PulpRVDMState *s = opaque;

    hwaddr abs_addr = addr + PULP_RV_DM_PROG_BASE;
    bool is_dm =
        (!attrs.unspecified) && (attrs.requester_id == PULP_RV_DM_REQUESTER_ID);
    bool is_data =
        (abs_addr >= A_DATAADDR_0) &&
        (abs_addr < (A_DATAADDR_0 + PULP_RV_DM_DATA_COUNT * sizeof(uint32_t)));
    bool in_debug = (s->in_debug_bm != 0u);

    /*
     * In dm_csrs.sv:627-647, data_q is held at '0 whenever
     * !dmcontrol_q.dmactive. Thus CPU writes to DATA0/DATA1 only latch into
     * data_q when the DM is active (e.g., during Debug Mode execution).
     */
    if (is_dm || (is_data && in_debug)) {
        uint8_t *ram = memory_region_get_ram_ptr(&s->prog);
        stl_le_p(ram + addr, (uint32_t)val64);
        memory_region_set_dirty(&s->prog, addr, size);
    }

    return MEMTX_OK;
}

static MemTxResult pulp_rv_dm_dmflag_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    PulpRVDMState *s = opaque;
    uint32_t val32;
    MemTxResult res;
    (void)size;
    (void)attrs;

    addr += PULP_RV_DM_DMFLAG_BASE;

    unsigned reg = R32_OFF(addr);

    /* NOLINTNEXTLINE */
    switch (reg) {
    case R_FLAGS_0 ... R_FLAGS_LAST:
        val32 = s->dmflag_regs[reg - R_FLAGS_0];
        res = MEMTX_OK;
        break;
    default:
        res = MEMTX_OK;
        val32 = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    if (MEMTX_OK == res) {
        *val64 = val32;
    }

    return res;
};

static MemTxResult pulp_rv_dm_dmflag_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    PulpRVDMState *s = opaque;
    uint32_t val32 = (uint32_t)val64;
    MemTxResult res;
    (void)size;

    addr += PULP_RV_DM_DMFLAG_BASE;

    unsigned reg = R32_OFF(addr);

    /* NOLINTNEXTLINE */
    switch (reg) {
    case R_FLAGS_0 ... R_FLAGS_LAST:
        if ((!attrs.unspecified) &&
            (attrs.requester_id == PULP_RV_DM_REQUESTER_ID)) {
            /* dm_access */
            s->dmflag_regs[reg - R_FLAGS_0] = val32;
            if (val32 && first_cpu && first_cpu->stopped &&
                !first_cpu->disabled) {
                cpu_resume(first_cpu);
            }
        } else {
            /* other (hart...) access */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: R/O register 0x%" HWADDR_PRIx "\n", __func__,
                          addr);
        }
        res = MEMTX_OK;
        break;
    default:
        res = MEMTX_OK;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    return res;
};

static const MemoryRegionOps pulp_rv_dm_regs_ops = {
    .read = &pulp_rv_dm_regs_read,
    .write = &pulp_rv_dm_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.accepts = &pulp_rv_dm_regs_accepts,
};

static bool pulp_rv_dm_dmact_accepts(void *opaque, hwaddr addr, unsigned size,
                                     bool is_write, MemTxAttrs attrs)
{
    (void)opaque;
    (void)size;
    (void)attrs;
    return !is_write || (addr & 3u) == 0u;
}

static const MemoryRegionOps pulp_rv_dm_dmact_ops = {
    .read_with_attrs = &pulp_rv_dm_dmact_read_with_attrs,
    .write_with_attrs = &pulp_rv_dm_dmact_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.accepts = &pulp_rv_dm_dmact_accepts,
};

static const MemoryRegionOps pulp_rv_dm_prog_ops = {
    .write_with_attrs = &pulp_rv_dm_prog_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.accepts = &pulp_rv_dm_prog_accepts,
};

static const MemoryRegionOps pulp_rv_dm_dmflag_ops = {
    .read_with_attrs = &pulp_rv_dm_dmflag_read_with_attrs,
    .write_with_attrs = &pulp_rv_dm_dmflag_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.accepts = &pulp_rv_dm_prog_accepts,
};

static void pulp_rv_dm_lc_hw_debug_en_in(void *opaque, int n, int level)
{
    PulpRVDMState *s = opaque;
    (void)n;
    s->lc_hw_debug_en = (bool)level;
    pulp_rv_dm_update_debug_en(s);
}

static void pulp_rv_dm_lc_dft_en_in(void *opaque, int n, int level)
{
    PulpRVDMState *s = opaque;
    (void)n;
    s->lc_dft_en = (bool)level;
    pulp_rv_dm_update_debug_en(s);
}

static void pulp_rv_dm_reset(DeviceState *dev)
{
    PulpRVDMState *s = PULP_RV_DM(dev);

    ibex_irq_set(&s->alert, false);

    uint8_t *prog_ptr = memory_region_get_ram_ptr(&s->prog);
    memset(prog_ptr, 0, PULP_RV_DM_PROG_SIZE);
    memcpy(prog_ptr + (A_ABSTRACTCMD_0 - PULP_RV_DM_PROG_BASE),
           DEFAULT_ABSTRACT_CMD, sizeof(DEFAULT_ABSTRACT_CMD));
    memset(s->dmflag_regs, 0, sizeof(s->dmflag_regs));
    s->late_debug_enable_regwen = 1u;
    s->late_debug_enable = OT_MULTIBITBOOL32_FALSE;
    pulp_rv_dm_update_debug_en(s);

    s->idle_bm = 0;
    s->in_debug_bm = 0;
}

static void pulp_rv_dm_init(Object *obj)
{
    PulpRVDMState *s = PULP_RV_DM(obj);

    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned max_cpus = ms->smp.max_cpus;
    s->hart_count = MIN(max_cpus, 64u);
    s->lc_hw_debug_en = true;
    s->lc_dft_en = true;

    /* Top-level container */
    memory_region_init(&s->mem, obj, TYPE_PULP_RV_DM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem);

    /* Top-level MMIO */
    memory_region_init_io(&s->regs, obj, &pulp_rv_dm_regs_ops, s,
                          TYPE_PULP_RV_DM ".regs", PULP_RV_DM_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->regs);

    /* Mem container content */
    memory_region_init_io(&s->dmact, obj, &pulp_rv_dm_dmact_ops, s,
                          TYPE_PULP_RV_DM ".act", PULP_RV_DM_DMACT_SIZE);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_DMACT_BASE, &s->dmact);

    memory_region_init_rom_device_nomigrate(&s->prog, obj, &pulp_rv_dm_prog_ops,
                                            s, TYPE_PULP_RV_DM ".prog",
                                            PULP_RV_DM_PROG_SIZE, &error_fatal);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_PROG_BASE, &s->prog);

    memory_region_init_io(&s->unmapped_prog0, obj, &pulp_rv_dm_prog_ops, NULL,
                          TYPE_PULP_RV_DM ".unmapped_prog0", 0x34);
    memory_region_add_subregion_overlap(&s->mem, 0x304, &s->unmapped_prog0, 1);

    memory_region_init_io(&s->unmapped_prog1, obj, &pulp_rv_dm_prog_ops, NULL,
                          TYPE_PULP_RV_DM ".unmapped_prog1", 0x78);
    memory_region_add_subregion_overlap(&s->mem, 0x388, &s->unmapped_prog1, 1);

    memory_region_init_io(&s->dmflag, obj, &pulp_rv_dm_dmflag_ops, s,
                          TYPE_PULP_RV_DM ".flag",
                          PULP_RV_DM_DMFLAG_REGION_SIZE);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_DMFLAG_BASE, &s->dmflag);
    s->dmflag.disable_reentrancy_guard = true;

    memory_region_init_rom_device_nomigrate(&s->rom, obj, &pulp_rv_dm_prog_ops,
                                            NULL, TYPE_PULP_RV_DM ".rom",
                                            PULP_RV_DM_ROM_SIZE, &error_abort);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_ROM_BASE, &s->rom);

    s->ack_out = g_new0(qemu_irq, ACK_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->ack_out, PULP_RV_DM_ACK_OUT_LINES,
                             ACK_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &pulp_rv_dm_lc_hw_debug_en_in,
                            PULP_RV_DM_LC_HW_DEBUG_EN, 1);
    qdev_init_gpio_in_named(DEVICE(obj), &pulp_rv_dm_lc_dft_en_in,
                            PULP_RV_DM_LC_DFT_EN, 1);

    pulp_rv_dm_load_rom(s);

    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
}

static const Property pulp_rv_dm_properties[] = {
    DEFINE_PROP_LINK("otp-ctrl", PulpRVDMState, otp_ctrl, TYPE_OT_OTP_IF,
                     OtOTPIf *),
};

static void pulp_rv_dm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    device_class_set_legacy_reset(dc, &pulp_rv_dm_reset);
    device_class_set_props(dc, pulp_rv_dm_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pulp_rv_dm_info = {
    .name = TYPE_PULP_RV_DM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PulpRVDMState),
    .instance_init = &pulp_rv_dm_init,
    .class_init = &pulp_rv_dm_class_init,
};

static void pulp_rv_dm_register_types(void)
{
    type_register_static(&pulp_rv_dm_info);
}

type_init(pulp_rv_dm_register_types);
