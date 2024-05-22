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

#define PULP_RV_DM_DMACT_BASE  (A_HALTED)
#define PULP_RV_DM_DMACT_SIZE  (A_EXCEPTION - A_HALTED + sizeof(uint32_t))
#define PULP_RV_DM_PROG_BASE   (A_WHERETO)
#define PULP_RV_DM_PROG_SIZE   0x100u
#define PULP_RV_DM_DMFLAG_BASE (A_FLAGS)
#define PULP_RV_DM_DMFLAG_SIZE (PULP_RV_DM_FLAGS_COUNT * sizeof(uint32_t))

/*
 * Type definitions
 */

struct PulpRVDMState {
    SysBusDevice parent_obj;

    MemoryRegion regs; /* MMIO */
    MemoryRegion mem; /* Container for the following: */
    MemoryRegion dmact; /* MMIO */
    MemoryRegion prog; /* ROM device */
    MemoryRegion dmflag; /* MMIO */
    MemoryRegion rom; /* ROM */

    qemu_irq *ack_out;
    IbexIRQ alert;

    uint32_t dmflag_regs[PULP_RV_DM_DMFLAG_SIZE / sizeof(uint32_t)];

    unsigned hart_count;
    uint64_t idle_bm;
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
    /* 800 */  0x00c0006fu, /* j   80c <_entry>                   */
  /* resume:   RESUME_OFFSET */
    /* 804 */  0x07c0006fu, /* j   880 <_resume>                  */
  /* exception: EXCEPTION */
    /* 808 */  0x04c0006fu, /* j   854 <_exception>               */
  /*_entry: */
    /* 80c */  0x0ff0000fu, /* fence                              */
    /* 810 */  0x7b241073u, /* csrw    dscratch0,s0               */
    /* 814 */  0x7b351073u, /* csrw    dscratch1,a0               */
    /* 818 */  0x00000517u, /* auipc   a0,0x0                     */
    /* 81c */  0x00c55513u, /* srl     a0,a0,0xc                  */
    /* 820 */  0x00c51513u, /* sll     a0,a0,0xc                  */
  /* entry_loop: */
    /* 824 */  0xf1402473u, /* csrr    s0,mhartid                 */
    /* 828 */  0x10852023u, /* sw      s0,256(a0)    # HALTED     */
    /* 82c */  0x00a40433u, /* add     s0,s0,a0                   */
    /* 830 */  0x40044403u, /* lbu     s0,1024(s0)   # FLAGS      */
    /* 834 */  0x00147413u, /* and     s0,s0,1                    */
    /* 838 */  0x02041c63u, /* bnez    s0,870 <going>             */
    /* 83c */  0xf1402473u, /* csrr    s0,mhartid                 */
    /* 840 */  0x00a40433u, /* add     s0,s0,a0                   */
    /* 844 */  0x40044403u, /* lbu     s0,1024(s0)   # FLAGS      */
    /* 848 */  0x00247413u, /* and     s0,s0,2                    */
    /* 84c */  0xfa041ce3u, /* bnez    s0,804 <resume>            */
    /* 850 */  0xfd5ff06fu, /* j       824 <entry_loop>           */
  /* _exception: */
    /* 854 */  0x00000517u, /* auipc   a0,0x0                     */
    /* 858 */  0x00c55513u, /* srl     a0,a0,0xc                  */
    /* 85c */  0x00c51513u, /* sll     a0,a0,0xc                  */
    /* 860 */  0x10052623u, /* sw      zero,268(a0)  # EXCEPTION  */
    /* 864 */  0x7b302573u, /* csrr    a0,dscratch1               */
    /* 868 */  0x7b202473u, /* csrr    s0,dscratch0               */
    /* 86c */  0x00100073u, /* ebreak                             */
  /* going: */
    /* 870 */  0x10052223u, /* sw      zero,260(a0)  # GOING      */
    /* 874 */  0x7b302573u, /* csrr    a0,dscratch1               */
    /* 878 */  0x7b202473u, /* csrr    s0,dscratch0               */
    /* 87c */  0xa85ff06fu, /* j       300 <whereto> # WHERETO    */
 /* _resume: */
    /* 880 */  0xf1402473u, /* csrr    s0,mhartid                 */
    /* 884 */  0x10852423u, /* sw      s0,264(a0)    # RESUMING   */
    /* 888 */  0x7b302573u, /* csrr    a0,dscratch1               */
    /* 88c */  0x7b202473u, /* csrr    s0,dscratch0               */
    /* 890 */  0x7b200073u, /* dret                               */
    /* clang-format on */
};

/*
 * Device implementation
 */

static void pulp_rv_dm_load_rom(PulpRVDMState *s)
{
    /* do not use rom_add_blob_fixed_as as absolute address is not yet known */
    void *rom = memory_region_get_ram_ptr(&s->rom);
    if (!rom) {
        error_setg(&error_fatal, "cannot load debug ROM");
        /* linter may not know error_fatal never returns */
        abort();
    }
    memcpy(rom, DEBUG_ROM, sizeof(DEBUG_ROM));
}

static uint64_t pulp_rv_dm_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)size;
    /* the unique register is W/O */
    qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%" HWADDR_PRIx "\n",
                  __func__, addr);

    return 0u;
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
        ibex_irq_set(&s->alert, (int)(bool)val32);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

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
            if (!(s->idle_bm & (1u << val32))) {
                /*
                 * use a local cache to avoid flooding the DM with the park loop
                 * running crazy
                 */
                qemu_set_irq(s->ack_out[ACK_HALTED], (int)val32);
                s->idle_bm |= (1u << val32);
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
            (attrs.requester_id == JTAG_MEMTX_REQUESTER_ID)) {
            /* dm_access */
            s->dmflag_regs[reg - R_FLAGS_0] = val32;
        } else {
            /* other (hart...) access */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: R/O register 0x%" HWADDR_PRIx "\n", __func__,
                          addr);
        }
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

static const MemoryRegionOps pulp_rv_dm_regs_ops = {
    .read = &pulp_rv_dm_regs_read,
    .write = &pulp_rv_dm_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps pulp_rv_dm_dmact_ops = {
    .read_with_attrs = &pulp_rv_dm_dmact_read_with_attrs,
    .write_with_attrs = &pulp_rv_dm_dmact_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps pulp_rv_dm_dmflag_ops = {
    .read_with_attrs = &pulp_rv_dm_dmflag_read_with_attrs,
    .write_with_attrs = &pulp_rv_dm_dmflag_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void pulp_rv_dm_reset(DeviceState *dev)
{
    PulpRVDMState *s = PULP_RV_DM(dev);

    ibex_irq_set(&s->alert, false);

    memset(memory_region_get_ram_ptr(&s->prog), 0, PULP_RV_DM_PROG_SIZE);
    memset(s->dmflag_regs, 0, sizeof(s->dmflag_regs));
}

static void pulp_rv_dm_init(Object *obj)
{
    PulpRVDMState *s = PULP_RV_DM(obj);

    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned max_cpus = ms->smp.max_cpus;
    s->hart_count = MIN(max_cpus, 64u);

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

    memory_region_init_ram_nomigrate(&s->prog, obj, TYPE_PULP_RV_DM ".prog",
                                     PULP_RV_DM_PROG_SIZE, &error_fatal);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_PROG_BASE, &s->prog);

    memory_region_init_io(&s->dmflag, obj, &pulp_rv_dm_dmflag_ops, s,
                          TYPE_PULP_RV_DM ".flag", PULP_RV_DM_DMFLAG_SIZE);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_DMFLAG_BASE, &s->dmflag);
    s->dmflag.disable_reentrancy_guard = true;

    memory_region_init_rom_nomigrate(&s->rom, obj, TYPE_PULP_RV_DM ".rom",
                                     PULP_RV_DM_ROM_SIZE, &error_abort);
    memory_region_add_subregion(&s->mem, PULP_RV_DM_ROM_BASE, &s->rom);

    s->ack_out = g_new0(qemu_irq, ACK_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->ack_out, PULP_RV_DM_ACK_OUT_LINES,
                             ACK_COUNT);

    pulp_rv_dm_load_rom(s);

    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
}

static void pulp_rv_dm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &pulp_rv_dm_reset;
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
