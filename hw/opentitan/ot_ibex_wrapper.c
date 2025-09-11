/*
 * QEMU OpenTitan Ibex wrapper device
 *
 * Copyright (c) 2022-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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
 *
 * Note: there are two modes to handle address remapping:
 *   - default mode: use an MMU-like implementation (via ot_vmapper) to remap
 *     addresses. This mode enables to remap instruction accesses and data
 *     accesses independently, as the real HW. However, due to QEMU limitations,
 *     addresses and mapped region sizes should be aligned and multiple of 4096
 *     bytes, i.e. a standard MMU page size. This is the recommended mode.
 *   - legacy mode: This mode has no address nor size limitations, however it
 *     cannot distinguish instruction accesses from data accesses, which means
 *     that both kind of accesses must be defined for each active remapping slot
 *     for the remapping to be enabled. Moreover it relies on MemoryRegion
 *     aliasing and may not be as robust as the default mode. It is recommended
 *     to use the default mode whenever possible. To enable this legacy mode,
 *     set the "alias-mode" property to true.
 */

#include "qemu/osdep.h"
#include <string.h>
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_ibex_wrapper.h"
#include "hw/opentitan/ot_vmapper.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "sysemu/runstate.h"
#include "trace.h"

/* DEBUG: define to print the full memory view on remap */
#undef PRINT_MTREE
/* DEBUG: define to print the enumerated list of registers */
#undef PRINT_REGNAMES

#define NUM_SW_ALERTS 2u
#define NUM_ALERTS    4u

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, FATAL_SW, 0u, 1u)
    FIELD(ALERT_TEST, RECOV_SW, 1u, 1u)
    FIELD(ALERT_TEST, FATAL_HW, 2u, 1u)
    FIELD(ALERT_TEST, RECOV_HW, 3u, 1u)
REG32(SW_RECOV_ERR, 0x4u)
    FIELD(SW_RECOV_ERR, VAL, 0u, 4u)
REG32(SW_FATAL_ERR, 0x8u)
    FIELD(SW_FATAL_ERR, VAL, 0u, 4u)
SHARED_FIELD(REGWEN_EN, 0u, 1u)
SHARED_FIELD(ADDR_EN, 0u, 1u)
SHARED_FIELD(NMI_ALERT_EN, 0u, 1u)
SHARED_FIELD(NMI_WDOG_EN, 1u, 1u)
SHARED_FIELD(ERR_STATUS_REG_INTG, 0u, 1u)
SHARED_FIELD(ERR_STATUS_FATAL_INTG, 8u, 1u)
SHARED_FIELD(ERR_STATUS_FATAL_CORE, 9u, 1u)
SHARED_FIELD(ERR_STATUS_RECOV_CORE, 10u, 1u)
SHARED_FIELD(RND_STATUS_RND_DATA_VALID, 0u, 1u)
SHARED_FIELD(RND_STATUS_RND_DATA_FIPS, 1u, 1u)
SHARED_FIELD(DV_SIM_STATUS_CODE, 0u, 16u)
SHARED_FIELD(DV_SIM_STATUS_INFO, 16u, 16u)
/* clang-format on */

#define ALERT_TEST_MASK \
    (R_ALERT_TEST_FATAL_SW_MASK | R_ALERT_TEST_RECOV_SW_MASK | \
     R_ALERT_TEST_FATAL_HW_MASK | R_ALERT_TEST_RECOV_HW_MASK)

#define ERR_STATUS_MASK \
    (ERR_STATUS_REG_INTG_MASK | ERR_STATUS_FATAL_INTG_MASK | \
     ERR_STATUS_FATAL_CORE_MASK | ERR_STATUS_RECOV_CORE_MASK)

#define NMI_MASK (NMI_ALERT_EN_MASK | NMI_WDOG_EN_MASK)

/* OpenTitan SW log severities. */
typedef enum {
    TEST_LOG_SEVERITY_INFO,
    TEST_LOG_SEVERITY_WARN,
    TEST_LOG_SEVERITY_ERROR,
    TEST_LOG_SEVERITY_FATAL,
} OtIbexTestLogLevel;

/* OpenTitan SW log metadata used to format a log line. */
typedef struct {
    OtIbexTestLogLevel severity;
    uint32_t file_name_ptr; /* const char * in RV32 */
    uint32_t line;
    uint32_t nargs;
    uint32_t format_ptr; /* const char * in RV32 */
} OtIbexTestLogFields;

typedef enum {
    TEST_LOG_STATE_IDLE,
    TEST_LOG_STATE_ARG,
    TEST_LOG_STATE_ERROR,
} OtIbexTestLogState;

/*
 * These enumerated values are not HW values, however the two last values are
 * documented by DV SW as:"This is a terminal state. Any code appearing after
 * this value is set is unreachable."
 *
 * There are therefore handled as special HW-SW case that triggers explicit
 * QEMU termination with a special exit code.
 */
typedef enum {
    TEST_STATUS_IN_BOOT_ROM = 0xb090, /* 'bogo', BOotrom GO */
    TEST_STATUS_IN_BOOT_ROM_HALT = 0xb057, /* 'bost', BOotrom STop */
    TEST_STATUS_IN_TEST = 0x4354, /* 'test' */
    TEST_STATUS_IN_WFI = 0x1d1e, /* 'idle' */
    TEST_STATUS_PASSED = 0x900d, /* 'good' */
    TEST_STATUS_FAILED = 0xbaad /* 'baad' */
} OtIbexTestStatus;

typedef enum {
    ACCESS_INSN,
    ACCESS_DATA,
    ACCESS_COUNT,
} OtIbexRemapAccess;

enum {
    DV_SIM_STATUS,
    DV_SIM_LOG,
    DV_SIM_WIN2,
    DV_SIM_WIN3,
    DV_SIM_WIN4,
    DV_SIM_WIN5,
    DV_SIM_WIN6,
    DV_SIM_WIN7,
    DV_SIM_COUNT,
};

typedef struct {
    OtIbexTestLogState state;
    AddressSpace *as;
    OtIbexTestLogFields fields;
    unsigned arg_count;
    uintptr_t *args; /* arguments */
    bool *strargs; /* whether slot should be freed or a not */
    const char *fmtptr; /* current pointer in format string */
    char *filename;
    char *format;
} OtIbexTestLogEngine;

typedef uint32_t (*ot_ibex_wrapper_reg_read_fn)(OtIbexWrapperState *s,
                                                unsigned reg);
typedef void (*ot_ibex_wrapper_reg_write_fn)(OtIbexWrapperState *s,
                                             unsigned reg, uint32_t value);

typedef struct {
    uint32_t regwen;
    uint32_t addr_en;
    uint32_t addr_matching;
    uint32_t remap_addr;
} OtIbexRemap;

typedef struct {
    uint32_t alert_test;
    uint32_t sw_recov_err;
    uint32_t sw_fatal_err;
    /* physical location for remapper regs are moved to the end of struct */
    uint32_t nmi_enable;
    uint32_t nmi_state;
    uint32_t err_status;
    uint32_t rnd_data;
    uint32_t rnd_status;
    uint32_t fpga_info;
    uint32_t _reserved[7u];
    uint32_t dv_sim_win[DV_SIM_COUNT];
    /* note: remap pointer introduce padding here */
    OtIbexRemap *remap[ACCESS_COUNT]; /* num_regions */
} OtIbexWrapperRegs;

typedef struct {
    ot_ibex_wrapper_reg_read_fn read;
    ot_ibex_wrapper_reg_write_fn write;
    uint32_t mask; /* the mask to apply to the written value */
} OtIbexWrapperAccess;

struct OtIbexWrapperState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion *remappers;
    MemoryRegion *sys_mem;
    IbexIRQ alerts[NUM_ALERTS];

    OtIbexTestLogEngine *log_engine;
    OtIbexWrapperRegs regs; /* not ordered by register index */
    OtIbexWrapperAccess *access_table; /* ordered by register index */
    uint32_t **access_regs; /* ordered by register index */
    char **reg_names; /* ordered by register index */
    CPUState *cpu;
    unsigned reg_count; /* total count of registers */
    unsigned remap_reg_count; /* count of remapping registers */
    uint8_t cpu_en_bm;
    bool entropy_requested;
    bool edn_connected;
    bool esc_rx;

    char *ot_id;
    char *lc_ignore_ids;
    OtEDNState *edn;
    OtVMapperState *vmapper;
    uint8_t num_regions;
    uint8_t edn_ep;
    uint8_t qemu_version;
    bool lc_ignore;
    bool alias_mode;
    bool dv_sim_status_exit;
    CharBackend chr;
};

struct OtIbexWrapperClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#define OT_IBEX_CPU_EN_MASK (((1u << OT_IBEX_CPU_EN_COUNT)) - 1u)

static const char MISSING_LOG_STRING[] = "(?)";

#define CASE_RANGE(_reg_, _cnt_) (_reg_)...((_reg_) + (_cnt_) - (1u))

#define xtrace_ot_ibex_wrapper_info(_s_, _msg_) \
    trace_ot_ibex_wrapper_info((_s_)->ot_id, __func__, __LINE__, _msg_)
#define xtrace_ot_ibex_wrapper_error(_s_, _msg_) \
    trace_ot_ibex_wrapper_error((_s_)->ot_id, __func__, __LINE__, _msg_)

#define REG_NAME_LENGTH 24u /* > "DBUS_ADDR_MATCHING_31" */

#define LAST_STATIC_REG     sw_fatal_err
#define FIRST_REMAP_REG_POS ((LAST_STATIC_REG_POS) + 1u)
#define LAST_STATIC_REG_POS R32_POS(LAST_STATIC_REG)
#define R32_OFF(_r_)        ((_r_) / sizeof(uint32_t))
#define R32_POS(_r_)        (offsetof(OtIbexWrapperRegs, _r_) / sizeof(uint32_t))
#define R32_DYN_OFFSET(_s_, _r_) \
    (R32_POS(_r_) <= LAST_STATIC_REG_POS ? 0 : s->remap_reg_count)
#define R32_DYN_POS(_s_, _r_) (R32_POS(_r_) + R32_DYN_OFFSET(_s_, _r_))
#define REG_NAME(_s_, _reg_) \
    ((_reg_) < (_s_)->reg_count ? (_s_)->reg_names[(_reg_)] : "?")
#define CREATE_NAME_REG_AT(_s_, _off_, _reg_) \
    strncpy((_s_)->reg_names[_off_], stringify(_reg_), REG_NAME_LENGTH - 1)
#define CREATE_NAME_REG(_s_, _reg_) CREATE_NAME_REG_AT(_s_, R_##_reg_, _reg_)
#define CREATE_NAME_REG_IX_AT(_s_, _pfx_, _off_, _ix_, _reg_) \
    do { \
        int l = snprintf((_s_)->reg_names[(_off_)], REG_NAME_LENGTH, \
                         "%s" stringify(_reg_) "_%u", (_pfx_), (_ix_)); \
        g_assert((unsigned)l < REG_NAME_LENGTH); \
    } while (0)

/*
 * Alert management
 */

static void ot_ibex_wrapper_update_alerts(OtIbexWrapperState *s)
{
    uint32_t level = s->regs.alert_test;

    if (s->regs.sw_fatal_err != OT_MULTIBITBOOL4_FALSE) {
        level |= R_SW_FATAL_ERR_VAL_MASK;
    }

    for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {
        ibex_irq_set(&s->alerts[ix], (int)((level >> ix) & 0x1u));
    }
}

/*
 * Entropy management (RND)
 */

static void ot_ibex_wrapper_fill_entropy(void *opaque, uint32_t bits, bool fips)
{
    OtIbexWrapperState *s = opaque;

    trace_ot_ibex_wrapper_fill_entropy(s->ot_id, bits, fips);

    s->regs.rnd_data = bits;
    s->regs.rnd_status = RND_STATUS_RND_DATA_VALID_MASK;
    if (fips) {
        s->regs.rnd_status |= RND_STATUS_RND_DATA_FIPS_MASK;
    }

    s->entropy_requested = false;
}

static bool ot_ibex_wrapper_has_edn(OtIbexWrapperState *s)
{
    return (s->edn != NULL) && (s->edn_ep != UINT8_MAX);
}

static void ot_ibex_wrapper_request_entropy(OtIbexWrapperState *s)
{
    if (!s->entropy_requested && ot_ibex_wrapper_has_edn(s)) {
        if (unlikely(!s->edn_connected)) {
            ot_edn_connect_endpoint(s->edn, s->edn_ep,
                                    &ot_ibex_wrapper_fill_entropy, s);
            s->edn_connected = true;
        }
        s->entropy_requested = true;
        trace_ot_ibex_wrapper_request_entropy(s->ot_id, s->entropy_requested);
        if (ot_edn_request_entropy(s->edn, s->edn_ep)) {
            s->entropy_requested = false;
            xtrace_ot_ibex_wrapper_error(s, "failed to request entropy");
        }
    }
}

/*
 * Address translation management (legacy mode using memory region aliases)
 */

static void
ot_ibex_wrapper_remapper_destroy(OtIbexWrapperState *s, unsigned slot)
{
    g_assert(slot < (unsigned)s->num_regions);
    MemoryRegion *mr = &s->remappers[slot];
    if (memory_region_is_mapped(mr)) {
        trace_ot_ibex_wrapper_unmap(s->ot_id, slot);
        memory_region_transaction_begin();
        memory_region_set_enabled(mr, false);
        /* QEMU memory model enables unparenting alias regions */
        memory_region_del_subregion(s->sys_mem, mr);
        memory_region_transaction_commit();
    }
}

/* NOLINTNEXTLINE */
static bool ot_ibex_wrapper_mr_map_offset(hwaddr *offset,
                                          const MemoryRegion *root, hwaddr dst,
                                          size_t size, const MemoryRegion *tmr)
{
    if (root == tmr) {
        return true;
    }

    const MemoryRegion *mr;

    QTAILQ_FOREACH(mr, &root->subregions, subregions_link) {
        if (dst < mr->addr ||
            (dst + size) > (mr->addr + int128_getlo(mr->size))) {
            continue;
        }

        bool ret;

        if (mr->alias) {
            hwaddr alias_offset = mr->addr - mr->alias_offset;
            dst -= alias_offset;

            ret = ot_ibex_wrapper_mr_map_offset(offset, mr->alias, dst, size,
                                                tmr);
            if (ret) {
                /*
                 * the selected MR tree leads to the target region, so update
                 * the alias offset with the local offset
                 */
                *offset += alias_offset;
            }
        } else {
            ret = ot_ibex_wrapper_mr_map_offset(offset, mr, dst, size, tmr);
            if (ret) {
                *offset += mr->addr;
            }
        }

        return ret;
    }

    return false;
}

static void ot_ibex_wrapper_remapper_create(
    OtIbexWrapperState *s, unsigned slot, hwaddr dst, hwaddr src, size_t size)
{
    g_assert(slot < (unsigned)s->num_regions);
    MemoryRegion *mr = &s->remappers[slot];
    g_assert(!memory_region_is_mapped(mr));

    int priority = (int)((unsigned)s->num_regions - slot);

    MemoryRegion *mr_dst;

    char *name = g_strdup_printf(TYPE_OT_IBEX_WRAPPER "-remap[%u]", slot);

    memory_region_transaction_begin();
    /*
     * try to map onto the actual device if there's a single one, otherwise
     * map on the whole address space.
     */
    MemoryRegionSection mrs;
    mrs = memory_region_find(s->sys_mem, dst, (uint64_t)size);
    size_t mrs_lsize = int128_getlo(mrs.size);
    mr_dst = (mrs.mr && mrs_lsize >= size) ? mrs.mr : s->sys_mem;

    /*
     * adjust the offset if the memory region target for the mapping
     * is itself mapped through memory region(s)
     */
    hwaddr offset = 0;
    if (ot_ibex_wrapper_mr_map_offset(&offset, s->sys_mem, dst, size, mr_dst)) {
        offset = dst - offset;
    }

    trace_ot_ibex_wrapper_map(s->ot_id, slot, src, dst, size, mr_dst->name,
                              (uint32_t)offset);
    memory_region_init_alias(mr, OBJECT(s), name, mr_dst, offset,
                             (uint64_t)size);
    memory_region_add_subregion_overlap(s->sys_mem, src, mr, priority);
    memory_region_set_enabled(mr, true);
    memory_region_transaction_commit();
    g_free(name);

#ifdef PRINT_MTREE
    mtree_info(false, false, false, true);
#endif
}

static void ot_ibex_wrapper_update_remap_mr(
    OtIbexWrapperState *s, OtIbexRemapAccess access, unsigned slot)
{
    g_assert(slot < (unsigned)s->num_regions);
    /*
     * Warning:
     * for now, QEMU is unable to distinguish instruction or data access.
     * in this implementation, we chose to enable remap whenever either D or I
     * remapping is selected, and both D & I configuration match; we disable
     * translation when both D & I are remapping are disabled
     */
    (void)access;

    bool en_remap_i = s->regs.remap[ACCESS_INSN][slot].addr_en;
    bool en_remap_d = s->regs.remap[ACCESS_DATA][slot].addr_en;
    if (!en_remap_i && !en_remap_d) {
        /* disable */
        ot_ibex_wrapper_remapper_destroy(s, slot);
    } else {
        uint32_t src_match_i = s->regs.remap[ACCESS_INSN][slot].addr_matching;
        uint32_t src_match_d = s->regs.remap[ACCESS_DATA][slot].addr_matching;
        if (src_match_i != src_match_d) {
            /* I and D do not match, do nothing */
            xtrace_ot_ibex_wrapper_info(s, "src remapping do not match");
            return;
        }
        uint32_t remap_addr_i = s->regs.remap[ACCESS_INSN][slot].remap_addr;
        uint32_t remap_addr_d = s->regs.remap[ACCESS_DATA][slot].remap_addr;
        if (remap_addr_i != remap_addr_d) {
            /* I and D do not match, do nothing */
            xtrace_ot_ibex_wrapper_info(s, "dst remapping do not match");
            return;
        }
        /* enable */
        uint32_t map_size = (-src_match_i & (src_match_i + 1u)) << 1u;
        uint32_t map_mask = ~(map_size - 1u);
        uint32_t src_base = src_match_i & map_mask;
        uint32_t dst_base = remap_addr_i & map_mask;

        ot_ibex_wrapper_remapper_destroy(s, slot);
        ot_ibex_wrapper_remapper_create(s, slot, (hwaddr)dst_base,
                                        (hwaddr)src_base, (size_t)map_size);
    }
}

static void ot_ibex_wrapper_update_remap_vmap(
    OtIbexWrapperState *s, OtIbexRemapAccess access, unsigned slot)

{
    g_assert(slot < s->num_regions);
    g_assert(s->vmapper);
    g_assert(access < ACCESS_COUNT);

    bool enable = s->regs.remap[access][slot].addr_en;
    uint32_t match_addr = s->regs.remap[access][slot].addr_matching;
    uint32_t remap_addr = s->regs.remap[access][slot].remap_addr;

    uint32_t map_size = (-match_addr & (match_addr + 1u)) << 1u;
    uint32_t map_mask = ~(map_size - 1u);
    uint32_t src_base = match_addr & map_mask;
    uint32_t dst_base = remap_addr & map_mask;

    OtVMapperClass *vc = OT_VMAPPER_GET_CLASS(s->vmapper);

    vc->translate(s->vmapper, access == ACCESS_INSN, slot, src_base, dst_base,
                  enable ? map_size : 0);
}

/*
 * DV logging management
 */

static bool
ot_ibex_wrapper_log_load_string(OtIbexWrapperState *s, hwaddr addr, char **str)
{
    OtIbexTestLogEngine *eng = s->log_engine;

    /*
     * Logging needs to access strings that are stored in guest memory.
     * This function adopts a "best effort" strategy: it may fails to retrieve
     * a log string argument.
     */
    bool res = false;
    MemoryRegionSection mrs;

    /*
     * Find the region where the string may reside, using a small size as the
     * length of the string is not known, and memory_region_find would fail if
     * look up is performed behing the end of the containing memory region
     */
    mrs = memory_region_find(eng->as->root, addr, 4u);
    MemoryRegion *mr = mrs.mr;
    if (!mr) {
        xtrace_ot_ibex_wrapper_error(s, "cannot find mr section");
        goto end;
    }

    if (!memory_region_is_ram(mr)) {
        xtrace_ot_ibex_wrapper_error(s, "invalid mr section");
        goto end;
    }

    uintptr_t src = (uintptr_t)memory_region_get_ram_ptr(mr);
    if (!src) {
        xtrace_ot_ibex_wrapper_error(s, "cannot get host mem");
        goto end;
    }
    src += mrs.offset_within_region;

    size_t size = int128_getlo(mrs.size) - mrs.offset_within_region;
    size = MIN(size, 4096u);

    const void *end = memchr((const void *)src, '\0', size);
    if (!end) {
        xtrace_ot_ibex_wrapper_error(s, "cannot compute strlen");
        goto end;
    }
    size_t slen = (uintptr_t)end - (uintptr_t)src;

    char *tstr = g_malloc(slen + 1);
    memcpy(tstr, (const void *)src, slen);
    tstr[slen] = '\0';

    *str = tstr;
    res = true;

end:
    if (mr) {
        memory_region_unref(mr);
    }
    return res;
}

static bool ot_ibex_wrapper_log_load_fields(OtIbexWrapperState *s, hwaddr addr)
{
    OtIbexTestLogEngine *eng = s->log_engine;

    MemoryRegionSection mrs;
    mrs = memory_region_find(eng->as->root, addr, sizeof(eng->fields));

    MemoryRegion *mr = mrs.mr;
    bool res = false;

    if (!mr) {
        xtrace_ot_ibex_wrapper_error(s, "cannot find mr section");
        goto end;
    }

    if (!memory_region_is_ram(mr)) {
        xtrace_ot_ibex_wrapper_error(s, "invalid mr section");
        goto end;
    }

    uintptr_t src = (uintptr_t)memory_region_get_ram_ptr(mr);
    if (!src) {
        xtrace_ot_ibex_wrapper_error(s, "cannot get host mem");
        goto end;
    }
    src += mrs.offset_within_region;

    memcpy(&eng->fields, (const void *)src, sizeof(eng->fields));

    if (eng->fields.file_name_ptr) {
        if (!ot_ibex_wrapper_log_load_string(s,
                                             (uintptr_t)
                                                 eng->fields.file_name_ptr,
                                             &eng->filename)) {
            xtrace_ot_ibex_wrapper_error(s, "cannot get filename");
            goto end;
        }
    }

    if (eng->fields.format_ptr) {
        if (!ot_ibex_wrapper_log_load_string(s,
                                             (uintptr_t)eng->fields.format_ptr,
                                             &eng->format)) {
            xtrace_ot_ibex_wrapper_error(s, "cannot get format string");
            goto end;
        }
    }

    eng->arg_count = 0;
    eng->fmtptr = eng->format;
    if (eng->fields.nargs) {
        eng->args = g_new0(uintptr_t, eng->fields.nargs);
        eng->strargs = g_new0(bool, eng->fields.nargs);
    } else {
        eng->args = NULL;
        eng->strargs = NULL;
    }

    res = true;

end:
    if (mr) {
        memory_region_unref(mr);
    }
    return res;
}

static bool ot_ibex_wrapper_log_load_arg(OtIbexWrapperState *s, uint32_t value)
{
    OtIbexTestLogEngine *eng = s->log_engine;

    if (!eng->fmtptr) {
        xtrace_ot_ibex_wrapper_error(s, "invalid fmtptr");
        return false;
    }

    bool cont;
    do {
        cont = false;
        eng->fmtptr = strchr(eng->fmtptr, '%');
        if (!eng->fmtptr) {
            xtrace_ot_ibex_wrapper_error(s, "cannot find formatter");
            return false;
        }
        eng->fmtptr++;
        switch (*eng->fmtptr) {
        case '%':
            eng->fmtptr++;
            cont = true;
            continue;
        case '\0':
            xtrace_ot_ibex_wrapper_error(s, "cannot find formatter");
            return false;
        case 's':
            if (!ot_ibex_wrapper_log_load_string(s, (hwaddr)value,
                                                 (char **)&eng
                                                     ->args[eng->arg_count])) {
                xtrace_ot_ibex_wrapper_error(s, "cannot load string arg");
                /* use a default string, best effort strategy */
                eng->args[eng->arg_count] = (uintptr_t)&MISSING_LOG_STRING[0];
            } else {
                /* string has been dynamically allocated, and should be freed */
                eng->strargs[eng->arg_count] = true;
            }
            break;
        default:
            eng->args[eng->arg_count] = (uintptr_t)value;
            break;
        }
    } while (cont);

    eng->arg_count++;

    return true;
}

static void ot_ibex_wrapper_log_cleanup(OtIbexWrapperState *s)
{
    OtIbexTestLogEngine *eng = s->log_engine;

    if (eng->strargs && eng->args) {
        for (unsigned ix = 0; ix < eng->fields.nargs; ix++) {
            if (eng->strargs[ix]) {
                if (eng->args[ix]) {
                    g_free((void *)eng->args[ix]);
                }
            }
        }
    }
    g_free(eng->format);
    g_free(eng->filename);
    g_free(eng->strargs);
    g_free(eng->args);
    eng->format = NULL;
    eng->filename = NULL;
    eng->fmtptr = NULL;
    eng->strargs = NULL;
    eng->args = NULL;
}

static void ot_ibex_wrapper_log_emit(OtIbexWrapperState *s)
{
    OtIbexTestLogEngine *eng = s->log_engine;

    const char *level;
    switch (eng->fields.severity) {
    case TEST_LOG_SEVERITY_INFO:
        level = "INFO";
        break;
    case TEST_LOG_SEVERITY_WARN:
        level = "WARN ";
        break;
    case TEST_LOG_SEVERITY_ERROR:
        level = "ERROR ";
        break;
    case TEST_LOG_SEVERITY_FATAL:
        level = "FATAL ";
        break;
    default:
        level = "DEBUG ";
        break;
    }

    /* discard the path of the stored file to reduce log message length */
    const char *basename = eng->filename ? strrchr(eng->filename, '/') : NULL;
    basename = basename ? basename + 1u : eng->filename;

    char *logfmt = g_strdup_printf("%s %s:%d %s\n", level, basename,
                                   eng->fields.line, eng->format);

/* hack ahead: use the uintptr_t array as a va_list */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    char *logmsg = g_strdup_vprintf(logfmt, (char *)eng->args);
#pragma GCC diagnostic pop

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_log_mask(LOG_STRACE, "%s", logmsg);
    } else {
        qemu_chr_fe_write(&s->chr, (const uint8_t *)logmsg,
                          (int)strlen(logmsg));
    }

    g_free(logmsg);
    g_free(logfmt);

    ot_ibex_wrapper_log_cleanup(s);
}

static void ot_ibex_wrapper_status_report(OtIbexWrapperState *s, uint32_t value)
{
    const char *msg;
    switch (value) {
    case TEST_STATUS_IN_BOOT_ROM:
        msg = "IN_BOOT_ROM";
        break;
    case TEST_STATUS_IN_BOOT_ROM_HALT:
        msg = "IN_BOOT_ROM_HALT";
        break;
    case TEST_STATUS_IN_TEST:
        msg = "IN_TEST";
        break;
    case TEST_STATUS_IN_WFI:
        msg = "IN_BOOT_WFI";
        break;
    case TEST_STATUS_PASSED:
        msg = "PASSED";
        break;
    case TEST_STATUS_FAILED:
        msg = "FAILED";
        break;
    default:
        msg = "UNKNOWN";
        break;
    }

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_log_mask(LOG_STRACE, "%s\n", msg);
    } else {
        qemu_chr_fe_write(&s->chr, (const uint8_t *)msg, (int)strlen(msg));
        uint8_t eol[] = { '\n' };
        qemu_chr_fe_write(&s->chr, eol, (int)sizeof(eol));
    }
}

static void ot_ibex_wrapper_log_handle(OtIbexWrapperState *s, uint32_t value)
{
    /*
     * Note about logging:
     *
     * For OT DV logging to work, the "fields" should not be placed in the
     * default linker-discarded sections such as ".logs.fields"
     * i.e. __attribute__((section(".logs.fields"))) should be removed from
     * the "LOG()"" macro.
     */
    OtIbexTestLogEngine *eng = s->log_engine;

    switch (eng->state) {
    case TEST_LOG_STATE_IDLE:
        if (!ot_ibex_wrapper_log_load_fields(s, (hwaddr)value)) {
            eng->state = TEST_LOG_STATE_ERROR;
            ot_ibex_wrapper_log_cleanup(s);
            break;
        }
        if (eng->fields.nargs) {
            eng->state = TEST_LOG_STATE_ARG;
        } else {
            ot_ibex_wrapper_log_emit(s);
            eng->state = TEST_LOG_STATE_IDLE;
        }
        break;
    case TEST_LOG_STATE_ARG:
        if (!ot_ibex_wrapper_log_load_arg(s, value)) {
            ot_ibex_wrapper_log_cleanup(s);
            eng->state = TEST_LOG_STATE_ERROR;
        }
        if (eng->arg_count == eng->fields.nargs) {
            ot_ibex_wrapper_log_emit(s);
            eng->state = TEST_LOG_STATE_IDLE;
        }
        break;
    case TEST_LOG_STATE_ERROR:
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Can no longer handle DV log, in error");
        break;
    }
}

/*
 * vCPU management
 */

static void ot_ibex_wrapper_update_exec(OtIbexWrapperState *s)
{
    /*
     * "Fetch is only enabled when local fetch enable, lifecycle CPU enable and
     *  power manager CPU enable are all enabled."
     */
    bool enable =
        ((s->cpu_en_bm & OT_IBEX_CPU_EN_MASK) == OT_IBEX_CPU_EN_MASK) &&
        !s->esc_rx;

    CPUState *cs = s->cpu;
    g_assert(cs);
    trace_ot_ibex_wrapper_update_exec(s->ot_id ?: "", s->cpu_en_bm, s->esc_rx,
                                      cs->halted, cs->disabled, enable);

    if (enable) {
        cs->halted = 0;
        cs->disabled = false;
        cpu_resume(cs);
    } else {
        cs->disabled = true;
        cpu_pause(cs);
    }
}

static void ot_ibex_wrapper_cpu_enable_recv(void *opaque, int n, int level)
{
    OtIbexWrapperState *s = opaque;

    g_assert((unsigned)n < OT_IBEX_CPU_EN_COUNT);

    bool override = s->lc_ignore && (n == OT_IBEX_LC_CTRL_CPU_EN) && !level;
    if (override) {
        level = 1;
    }

    if (level) {
        s->cpu_en_bm |= 1u << (unsigned)n;
    } else {
        s->cpu_en_bm &= ~(1u << (unsigned)n);
    }

    /*
     * "Fetch is only enabled when local fetch enable, lifecycle CPU enable and
     *  power manager CPU enable are all enabled."
     */
    trace_ot_ibex_wrapper_cpu_enable(s->ot_id ?: "",
                                     n == OT_IBEX_LC_CTRL_CPU_EN ?
                                         (override ? "LC-override" : "LC") :
                                         "PWR",
                                     (bool)level);

    ot_ibex_wrapper_update_exec(s);
}

static void ot_ibex_wrapper_escalate_rx(void *opaque, int n, int level)
{
    OtIbexWrapperState *s = opaque;

    g_assert(n == 0);

    trace_ot_ibex_wrapper_escalate_rx(s->ot_id ?: "", (bool)level);

    s->esc_rx = (bool)level;

    ot_ibex_wrapper_update_exec(s);
}

/*
 * I/O
 */

static uint32_t ot_ibex_wrapper_read_zero(OtIbexWrapperState *s, unsigned reg)
{
    (void)s;
    (void)reg;

    return 0u;
}

static uint32_t ot_ibex_wrapper_read_reg(OtIbexWrapperState *s, unsigned reg)
{
    g_assert(reg < s->reg_count);

    return *s->access_regs[reg];
}

static uint32_t
ot_ibex_wrapper_read_rnd_data(OtIbexWrapperState *s, unsigned reg)
{
    (void)reg;

    if (!ot_ibex_wrapper_has_edn(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: No EDN connection\n", __func__);
        return 0;
    }

    uint32_t value = s->regs.rnd_data;
    if (!(s->regs.rnd_status & RND_STATUS_RND_DATA_VALID_MASK)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Read invalid entropy data 0x%08x\n",
                      __func__, value);
    }
    s->regs.rnd_data = 0;
    s->regs.rnd_status = 0;

    ot_ibex_wrapper_request_entropy(s);

    return value;
}

static uint32_t
ot_ibex_wrapper_read_rnd_status(OtIbexWrapperState *s, unsigned reg)
{
    (void)reg;

    if (!ot_ibex_wrapper_has_edn(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: No EDN connection\n", __func__);
        return 0;
    }

    if (!(s->regs.rnd_status & RND_STATUS_RND_DATA_VALID_MASK)) {
        ot_ibex_wrapper_request_entropy(s);
    }

    return s->regs.rnd_status;
}

static void ot_ibex_wrapper_write_reg(OtIbexWrapperState *s, unsigned reg,
                                      uint32_t value)
{
    *s->access_regs[reg] = value;
}

static void ot_ibex_wrapper_write_ro(OtIbexWrapperState *s, unsigned reg,
                                     uint32_t value)
{
    (void)value;

    qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s is a R/O register\n", __func__,
                  s->ot_id, REG_NAME(s, reg));
}

static void ot_ibex_wrapper_write_alert_test(OtIbexWrapperState *s,
                                             unsigned reg, uint32_t value)
{
    (void)reg;

    s->regs.alert_test = value;
    ot_ibex_wrapper_update_alerts(s);
    s->regs.alert_test = 0u;
    ot_ibex_wrapper_update_alerts(s);
}

static void ot_ibex_wrapper_write_sw_recov_err(OtIbexWrapperState *s,
                                               unsigned reg, uint32_t value)
{
    (void)reg;

    s->regs.sw_recov_err = value;
    ot_ibex_wrapper_update_alerts(s);
    s->regs.sw_recov_err = OT_MULTIBITBOOL4_FALSE;
    ot_ibex_wrapper_update_alerts(s);
}

static void ot_ibex_wrapper_write_sw_fatal_err(OtIbexWrapperState *s,
                                               unsigned reg, uint32_t value)
{
    (void)reg;

    /* QEMU extenson */
    if ((value >> 16u) == 0xC0DEu) {
        /* guest should now use DV_SIM_STATUS register */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: QEMU exit on SW_FATAL_ERR is deprecated",
                      __func__, s->ot_id);
        /* discard MSB magic */
        value &= UINT16_MAX;
        /* discard multibool4false mark */
        value >>= 4u;
        /* std exit code should be in [0..127] range */
        if (value > 127u) {
            value = 127u;
        }
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                               (int)value);
    }

    /* real HW */
    value &= R_SW_FATAL_ERR_VAL_MASK;
    s->regs.sw_fatal_err =
        ot_multibitbool_w1s_write(s->regs.sw_fatal_err, value, 4u);
    ot_ibex_wrapper_update_alerts(s);
}

static void ot_ibex_wrapper_write_regwen(OtIbexWrapperState *s, unsigned reg,
                                         uint32_t value)
{
    g_assert(reg >= FIRST_REMAP_REG_POS);
    reg -= FIRST_REMAP_REG_POS;
    unsigned access = reg / (R32_OFF(sizeof(OtIbexRemap)) * s->num_regions);
    unsigned group = reg % (R32_OFF(sizeof(OtIbexRemap)) * s->num_regions);
    unsigned region = group % s->num_regions;
    g_assert(access < ACCESS_COUNT);
    g_assert(region < s->num_regions);

    s->regs.remap[access][region].regwen &= value; /* RW0C */
}

static void
ot_ibex_wrapper_write_remap(OtIbexWrapperState *s, unsigned reg, uint32_t value)
{
    g_assert(reg >= FIRST_REMAP_REG_POS);
    reg -= FIRST_REMAP_REG_POS;
    unsigned access = reg / (R32_OFF(sizeof(OtIbexRemap)) * s->num_regions);
    unsigned group = reg % (R32_OFF(sizeof(OtIbexRemap)) * s->num_regions);
    unsigned offset = group / s->num_regions;
    unsigned region = group % s->num_regions;
    g_assert(access < ACCESS_COUNT);

    if (offset != offsetof(OtIbexRemap, regwen) &&
        !s->regs.remap[access][region].regwen) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                      __func__, s->ot_id, REG_NAME(s, reg));
        return;
    }

    switch (offset) {
    case R32_OFF(offsetof(OtIbexRemap, addr_en)):
        s->regs.remap[access][region].addr_en = value;
        break;
    case R32_OFF(offsetof(OtIbexRemap, addr_matching)):
        s->regs.remap[access][region].addr_matching = value;
        break;
    case R32_OFF(offsetof(OtIbexRemap, remap_addr)):
        s->regs.remap[access][region].remap_addr = value;
        break;
    case R32_OFF(offsetof(OtIbexRemap, regwen)):
    default:
        g_assert_not_reached();
        return;
    }

    if (s->alias_mode) {
        ot_ibex_wrapper_update_remap_mr(s, (OtIbexRemapAccess)access, region);

    } else {
        ot_ibex_wrapper_update_remap_vmap(s, (OtIbexRemapAccess)access, region);
    }
}

static void ot_ibex_wrapper_write_nmi_enable(OtIbexWrapperState *s,
                                             unsigned reg, uint32_t value)
{
    (void)reg;

    s->regs.nmi_enable |= value; /* RW1S */
}

static void ot_ibex_wrapper_write_nmi_state(OtIbexWrapperState *s, unsigned reg,
                                            uint32_t value)
{
    (void)reg;

    qemu_log_mask(LOG_UNIMP, "%s: %s: %s is not supported\n", __func__,
                  s->ot_id, REG_NAME(s, reg));

    s->regs.nmi_state &= ~value; /* RW1C */
}

static void ot_ibex_wrapper_write_err_status(OtIbexWrapperState *s,
                                             unsigned reg, uint32_t value)
{
    qemu_log_mask(LOG_UNIMP, "%s: %s: %s is not supported\n", __func__,
                  s->ot_id, REG_NAME(s, reg));

    s->regs.err_status &= ~value; /* RW1C */
}

static void ot_ibex_wrapper_write_dv_sim_status(OtIbexWrapperState *s,
                                                unsigned reg, uint32_t value)
{
    (void)reg;

    ot_ibex_wrapper_status_report(s, value);

    if (s->dv_sim_status_exit) {
        switch (value & DV_SIM_STATUS_CODE_MASK) {
        case TEST_STATUS_PASSED:
            trace_ot_ibex_wrapper_exit(s->ot_id, "DV SIM success, exiting", 0);
            qemu_system_shutdown_request_with_code(
                SHUTDOWN_CAUSE_GUEST_SHUTDOWN, 0);
            break;
        case TEST_STATUS_FAILED: {
            uint32_t info = SHARED_FIELD_EX32(value, DV_SIM_STATUS_INFO);
            int ret;
            if (info == 0) {
                /* no extra info */
                ret = 1;
            } else {
                ret = (int)(info & 0x7fu);
            }
            trace_ot_ibex_wrapper_exit(s->ot_id, "DV SIM failure, exiting",
                                       ret);
            qemu_system_shutdown_request_with_code(
                SHUTDOWN_CAUSE_GUEST_SHUTDOWN, ret);
            break;
        }
        default:
            break;
        }
    } else {
        trace_ot_ibex_wrapper_exit(s->ot_id,
                                   "dv-sim-status-exit disabled, not exiting",
                                   0);
    }

    s->regs.dv_sim_win[DV_SIM_STATUS] = value;
}

static void ot_ibex_wrapper_write_dv_sim_log(OtIbexWrapperState *s,
                                             unsigned reg, uint32_t value)
{
    g_assert(reg < s->reg_count);
    reg -= R32_DYN_POS(s, dv_sim_win[0]);
    g_assert(reg < ARRAY_SIZE(((OtIbexWrapperRegs *)0)->dv_sim_win));
    /* NOLINTNEXTLINE */
    switch (reg) {
    case 0u:
        ot_ibex_wrapper_log_handle(s, value);
        break;
    default:
        s->regs.dv_sim_win[reg] = value;
        break;
    }
}

static uint64_t
ot_ibex_wrapper_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtIbexWrapperState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    if (reg >= s->reg_count) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid register 0x%03" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        return 0;
    }

    g_assert(s->access_table[reg].read);
    val32 = (*s->access_table[reg].read)(s, reg);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_ibex_wrapper_io_read_out(s->ot_id, (uint32_t)addr,
                                      REG_NAME(s, reg), val32, pc);

    return (uint64_t)val32;
};

static void ot_ibex_wrapper_regs_write(void *opaque, hwaddr addr,
                                       uint64_t val64, unsigned size)
{
    OtIbexWrapperState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_ibex_wrapper_io_write(s->ot_id, (uint32_t)addr, REG_NAME(s, reg),
                                   val32, pc);

    if (reg >= s->reg_count) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: Invalid register 0x%03" HWADDR_PRIx "\n",
                      __func__, s->ot_id, addr);
        return;
    }

    val32 &= s->access_table[reg].mask;

    g_assert(s->access_table[reg].write);
    (*s->access_table[reg].write)(s, reg, val32);
};

static void ot_ibex_wrapper_fill_tables(OtIbexWrapperState *s)
{
    uint32_t *regs;

    CREATE_NAME_REG(s, ALERT_TEST);
    CREATE_NAME_REG(s, SW_RECOV_ERR);
    CREATE_NAME_REG(s, SW_FATAL_ERR);

    regs = &s->regs.alert_test;
    unsigned rix;
    for (rix = 0; rix <= LAST_STATIC_REG_POS; rix++) {
        s->access_regs[rix] = &regs[rix];
    }

    for (unsigned aix = 0; aix < ACCESS_COUNT; aix++) {
        char prefix[6] = " BUS_";
        prefix[0] = aix ? 'D' : 'I';
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            CREATE_NAME_REG_IX_AT(s, prefix, rix, mix, REGWEN);
            s->access_regs[rix++] = &s->regs.remap[aix][mix].regwen;
        }
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            CREATE_NAME_REG_IX_AT(s, prefix, rix, mix, ADDR_EN);
            s->access_regs[rix++] = &s->regs.remap[aix][mix].addr_en;
        }
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            CREATE_NAME_REG_IX_AT(s, prefix, rix, mix, ADDR_MATCHING);
            s->access_regs[rix++] = &s->regs.remap[aix][mix].addr_matching;
        }
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            CREATE_NAME_REG_IX_AT(s, prefix, rix, mix, REMAP_ADDR);
            s->access_regs[rix++] = &s->regs.remap[aix][mix].remap_addr;
        }
    }
    g_assert(rix == R32_DYN_POS(s, nmi_enable));

    CREATE_NAME_REG_AT(s, R32_DYN_POS(s, nmi_enable), NMI_ENABLE);
    CREATE_NAME_REG_AT(s, R32_DYN_POS(s, nmi_state), NMI_STATE);
    CREATE_NAME_REG_AT(s, R32_DYN_POS(s, err_status), ERR_STATUS);
    CREATE_NAME_REG_AT(s, R32_DYN_POS(s, rnd_data), RND_DATA);
    CREATE_NAME_REG_AT(s, R32_DYN_POS(s, rnd_status), RND_STATUS);
    CREATE_NAME_REG_AT(s, R32_DYN_POS(s, fpga_info), FPGA_INFO);
    CREATE_NAME_REG_AT(s,
                       s->remap_reg_count + R32_POS(dv_sim_win[DV_SIM_STATUS]),
                       DV_SIM_STATUS);
    CREATE_NAME_REG_AT(s, s->remap_reg_count + R32_POS(dv_sim_win[DV_SIM_LOG]),
                       DV_SIM_LOG);

    for (unsigned wix = DV_SIM_WIN2; wix <= DV_SIM_WIN7; wix++) {
        CREATE_NAME_REG_IX_AT(s, "", R32_DYN_POS(s, dv_sim_win[wix]), wix,
                              DV_SIM_WIN);
    }

    regs = &s->regs.nmi_enable;
    while (rix < R32_DYN_POS(s, dv_sim_win[DV_SIM_COUNT])) {
        s->access_regs[rix++] = regs++;
    }

    /* assign readers */
    for (rix = 0; rix < s->reg_count; rix++) {
        s->access_table[rix].read = &ot_ibex_wrapper_read_reg;
    }

    s->access_table[R32_DYN_POS(s, rnd_data)].read =
        &ot_ibex_wrapper_read_rnd_data;
    s->access_table[R32_DYN_POS(s, rnd_status)].read =
        &ot_ibex_wrapper_read_rnd_status;
    s->access_table[R32_DYN_POS(s, dv_sim_win[0u])].read =
        &ot_ibex_wrapper_read_zero;

    /* assign writers */
    for (rix = 0; rix < s->reg_count; rix++) {
        s->access_table[rix].write = &ot_ibex_wrapper_write_reg;
        s->access_table[rix].mask = UINT32_MAX;
    }

    s->access_table[R32_DYN_POS(s, alert_test)].write =
        &ot_ibex_wrapper_write_alert_test;
    s->access_table[R32_DYN_POS(s, sw_recov_err)].write =
        &ot_ibex_wrapper_write_sw_recov_err;
    s->access_table[R32_DYN_POS(s, sw_fatal_err)].write =
        &ot_ibex_wrapper_write_sw_fatal_err;
    s->access_table[R32_DYN_POS(s, err_status)].write =
        &ot_ibex_wrapper_write_err_status;
    s->access_table[R32_DYN_POS(s, nmi_enable)].write =
        &ot_ibex_wrapper_write_nmi_enable;
    s->access_table[R32_DYN_POS(s, nmi_state)].write =
        &ot_ibex_wrapper_write_nmi_state;
    s->access_table[R32_DYN_POS(s, rnd_data)].write = &ot_ibex_wrapper_write_ro;
    s->access_table[R32_DYN_POS(s, rnd_status)].write =
        &ot_ibex_wrapper_write_ro;
    s->access_table[R32_DYN_POS(s, fpga_info)].write =
        &ot_ibex_wrapper_write_ro;
    s->access_table[R32_DYN_POS(s, dv_sim_win[DV_SIM_STATUS])].write =
        &ot_ibex_wrapper_write_dv_sim_status;
    s->access_table[R32_DYN_POS(s, dv_sim_win[DV_SIM_LOG])].write =
        &ot_ibex_wrapper_write_dv_sim_log;

    s->access_table[R32_DYN_POS(s, alert_test)].mask = ALERT_TEST_MASK;
    s->access_table[R32_DYN_POS(s, sw_recov_err)].mask =
        R_SW_RECOV_ERR_VAL_MASK;
    /* this register is extended in QEMU, HW mask is applied in HW handler */
    s->access_table[R32_DYN_POS(s, sw_fatal_err)].mask = UINT32_MAX;
    s->access_table[R32_DYN_POS(s, nmi_enable)].mask = NMI_MASK;
    s->access_table[R32_DYN_POS(s, nmi_state)].mask = NMI_MASK;
    s->access_table[R32_DYN_POS(s, err_status)].mask = ERR_STATUS_MASK;

    unsigned base = FIRST_REMAP_REG_POS;
    for (unsigned aix = 0; aix < ACCESS_COUNT; aix++) {
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            s->access_table[base + mix].write = &ot_ibex_wrapper_write_regwen;
            s->access_table[base + mix].mask = REGWEN_EN_MASK;
        }
        base += s->num_regions;
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            s->access_table[base + mix].write = &ot_ibex_wrapper_write_remap;
            s->access_table[base + mix].mask = UINT32_MAX;
        }
        base += s->num_regions;
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            s->access_table[base + mix].write = &ot_ibex_wrapper_write_remap;
            s->access_table[base + mix].mask = UINT32_MAX;
        }
        base += s->num_regions;
        for (unsigned mix = 0; mix < s->num_regions; mix++) {
            s->access_table[base + mix].write = &ot_ibex_wrapper_write_remap;
            s->access_table[base + mix].mask = UINT32_MAX;
        }
        base += s->num_regions;
    }


#ifdef PRINT_REGNAMES
    for (unsigned ix = 0; ix < s->reg_count; ix++) {
        qemu_log("%s: %s: REG[%2u] = %s\n", __func__, s->ot_id, ix,
                 REG_NAME(s, ix));
    }
#endif
}

/* all properties are optional */
static Property ot_ibex_wrapper_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtIbexWrapperState, ot_id),
    DEFINE_PROP_LINK("edn", OtIbexWrapperState, edn, TYPE_OT_EDN, OtEDNState *),
    DEFINE_PROP_LINK("vmapper", OtIbexWrapperState, vmapper, TYPE_OT_VMAPPER,
                     OtVMapperState *),
    DEFINE_PROP_UINT8("num-regions", OtIbexWrapperState, num_regions, 0),
    DEFINE_PROP_UINT8("edn-ep", OtIbexWrapperState, edn_ep, UINT8_MAX),
    DEFINE_PROP_BOOL("lc-ignore", OtIbexWrapperState, lc_ignore, false),
    DEFINE_PROP_BOOL("alias-mode", OtIbexWrapperState, alias_mode, false),
    DEFINE_PROP_BOOL("dv-sim-status-exit", OtIbexWrapperState,
                     dv_sim_status_exit, true),
    DEFINE_PROP_UINT8("qemu_version", OtIbexWrapperState, qemu_version, 0),
    DEFINE_PROP_STRING("lc-ignore-ids", OtIbexWrapperState, lc_ignore_ids),
    DEFINE_PROP_CHR("logdev", OtIbexWrapperState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_ibex_wrapper_regs_ops = {
    .read = &ot_ibex_wrapper_regs_read,
    .write = &ot_ibex_wrapper_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_ibex_wrapper_reset_enter(Object *obj, ResetType type)
{
    OtIbexWrapperClass *c = OT_IBEX_WRAPPER_GET_CLASS(obj);
    OtIbexWrapperState *s = OT_IBEX_WRAPPER(obj);

    trace_ot_ibex_wrapper_reset(s->ot_id, "enter");

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    if (s->lc_ignore_ids) {
        char *ign = g_strdup(s->lc_ignore_ids);
        char *token = strtok(ign, ",");
        while (token) {
            if (!strcmp(token, s->ot_id)) {
                s->lc_ignore = true;
            }
            token = strtok(NULL, ",");
        }
        g_free(ign);
    }

    if (!s->cpu) {
        CPUState *cpu = ot_common_get_local_cpu(DEVICE(s));
        if (!cpu) {
            error_setg(&error_fatal, "Could not find the associated vCPU");
            g_assert_not_reached();
        }
        s->cpu = cpu;
    }

    for (unsigned slot = 0; slot < (unsigned)s->num_regions; slot++) {
        ot_ibex_wrapper_remapper_destroy(s, slot);
    }

    static_assert(offsetof(OtIbexWrapperRegs, remap) ==
                      sizeof(OtIbexWrapperRegs) -
                          sizeof(((OtIbexWrapperRegs *)0)->remap),
                  "Offset of remap field is incorrect");
    memset(&s->regs, 0, offsetof(OtIbexWrapperRegs, remap));
    s->regs.sw_recov_err = OT_MULTIBITBOOL4_FALSE;
    s->regs.sw_fatal_err = OT_MULTIBITBOOL4_FALSE;
    for (unsigned aix = 0; aix < ACCESS_COUNT; aix++) {
        for (unsigned rix = 0; rix < (unsigned)s->num_regions; rix++) {
            s->regs.remap[aix][rix].regwen = 0x1u;
        }
    }

    /* 'QMU_' in LE, _ is the QEMU version stored in the MSB */
    s->regs.fpga_info = 0x00554d51u + (((uint32_t)s->qemu_version) << 24u);
    s->entropy_requested = false;
    s->cpu_en_bm = s->lc_ignore ? (1u << OT_IBEX_LC_CTRL_CPU_EN) : 0;

    memset(s->log_engine, 0, sizeof(*s->log_engine));
}

static void ot_ibex_wrapper_reset_exit(Object *obj, ResetType type)
{
    OtIbexWrapperClass *c = OT_IBEX_WRAPPER_GET_CLASS(obj);
    OtIbexWrapperState *s = OT_IBEX_WRAPPER(obj);

    trace_ot_ibex_wrapper_reset(s->ot_id, "exit");

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    s->log_engine->as = ot_common_get_local_address_space(DEVICE(s));

    /* "Upon reset the data will be invalid with a new EDN request pending." */
    ot_ibex_wrapper_request_entropy(s);
}

static void ot_ibex_wrapper_realize(DeviceState *dev, Error **errp)
{
    OtIbexWrapperState *s = OT_IBEX_WRAPPER(dev);
    (void)errp;

    s->sys_mem = ot_common_get_local_address_space(dev)->root;
    g_assert(s->sys_mem);
    g_assert(s->ot_id);
    g_assert(s->num_regions);
    /* if EDN mode is enabled, EDN endpoint must be set */
    g_assert(!s->edn || s->edn_ep != UINT8_MAX);
    /* if legacy alias_mode is disabled, vmapper must be set */
    g_assert(!s->alias_mode || s->vmapper);

    s->remap_reg_count =
        s->num_regions * ACCESS_COUNT * sizeof(OtIbexRemap) / sizeof(uint32_t);
    s->reg_count = offsetof(OtIbexWrapperRegs, dv_sim_win) / sizeof(uint32_t) +
                   DV_SIM_COUNT + s->remap_reg_count;

    memory_region_init_io(&s->mmio, OBJECT(dev), &ot_ibex_wrapper_regs_ops, s,
                          TYPE_OT_IBEX_WRAPPER,
                          s->reg_count * sizeof(uint32_t));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->remappers = g_new0(MemoryRegion, s->num_regions);
    s->access_table = g_new0(OtIbexWrapperAccess, s->reg_count);
    s->access_regs = g_new0(uint32_t *, s->reg_count);
    s->reg_names = g_new0(char *, s->reg_count);
    for (unsigned ix = 0; ix < ACCESS_COUNT; ix++) {
        s->regs.remap[ix] = g_new0(OtIbexRemap, s->num_regions);
    }
    char *name_buf = g_new0(char, (size_t)(REG_NAME_LENGTH * s->reg_count));
    for (unsigned ix = 0; ix < s->reg_count;
         ix++, name_buf += REG_NAME_LENGTH) {
        s->reg_names[ix] = name_buf;
    }

    ot_ibex_wrapper_fill_tables(s);
}

static void ot_ibex_wrapper_init(Object *obj)
{
    OtIbexWrapperState *s = OT_IBEX_WRAPPER(obj);

    for (unsigned ix = 0; ix < NUM_ALERTS; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }

    qdev_init_gpio_in_named(DEVICE(obj), &ot_ibex_wrapper_cpu_enable_recv,
                            OT_IBEX_WRAPPER_CPU_EN, OT_IBEX_CPU_EN_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_ibex_wrapper_escalate_rx,
                            OT_ALERT_ESCALATE, 1);

    s->log_engine = g_new0(OtIbexTestLogEngine, 1u);
}

static void ot_ibex_wrapper_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_ibex_wrapper_realize;
    device_class_set_props(dc, ot_ibex_wrapper_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtIbexWrapperClass *ic = OT_IBEX_WRAPPER_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_ibex_wrapper_reset_enter, NULL,
                                       &ot_ibex_wrapper_reset_exit,
                                       &ic->parent_phases);
}

static const TypeInfo ot_ibex_wrapper_info = {
    .name = TYPE_OT_IBEX_WRAPPER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtIbexWrapperState),
    .instance_init = &ot_ibex_wrapper_init,
    .class_size = sizeof(OtIbexWrapperClass),
    .class_init = &ot_ibex_wrapper_class_init,
};

static void ot_ibex_wrapper_register_types(void)
{
    type_register_static(&ot_ibex_wrapper_info);
}

type_init(ot_ibex_wrapper_register_types);
