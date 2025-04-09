/*
 * QEMU OpenTitan virtual mapper device
 *
 * Copyright (c) 2025 Rivos, Inc.
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
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "exec/exec-all.h"
#include "exec/page-protection.h"
#include "hw/boards.h"
#include "hw/opentitan/ot_vmapper.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/ibex_common.h"
#include "trace.h"

/* define to log range definitions */
#undef SHOW_RANGE_LIST
#undef SHOW_RANGE_TREE
/* define to log address mapping (highly verbose) */
#undef SHOW_ADDRESS_MAPPING

/* Translatable region */
typedef struct {
    uint32_t start; /* first address of the region to be translated */
    uint32_t end; /* last address of the region to be translated */
    uint32_t dest; /* first address of the destination */
    uint8_t prio; /* priority of the slot, 0 = highest priority */
    bool active; /* whether this range is active or should be ignored */
    bool execute; /* whether this range is executable (for insn range)*/
} OtRegionRange;

struct OtVMapperState {
    DeviceState parent_obj;

    OtRegionRange *dranges; /* Configured data ranges */
    OtRegionRange *iranges; /* Configured instruction ranges */

    OtRegionRange *lranges[2u]; /* Last matched data and instruction ranges */

    CPUState *cpu;
    GTree *dtree; /* Active data ranges */
    GTree *itree; /* Active instruction ranges */
    bool insert_mode; /* Whether trees are used in insertion or matching mode */
    bool show; /* debug flag to enable list and tree dump */
    bool silent_align; /* whether to silence alignment warnings */

    char *ot_id;
    uint8_t cpu_idx; /* cpu index, i.e. which vCPU is translated */
    uint8_t trans_count; /* count of translatable regions */
};

#define VMAP_RANGE(_glist_) ((OtRegionRange *)((_glist_)->data))
#define VMAP_PRIOR(_ra_, _rb_) \
    ((VMAP_RANGE(_ra_)->pos < VMAP_RANGE(_rb_)->pos) ? VMAP_RANGE(_ra_) : \
                                                       VMAP_RANGE(_rb_))
#define VMAP_RANGE_TO_TREE_KEY(_s_, _e_) \
    ((gpointer)(((uintptr_t)(_s_)) | ((uintptr_t)(_e_) << 32u)))
#define VMAP_REGION_TO_TREE_KEY(_r_) \
    VMAP_RANGE_TO_TREE_KEY((_r_)->start, (_r_)->end)
#define VMAP_TREE_KEY_TO_RANGE_START(_g_) \
    ((uint32_t)(((uintptr_t)(_g_)) & UINT32_MAX))
#define VMAP_TREE_KEY_TO_RANGE_END(_g_) ((uint32_t)(((uintptr_t)(_g_)) >> 32u))

/* 'g_tree_remove_all' is deprecated: Not available before 2.70 */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

/*
 * order ranges for g_list_sort
 *  1. by start address first,
 *  2. then by end address,
 *  3. then by priority (from higest to lowest)
 */
static gint ot_vmapper_compare(gconstpointer a, gconstpointer b)
{
    const OtRegionRange *ra = (const OtRegionRange *)a;
    const OtRegionRange *rb = (const OtRegionRange *)b;

    return (gint)((ra->start == rb->start) ?
                      ((ra->end == rb->end) ? (ra->prio - rb->prio) :
                                              (ra->end - rb->end)) :
                      (ra->start - rb->start));
}

/*
 * order range by increasing addresses, for g_tree
 * - in insert mode, return a strict range comparison
 * - in match mode, return equality for any A address in B range
 */
static gint ot_vmapper_compare_address(gconstpointer a, gconstpointer b,
                                       gpointer user_data)
{
    gint ret;
    bool insert_mode = *(const bool *)user_data;

    uint32_t as = VMAP_TREE_KEY_TO_RANGE_START(a);
    uint32_t bs = VMAP_TREE_KEY_TO_RANGE_START(b);

    if (!insert_mode) {
        uint32_t ae = VMAP_TREE_KEY_TO_RANGE_END(a);
        uint32_t be = VMAP_TREE_KEY_TO_RANGE_END(b);
        /* in match mode, any A start address within B range matches */
        g_assert(ae == 0);
        if (as < bs) {
            ret = -1;
        } else if (as > be) {
            ret = 1;
        } else {
            ret = 0;
        }
    } else {
        /* in insertion mode, compare the start address */
        ret = as == bs ? 0 : ((as < bs) ? -1 : 1);
    }

    return ret;
}

#ifdef SHOW_RANGE_LIST
#define VMAP_SHOW_RANGE_LIST(_s_, _l_, _m_) \
    ot_vmapper_show_range_list(_s_, _l_, _m_)

static void ot_vmapper_show_range_list(const OtVMapperState *s,
                                       const GList *rglist, const char *msg)
{
    if (!s->show) {
        return;
    }
    qemu_log("%s: %s %s\n", __func__, s->ot_id, msg);
    const GList *current = rglist;
    unsigned pos = 0;
    while (current) {
        const OtRegionRange *rg = VMAP_RANGE(current);
        qemu_log(" * %2u: [%2u] 0x%08x..0x%08x -> 0x%08x X:%u\n", pos, rg->prio,
                 rg->start, rg->end, rg->dest, rg->execute);
        current = current->next;
        pos++;
    }
    qemu_log("%s: %s %u items\n\n", __func__, s->ot_id, pos);
}
#else
#define VMAP_SHOW_RANGE_LIST(_s_, _l_, _m_)
#endif

static GTree *ot_vmapper_create_tree(OtVMapperState *s)
{
    return g_tree_new_full(&ot_vmapper_compare_address, &s->insert_mode, NULL,
                           &g_free);
}

/*
 * there should be no need for such workarounds, however some prehistoric
 * OSes (CentOS 7, ...) rely on outdated Glib versions that lack useful
 * functions. Some other old OSes (Ubuntu 20.x) also lack new functions...
 */
#if (GLIB_MAJOR_VERSION == 2)

/* 'g_tree_remove_all' is deprecated: Not available before 2.70 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#if (GLIB_MINOR_VERSION >= 68) && (GLIB_MINOR_VERSION < 70)

/*
 * g_tree_remove_all is only available from 2.70
 * re-implement it
 */
static void g_tree_remove_all(GTree *tree)
{
    GTreeNode *node;
    GTreeNode *next;
    g_return_if_fail(tree != NULL);
    node = g_tree_node_first(tree);
    while (node) {
        next = g_tree_node_next(node);
        if (tree->key_destroy_func) {
            tree->key_destroy_func(node->key);
        }
        if (tree->value_destroy_func) {
            tree->value_destroy_func(node->value);
        }
        g_slice_free(GTreeNode, node);
        node = next;
    }
    tree->root = NULL;
}
#endif /* >= 2.68 < 2.70 */

static void ot_vmapper_flush_tree(OtVMapperState *s, GTree *tree)
{
#if (GLIB_MINOR_VERSION < 68)
#ifdef SHOW_RANGE_TREE
/* non essential feature, please upgrade Glib */
#error SHOW_RANGE_TREE not supported with current Glib version
#endif /* SHOW_RANGE_TREE */
    /*
     * GTreeNode is only available from 2.68
     * destroy the whole tree and build a new one
     * this is ulgy, but should not be used except on outdated hosts
     */
    if (tree == s->itree) {
        g_tree_destroy(s->itree);
        s->itree = ot_vmapper_create_tree(s);
    } else if (tree == s->dtree) {
        g_tree_destroy(s->dtree);
        s->dtree = ot_vmapper_create_tree(s);
    } else {
        g_assert_not_reached();
    }
#else /* >= 2.68 */
    (void)s;
    g_tree_remove_all(tree);
#endif /* >= 2.68 */
}

/* "-Wdeprecated-declarations" */
#pragma GCC diagnostic pop

#endif /* 2.x */

#ifdef SHOW_RANGE_TREE
#define VMAP_SHOW_RANGE_TREE(_s_, _i_) ot_vmapper_show_range_tree(_s_, _i_)

static gboolean ot_vmapper_show_node(GTreeNode *node, gpointer data)
{
    const OtRegionRange *rg = (const OtRegionRange *)g_tree_node_value(node);
    unsigned *count = (unsigned *)data;
    qemu_log(" * %2u: 0x%08x..0x%08x -> 0x%08x X:%u\n", *count, rg->start,
             rg->end, rg->dest, rg->execute);
    *count += 1;
    return FALSE;
}

static void ot_vmapper_show_range_tree(const OtVMapperState *s, bool insn)
{
    if (!s->show) {
        return;
    }
    qemu_log("%s: %s %s\n", __func__, s->ot_id, insn ? "insn" : "data");
    unsigned count = 0;
    g_tree_foreach_node(insn ? s->itree : s->dtree, &ot_vmapper_show_node,
                        &count);
    qemu_log("%s: %s %u items\n\n", __func__, s->ot_id, count);
}
#else
#define VMAP_SHOW_RANGE_TREE(_s_, _i_)
#endif

/* class singleton */
static OtVMapperClass *ot_vmapper_class;

static int ot_vmapper_get_phy_addr(OtVMapperState *s, hwaddr *physical,
                                   int *ret_prot, vaddr addr, int access_type)
{
    bool insn = access_type == MMU_INST_FETCH;

    OtRegionRange *range = s->lranges[insn];

#ifdef SHOW_ADDRESS_MAPPING
    bool trace;
#endif
    if (!range || (((uint32_t)addr) < range->start) ||
        (((uint32_t)addr) > range->end)) {
        GTree *tree = insn ? s->itree : s->dtree;
        range = g_tree_lookup(tree, VMAP_RANGE_TO_TREE_KEY(addr, 0));
        s->lranges[insn] = range;
#ifdef SHOW_ADDRESS_MAPPING
        trace = true;
#endif
    } else {
#ifdef SHOW_ADDRESS_MAPPING
        trace = false;
#endif
    }

    if G_UNLIKELY (!range || (addr > range->end)) {
        return TRANSLATE_FAIL;
    }

    hwaddr offset = addr - (hwaddr)range->start;
    *physical = range->dest + offset;
    *ret_prot = PAGE_READ | PAGE_WRITE | (range->execute ? PAGE_EXEC : 0);

#ifdef SHOW_ADDRESS_MAPPING
    if (trace) {
        trace_ot_vmapper_new_phy_addr(s->ot_id, insn, (uint32_t)addr,
                                      *physical);
    }
#endif

    return TRANSLATE_SUCCESS;
}

/* NOLINTBEGIN(readability-non-const-parameter) */
static int ot_riscv_get_physical_address(
    CPURISCVState *env, hwaddr *physical, int *ret_prot, vaddr addr,
    target_ulong *fault_pte_addr, int access_type, int mmu_idx,
    bool first_stage, bool two_stage, bool is_debug, bool is_probe)
/* NOLINTEND(readability-non-const-parameter) */
{
    (void)fault_pte_addr;
    (void)mmu_idx;
    (void)first_stage;
    (void)two_stage;
    (void)is_debug;
    (void)is_probe;

    CPUState *cpu = env_cpu(env);
    g_assert(cpu->cpu_index < ot_vmapper_class->num_instances);

    OtVMapperState *s = ot_vmapper_class->instances[cpu->cpu_index];

    return ot_vmapper_get_phy_addr(s, physical, ret_prot, addr, access_type);
}

static GList *ot_vmapper_range_discretize(OtVMapperState *s, GList *rglist)
{
    (void)s;

    /*
     * replace overlapping ranges with additional, non-overlapping ones,
     * the input list should be sorted by address, and only contain active
     * regions.
     */
    GList *current = rglist;
    while (current) {
        GList *next = current->next;

        if (!next) {
            break;
        }

        /*
         * Note about region priority:
         * "If a transaction matches multiple regions, the lowest indexed region
         *  has priority."
         * Here, region with the lower position has higher priority.
         */

        /* are two consecutive ranges overlapping? */
        if (VMAP_RANGE(next)->start < VMAP_RANGE(current)->end) {
            /*
             * next item starts before current ends: do split, two cases:
             *   +-----------------+       +---------------+
             *   |     current     |       |    current    |
             *   +-----+-------+---+       +-----+---------+----+
             *         | next  |     (a)         |     next     | (b)
             *         +-------+                 +--------------+
             */
            if (VMAP_RANGE(next)->end <= VMAP_RANGE(current)->end) {
                /*
                 * case (a): next is fully overlapped by current, two cases:
                 *
                 * +-----------------+        +-----------------+
                 * |     current     |        | cur |  ...  | rh|
                 * +-----+-------+---+        +-----+-------+---+
                 *       |  ...  |     (a1)         | next  |     (a2)
                 *       +-------+                  +-------+
                 */
                if (VMAP_RANGE(current)->prio < VMAP_RANGE(next)->prio) {
                    /*
                     * case (a1): next is fully masked by current, as next as
                     * a less priority than current, skip next entirely
                     */
                    current = g_list_remove_link(current, next);
                    g_list_free_1(next);
                } else {
                    /*
                     * case (a2): central part of current is masked by next
                     * 1. create a new region for the right part of current
                     * 2. insert the new region after next
                     * 3. update current's end position
                     */
                    OtRegionRange *right = g_new0(OtRegionRange, 1);
                    right->start = VMAP_RANGE(next)->end + 1u;
                    right->end = VMAP_RANGE(current)->end;
                    right->dest =
                        right->dest + right->start - VMAP_RANGE(current)->start;
                    right->prio = VMAP_RANGE(current)->prio;
                    right->execute = VMAP_RANGE(current)->execute;
                    right->active = true;
                    /* assignation is useless, but GCC won't let it go */
                    next = g_list_insert(next, right, 1u);
                    /* trim current on next */
                    VMAP_RANGE(current)->end = VMAP_RANGE(next)->start - 1u;
                }
            } else {
                /* case (b): next extends after current, two cases:
                 *
                 * +-------------+            +-------------+
                 * |   current   |            | cur | ...   |
                 * +-----+-------+----+       +-----+-------+---+
                 *       |  ...  |next| (b1)        |    next   |  (b2)
                 *       +-------+----+             +-----------+
                 */
                if (VMAP_RANGE(current)->prio < VMAP_RANGE(next)->prio) {
                    /* case (b1): next is partially masked by current, trim it*/
                    VMAP_RANGE(next)->dest +=
                        VMAP_RANGE(current)->end - VMAP_RANGE(next)->start;
                    VMAP_RANGE(next)->start = VMAP_RANGE(current)->end + 1u;
                } else {
                    /* case (b2): current is partially masked by next, trim it*/
                    VMAP_RANGE(current)->end = VMAP_RANGE(next)->start - 1u;
                }
            }
        }

        /*
         * be sure to take the immediate next element of current, which may be
         * the left item if one has been inserted
         */
        current = current->next;
    }

    return rglist;
}

static GList *
ot_vmapper_fill_empty_gaps(OtVMapperState *s, GList *rglist, bool insn)
{
    /*
     * The full address range needs to be defined. Fill all gaps between defined
     * ranges with no access items. The input list should be sorted by address,
     * and only contain active regions.
     */
    GList *current = rglist;
    uint32_t addr = 0;

    while (current) {
        if (addr < VMAP_RANGE(current)->start) {
            OtRegionRange *gap = g_new0(OtRegionRange, 1);
            gap->start = addr;
            gap->end = VMAP_RANGE(current)->start - 1u;
            gap->dest = gap->start;
            /* lowest priority */
            gap->prio = s->trans_count;
            gap->execute = insn;
            gap->active = true;
            rglist = g_list_insert_before(rglist, current, gap);
        }
        addr = VMAP_RANGE(current)->end + 1u;
        current = current->next;
    }

    /*
     * if the list is empty or if the last item of the list does not end on the
     * last valid address, append an all access item.
     */
    uint32_t end;
    if (rglist) {
        current = g_list_last(rglist);
        end = VMAP_RANGE(current)->end;
    } else {
        current = NULL;
        end = 0;
    }

    if (end != UINT32_MAX) {
        OtRegionRange *last = g_new0(OtRegionRange, 1);
        last->start = end ? end + 1u : 0;
        last->end = UINT32_MAX;
        last->dest = last->start; /* 1:1 mapping */
        last->prio = s->trans_count;
        last->execute = insn;
        last->active = true;
        if (current) {
            /* assignation is useless, but GCC won't let it go ... */
            current = g_list_insert(current, last, 1u);
            /* ... while clang-tidy found this stupid */
            (void)current;
        } else {
            rglist = g_list_insert(NULL, last, 0u);
        }
    }

    /* from here, its is guaranteed that rglist contains at least one item */

    return rglist;
}

static GList *ot_vmapper_fuse(OtVMapperState *s, GList *rglist)
{
    /*
     * simplify the list whenever possible; if contiguous items share the same
     * access permissions, fuse them to reduce the number of memory regions to
     * create; the input list should be sorted by address, and only contain
     * active regions.
     */
    GList *current = rglist;
    (void)s;

    while (current && current->next) {
        GList *next = current->next;
        /* should always be valid, use this opportunity to validate the list */
        g_assert(VMAP_RANGE(current)->end + 1u == VMAP_RANGE(next)->start);
        if ((VMAP_RANGE(current)->execute == VMAP_RANGE(next)->execute) &&
            ((VMAP_RANGE(next)->start - VMAP_RANGE(next)->dest) ==
             (VMAP_RANGE(current)->start - VMAP_RANGE(current)->dest))) {
            VMAP_RANGE(current)->end = VMAP_RANGE(next)->end;

            rglist = g_list_remove_link(rglist, next);
            g_free(next->data); // OtRegionRange
            g_list_free(next);

            /*
             * do not change the current item if a fusion occured, since the
             * current new adjacent 'next' may also be a fusion candidate.
             */
        } else {
            current = current->next;
        }
    }

    return rglist;
}

static void ot_vmapper_rebuild_tree(OtVMapperState *s, GTree *tree,
                                    GList *rglist)
{
    const GList *current = rglist;

    /* empty the tree AND free any contained OtRegionRange items */
    ot_vmapper_flush_tree(s, tree);

    /* configure the tree comparison for insertion */
    s->insert_mode = true;

    while (current) {
        OtRegionRange *range = VMAP_RANGE(current);
        g_assert(range->active);

        /* transfer ownership of the OtRegionRange item from list to the tree */
        g_tree_insert(tree, VMAP_REGION_TO_TREE_KEY(range), range);

        current = current->next;
    }

    /* configure the tree comparison for matching range */
    s->insert_mode = false;

    /*
     * free the list but not its OtRegionRange items which are now owned by the
     * tree
     */
    g_list_free(rglist);
}

static void ot_vmapper_update(OtVMapperState *s, bool insn)
{
    GList *rglist = NULL;
    GTree *rgtree = insn ? s->itree : s->dtree;
    OtRegionRange *ranges = insn ? s->iranges : s->dranges;

    /* create sortable range items and add them to a new list */
    for (unsigned ix = 0; ix < s->trans_count; ix++) {
        const OtRegionRange *crg = &ranges[ix];

        /* ignore disabled range entries */
        if (!crg->active) {
            continue;
        }

        /* duplicate range since it may be modified */
        OtRegionRange *range = g_new0(OtRegionRange, 1);
        memcpy(range, crg, sizeof(OtRegionRange));

        rglist = g_list_prepend(rglist, range);
    }

    if (rglist) {
        VMAP_SHOW_RANGE_LIST(s, rglist, "initial");

        /* sort the list, in start address order (end address if start are
         * equal) */
        rglist = g_list_sort(rglist, &ot_vmapper_compare);

        VMAP_SHOW_RANGE_LIST(s, rglist, "sorted");

        rglist = ot_vmapper_range_discretize(s, rglist);

        VMAP_SHOW_RANGE_LIST(s, rglist, "discretized");

        /* now rglist contains a list of unique range permissions */

        /* fill all empty gaps with denied ranges */
        rglist = ot_vmapper_fill_empty_gaps(s, rglist, insn);

        VMAP_SHOW_RANGE_LIST(s, rglist, "extended");

        /* combine adjacent items sharing the same properties */
        rglist = ot_vmapper_fuse(s, rglist);

        VMAP_SHOW_RANGE_LIST(s, rglist, "fused");
    } else {
        /* create a one item list with no access for the whole address range */
        rglist = ot_vmapper_fill_empty_gaps(s, rglist, insn);

        VMAP_SHOW_RANGE_LIST(s, rglist, "default");
    }

    /* rglist is freed on return */
    ot_vmapper_rebuild_tree(s, rgtree, rglist);

    s->lranges[insn] = NULL;

    VMAP_SHOW_RANGE_TREE(s, true);
    VMAP_SHOW_RANGE_TREE(s, false);
}

static void ot_vmapper_translate(OtVMapperState *s, bool insn, unsigned slot,
                                 hwaddr src, hwaddr dst, size_t size)
{
    g_assert(slot < s->trans_count);

    /*
     * QEMU virtual address implementation is built around the size of a small
     * MMU page, usually 4KiB. Any attempt to use non-aligned address or size
     * makes QEMU fall back on a 1-byte page size, which leads to terrible
     * performance issues (this usually translates into up to 3 lookups per
     * instruction fetch or data access). This mode is not even supported here
     * as it makes QEMU unusable.
     *
     * Virtual remapper should not be used when non-aligned remapping are
     * expected. See ot_ibex_wrapper for more details.
     */
    struct {
        const char *name;
        uint32_t value;
    } checks[] = {
        { "source", (uint32_t)src },
        { "dest", (uint32_t)dst },
        { "size", (uint32_t)size },
    };
    for (unsigned cix = 0; cix < ARRAY_SIZE(checks); cix++) {
        if (!s->silent_align && (checks[cix].value & (TARGET_PAGE_SIZE - 1u))) {
            error_report("%s: %s %s %s (0x%08x) is not aligned on 4KiB, "
                         "translation may fail",
                         __func__, s->ot_id, insn ? "insn" : "data",
                         checks[cix].name, checks[cix].value);
            s->silent_align = true;
        }
    }

    OtRegionRange *ranges = insn ? s->iranges : s->dranges;
    OtRegionRange *range = &ranges[slot];

    bool activated = range->active;
    range->start = src;
    range->end = size ? src + size - 1u : src;
    range->dest = dst;
    range->execute = insn;
    range->active = size != 0;

    if (activated != range->active) {
        if (size) {
            trace_ot_vmapper_translate_enable(s->ot_id, insn, slot, src, dst,
                                              size);
        } else {
            trace_ot_vmapper_translate_disable(s->ot_id, insn, slot);
        }
        s->show = true;
    }

    ot_vmapper_update(s, insn);
    s->show = false;

    tlb_flush_all_cpus_synced(s->cpu);
}

static CPUState *ot_vmapper_retrieve_cpu(OtVMapperState *s)
{
    DeviceState *cs =
        ibex_get_child_device(DEVICE(OBJECT(s)->parent), TYPE_RISCV_CPU, 0);
    return cs ? CPU(cs) : NULL;
}

static void ot_vmapper_override_vcpu_config(OtVMapperState *s)
{
    g_assert(s->cpu);

    RISCVCPUClass *cc = RISCV_CPU_GET_CLASS(s->cpu);
    RISCVCPU *cpu = RISCV_CPU(s->cpu);

    trace_ot_vmapper_override_vcpu_config(s->ot_id);

    /* reroute riscv_get_physical_address to custom implementation */
    cc->riscv_get_physical_address = &ot_riscv_get_physical_address;
    /* force PMP to work with remapped address */
    cpu->env.vaddr_pmp = true;
}

static Property ot_vmapper_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtVMapperState, ot_id),
    DEFINE_PROP_UINT8("cpu_index", OtVMapperState, cpu_idx, UINT8_MAX),
    DEFINE_PROP_UINT8("trans_count", OtVMapperState, trans_count, UINT8_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_vmapper_reset_enter(Object *obj, ResetType type)
{
    OtVMapperClass *c = OT_VMAPPER_GET_CLASS(obj);
    OtVMapperState *s = OT_VMAPPER(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    memset(s->dranges, 0, sizeof(OtRegionRange) * s->trans_count);
    memset(s->iranges, 0, sizeof(OtRegionRange) * s->trans_count);
    memset(s->lranges, 0, sizeof(s->lranges));

    for (unsigned ix = 0; ix < s->trans_count; ix++) {
        s->dranges[ix].prio = ix;
        s->iranges[ix].prio = ix;
    }

    s->insert_mode = false;
    s->show = false;
    s->silent_align = false;

    ot_vmapper_flush_tree(s, s->dtree);
    ot_vmapper_flush_tree(s, s->itree);

    s->cpu = ot_vmapper_retrieve_cpu(s);
}

static void ot_vmapper_reset_exit(Object *obj, ResetType type)
{
    OtVMapperClass *c = OT_VMAPPER_GET_CLASS(obj);
    OtVMapperState *s = OT_VMAPPER(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    ot_vmapper_override_vcpu_config(s);

    ot_vmapper_update(s, false);
    ot_vmapper_update(s, true);
}

static void ot_vmapper_realize(DeviceState *dev, Error **errp)
{
    OtVMapperState *s = OT_VMAPPER(dev);
    OtVMapperClass *c = OT_VMAPPER_GET_CLASS(dev);
    (void)errp;

    if (s->cpu_idx == UINT8_MAX) {
        CPUState *cs = ot_vmapper_retrieve_cpu(s);
        if (cs) {
            s->cpu_idx = cs->cpu_index;
        }
    }

    /*
     * if the CPU cannot be retrieved from our parent, it needs to be configured
     * from a property
     */
    g_assert(s->cpu_idx != UINT8_MAX);

    c->instances[s->cpu_idx] = s;

    s->dranges = g_new0(OtRegionRange, s->trans_count);
    s->iranges = g_new0(OtRegionRange, s->trans_count);
}

static void ot_vmapper_init(Object *obj)
{
    OtVMapperState *s = OT_VMAPPER(obj);

    if (!ot_vmapper_class->instances) {
        OtVMapperClass *vc = OT_VMAPPER_GET_CLASS(obj);
        vc->num_instances = MACHINE(qdev_get_machine())->smp.cpus;
        vc->instances = g_new0(OtVMapperState *, vc->num_instances);
    }

    /*
     * Binary trees where key is a translation region start address and value
     * is an active translation region (OtRegionRange).
     */
    s->dtree = ot_vmapper_create_tree(s);
    s->itree = ot_vmapper_create_tree(s);
}

static void ot_vmapper_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_vmapper_realize;
    device_class_set_props(dc, ot_vmapper_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtVMapperClass *vc = OT_VMAPPER_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_vmapper_reset_enter, NULL,
                                       &ot_vmapper_reset_exit,
                                       &vc->parent_phases);

    vc->translate = &ot_vmapper_translate;

    ot_vmapper_class = vc;
}

static const TypeInfo ot_vmapper_info = {
    .name = TYPE_OT_VMAPPER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtVMapperState),
    .instance_init = &ot_vmapper_init,
    .class_size = sizeof(OtVMapperClass),
    .class_init = &ot_vmapper_class_init,
};

static void ot_vmapper_register_types(void)
{
    type_register_static(&ot_vmapper_info);
}

type_init(ot_vmapper_register_types);
