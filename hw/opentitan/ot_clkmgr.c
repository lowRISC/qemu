/*
 * QEMU OpenTitan Clock manager device
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
 * Copyright (c) 2025 lowRISC contributors.
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
 * For now, only clock hinting for transactional blocks is actually implemented.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_common.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_clock_src.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

/* clang-format off */
REG32(ALERT_TEST, 0x0u)
    FIELD(ALERT_TEST, RECOV_FAULT, 0u, 1u)
    FIELD(ALERT_TEST, FATAL_FAULT, 1u, 1u)
REG32(EXTCLK_CTRL_REGWEN, 0x4u)
    FIELD(EXTCLK_CTRL, REGWEN_EN, 0u, 1u)
REG32(EXTCLK_CTRL, 0x8u)
    FIELD(EXTCLK_CTRL, SEL, 0u, 4u)
    FIELD(EXTCLK_CTRL, HI_SPEED_SEL, 4u, 4u)
REG32(EXTCLK_STATUS, 0xcu)
    FIELD(EXTCLK_STATUS, ACK, 0u, 4u)
REG32(JITTER_REGWEN, 0x10u)
    FIELD(JITTER_REGWEN, EN, 0u, 1u)
REG32(JITTER_ENABLE, 0x14u)
    FIELD(JITTER_ENABLE, VAL, 0, 4u)
REG32(CLK_ENABLES, 0x18u)
    /* seems field order is randomized */
    FIELD(CLK_ENABLES, CLK_IO_DIV4_PERI_EN, 0u, 1u)
    FIELD(CLK_ENABLES, CLK_IO_DIV2_PERI_EN, 1u, 1u)
    FIELD(CLK_ENABLES, CLK_IO_PERI_EN, 2u, 1u)
    FIELD(CLK_ENABLES, CLK_USB_PERI_EN, 3u, 1u)
REG32(CLK_HINTS, 0x1cu)
REG32(CLK_HINTS_STATUS, 0x20u)
REG32(MEASURE_CTRL_REGWEN, 0x24u)
    FIELD(MEASURE_CTRL_REGWEN, EN, 0u, 1u)
SHARED_FIELD(MEAS_CTRL_EN, 0u, 4u)
SHARED_FIELD(MEAS_CTRL_SHADOWED_HI, 0u, 10u)
SHARED_FIELD(MEAS_CTRL_SHADOWED_LO, 10u, 10u)
/* not the real address, they are offset by (2 * measure_count) * 4 */
REG32(RECOV_ERR_CODE, 0x28u)
    FIELD(RECOV_ERR_CODE, SHADOW_UPDATE_ERR, 0u, 1u)
REG32(FATAL_ERR_CODE, 0x2cu)
    FIELD(FATAL_ERR_CODE, REG_INTG, 0u, 1u)
    FIELD(FATAL_ERR_CODE, IDLE_CNT, 1u, 1u)
    FIELD(FATAL_ERR_CODE, SHADOW_STORAGE_ERR, 2u, 1u)
/* clang-format on */

REG32(MEASURE_REG_BASE, A_RECOV_ERR_CODE)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

/* last statically defined register */
#define R_LAST_REG (R_FATAL_ERR_CODE)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) < REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define ALERT_TEST_MASK \
    (R_ALERT_TEST_RECOV_FAULT_MASK | R_ALERT_TEST_FATAL_FAULT_MASK)

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(ALERT_TEST),       REG_NAME_ENTRY(EXTCLK_CTRL_REGWEN),
    REG_NAME_ENTRY(EXTCLK_CTRL),      REG_NAME_ENTRY(EXTCLK_STATUS),
    REG_NAME_ENTRY(JITTER_REGWEN),    REG_NAME_ENTRY(JITTER_ENABLE),
    REG_NAME_ENTRY(CLK_ENABLES),      REG_NAME_ENTRY(CLK_HINTS),
    REG_NAME_ENTRY(CLK_HINTS_STATUS), REG_NAME_ENTRY(MEASURE_CTRL_REGWEN),
    REG_NAME_ENTRY(RECOV_ERR_CODE),   REG_NAME_ENTRY(FATAL_ERR_CODE),
};
#undef REG_NAME_ENTRY

enum { ALERT_RECOVERABLE, ALERT_FATAL, ALERT_COUNT };

/* note: cannot use strlcpy as CentOS 7 (...) does not support it */
#define strbcpy(_b_, _d_, _s_) \
    do { \
        size_t l = ARRAY_SIZE(_b_) - 1u; \
        const char *end = (_b_) + l; \
        g_assert(end > (_d_)); \
        strncpy((_d_), (_s_), (size_t)((uintptr_t)end) - ((uintptr_t)(_d_))); \
    } while (0)

/*
 * Any clock.
 *
 * A clock may have derived clocks, denoted subclocks here, which are in sync
 * with their parent, but beat at a lower frequency, the ratio being stored
 * in the divider field.
 *
 * A clock may have outputs (leaf clocks) which are connected to other OT
 * devices via OtClkMgrClockSink.
 *
 * All clocks are instantiated at device realization time, from the clock
 * properties fields (which are usually loaded from a QEMU readconf file).
 */
typedef struct {
    char *name; /* clock name */
    GList *subclocks; /* weakrefs to OtClkMgrClock */
    GList *outputs; /* wekrefs to OtClkMgrClockOutput */
    unsigned divider; /* divider applied on parent clock if any (0 if top) */
    unsigned ratio; /* ratio w/ reference clock (may be 0) */
    bool ref; /* reference clock */
    bool loose; /* clock which is declared but not connected */
} OtClkMgrClock;

/*
 * Logical clock group.
 *
 * A group defines how clock can be activated/disabled, depending on the group
 * property.
 *
 * Each group may reference one or more physical clocks, and each clock may be
 * referenced in one or may groups.
 *
 * A group may be fully configured by SW, or only receive deactivation hints,
 * depending on the group property.
 *
 * All groups are instantiated at device realization time, from the clock
 * properties fields (which are usually loaded from a QEMU readconf file),
 * which also define the group properties (if any).
 */
typedef struct {
    char *name; /* group name */
    GList *clocks; /* weakrefs to OtClkMgrClock */
    bool sw_cg; /* software configurable */
    bool hint; /* software hintable */
} OtClkMgrClockGroup;

/*
 * Each clock output defines a unique physical (clock, group) pair. The clock
 * output beats at the pace of its clock, while its activation is driven by its
 * group.
 *
 * Each clock output may be connected to many output. To match the QEMU IRQ API,
 * each IRQ connection to another device is managed as clock sinks. All clock
 * sinks of a clock group behave (beat and active) the same.
 *
 * Clock outputs are lazily instantiated whenever a remote OT device queries the
 * clock manager via the #get_clock_source API, or when an output is defined as
 * a SW configurable output. This avoid creating useless clock output instances
 * as most defined HW clocks are not actually used in the QEMU implementation.
 */
typedef struct {
    OtClkMgrClock *clock; /* weak ref */
    const OtClkMgrClockGroup *group; /* weak ref */
    GList *sinks; /* OtClkMgrClockSink */
    unsigned frequency; /* computed frequency */
    bool disabled; /* whether the output is disabled */
} OtClkMgrClockOutput;

/*
 * A clock sink represents a unique connection from a clock output to a single
 * OT device clock input. Its name is dynmically generated based on its parent
 * output and the number of connected OT devices of this output.
 *
 * Clock sinks are lazily instantiated, when a connection for the specified OT
 * device and the selected clock output are queried via the #get_clock_source
 * API.
 *
 * It is guaranteed that an OT device querying the same clock receives the same
 * IRQ name.
 */
typedef struct {
    const char *irq_name;
    const DeviceState *dev;
    IbexIRQ out;
} OtClkMgrClockSink;

/*
 * Software configurable clock.
 *
 * This is used to handle clock enable/disable requests for clocks that can be
 * driven this way.
 *
 * The name of a SW configurable clock is suffixed with the group name, as this
 * identifier is used to sort the SW clocks. This identifier strongly matters to
 * order the SW clocks in the management fields.
 */
typedef struct {
    char *name; /* full clock name, built from <clock>_<group> */
    OtClkMgrClockOutput *output; /* weakref to the managed clock output */
} OtClkMgrSwCgClock;

/* Measure register pair */
typedef struct {
    uint32_t ctrl_en; /* multibit bool4 */
    OtShadowReg ctrl; /* value */
    OtClkMgrClock *clock; /* driven clocks */
} OtClkMgrMeasureRegs;

struct OtClkMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    IbexIRQ alerts[ALERT_COUNT];

    GList *clocks; /* OtClkMgrClock (top clocks and derived clocks) */
    GList *groups; /* OtClkMgrClockGroup */
    GList *outputs; /* OtClkMgrClockOutput */
    GList *ordered; /* OtClkMgrClock ordered weakref for register/field usage */
    OtClkMgrClock **tops; /* ordered array of wearef top clocks */
    OtClkMgrClock **hints; /* ordered array of wearef hintable clocks */
    OtClkMgrSwCgClock **swcgs; /* ordered array of SW configurable clocks */
    unsigned clock_count; /* count of all clocks */
    unsigned top_count; /* count of top clocks */
    unsigned hint_count; /* count of hintable clocks */
    unsigned swcg_count; /* count of SW configurable clocks */

    uint32_t clock_states; /* bit set: active, reset: clock is idle */
    uint32_t regs[REGS_COUNT]; /* shadowed slots are not used */
    OtClkMgrMeasureRegs *measure_regs;
    unsigned measure_count; /* count of measure_regs */
    bool input_clock_connected; /* true once clock source are connected */

    char *ot_id;
    DeviceState *clock_src; /* Top clock source */
    /* comma-separated definition list */
    /* pair of name:ratio of top level clocks, ratio w/ ref clock */
    char *cfg_topclocks;
    /* name of reference clock, i.e. clock that is not measured */
    char *cfg_refclock;
    /* name of top level loose clocks, i.e. clocks not connected */
    char *cfg_looseclocks;
    /* triplets of name:parent_name:divider derivated clock definitions */
    char *cfg_subclocks;
    /* pairs of name:clocks definition, where clocks are joined with '+' char */
    char *cfg_groups;
    /* list of software-configurable groups */
    char *cfg_swcg;
    /* list of software-hintable groups */
    char *cfg_hint;
    uint8_t version;
};

/*
 * Device state-index pair used as opaque data with the `ot_clkmgr_clock_hint`
 * clock hint callback.
 *
 * To enable dynamic clock-hinting across IP based on the clocks defined in the
 * property strings, we need to expose the clock hint signal as a named GPIO
 * input to the QDev, but by creating individual named signals instead of an
 * array we lose the context of the hint number.
 *
 * We hence must pass additional opaque data to describe the hint index.
 */
typedef struct {
    OtClkMgrState *state;
    unsigned hint_index;
} OtClkMgrHintContext;

struct OtClkMgrClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static const char *CFGSEP = ",";

static void ot_clkmgr_update_alerts(OtClkMgrState *s)
{
    bool recov = (bool)(s->regs[R_RECOV_ERR_CODE] &
                        R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK);
    ibex_irq_set(&s->alerts[ALERT_RECOVERABLE], recov);
}

/* NOLINTNEXTLINE(misc-no-recursion) */
static void ot_clkmgr_update_clock_frequency(
    OtClkMgrState *s, OtClkMgrClock *clk, unsigned input_freq)
{
    unsigned frequency =
        clk->divider > 1 ? (input_freq / clk->divider) : input_freq;

    trace_ot_clkmgr_update_clock(s->ot_id, clk->name, frequency);

    /* propagate to derived clocks */
    for (GList *cnode = clk->subclocks; cnode; cnode = cnode->next) {
        OtClkMgrClock *sclk = (OtClkMgrClock *)(cnode->data);
        ot_clkmgr_update_clock_frequency(s, sclk, frequency);
    }

    /* update clock outputs */
    for (GList *onode = clk->outputs; onode; onode = onode->next) {
        OtClkMgrClockOutput *out = (OtClkMgrClockOutput *)onode->data;

        out->frequency = frequency;

        /* update each sink */
        for (GList *snode = out->sinks; snode; snode = snode->next) {
            OtClkMgrClockSink *sink = (OtClkMgrClockSink *)(snode->data);
            unsigned active_freq = out->disabled ? 0 : out->frequency;
            trace_ot_clkmgr_update_sink(s->ot_id, sink->irq_name, active_freq);

            ibex_irq_set(&sink->out, (int)active_freq);
        }
    }
}

static void ot_clkmgr_update_swcg(OtClkMgrState *s, uint32_t change)
{
    for (unsigned ix = 0; ix < s->swcg_count; ix++) {
        uint32_t bm = 1u << ix;
        if (!(change & bm)) {
            continue;
        }

        bool enabled = (bool)(s->regs[R_CLK_ENABLES] & bm);

        OtClkMgrSwCgClock *swcg_clk = s->swcgs[ix];
        OtClkMgrClockOutput *out = swcg_clk->output;

        out->disabled = !enabled;

        /* update each sink */
        for (GList *snode = out->sinks; snode; snode = snode->next) {
            OtClkMgrClockSink *sink = (OtClkMgrClockSink *)(snode->data);
            unsigned active_freq = out->disabled ? 0 : out->frequency;
            trace_ot_clkmgr_update_sink(s->ot_id, sink->irq_name, active_freq);

            ibex_irq_set(&sink->out, (int)active_freq);
        }
    }
}

static void ot_clkmgr_clock_hint(void *opaque, int irq, int level)
{
    OtClkMgrHintContext *hint_ctx = opaque;
    OtClkMgrState *s = hint_ctx->state;
    unsigned hint = hint_ctx->hint_index;

    g_assert(irq == 0);
    g_assert(hint < s->hint_count);

    trace_ot_clkmgr_clock_hint(s->ot_id, s->hints[hint]->name, hint,
                               (bool)level);

    if (level) {
        s->clock_states |= 1u << hint;
    } else {
        s->clock_states &= ~(1u << hint);
    }
}

static void ot_clkmgr_clock_input(void *opaque, int irq, int level)
{
    OtClkMgrState *s = opaque;

    unsigned clknum = (unsigned)irq;
    g_assert(clknum < s->clock_count);

    OtClkMgrClock *clk = g_list_nth_data(s->clocks, clknum);
    g_assert(clk);

    trace_ot_clkmgr_clock_input(s->ot_id, clk->name, (unsigned)level);

    ot_clkmgr_update_clock_frequency(s, clk, (unsigned)level);
}

static uint32_t ot_clkmgr_get_clock_hints(OtClkMgrState *s)
{
    uint32_t hint_status = s->regs[R_CLK_HINTS] | s->clock_states;

    trace_ot_clkmgr_get_clock_hints(s->ot_id, s->regs[R_CLK_HINTS],
                                    s->clock_states, hint_status);

    return hint_status;
}

static gint ot_clkmgr_compare_group_by_name(gconstpointer a, gconstpointer b)
{
    const OtClkMgrClockGroup *ga = a;
    const OtClkMgrClockGroup *gb = b;

    return strcmp(ga->name, gb->name);
}

static OtClkMgrClockGroup *
ot_clkmgr_find_group(OtClkMgrState *s, const char *name)
{
    OtClkMgrClockGroup group = { .name = (char *)name };

    GList *glist =
        g_list_find_custom(s->groups, &group, &ot_clkmgr_compare_group_by_name);

    return glist ? glist->data : NULL;
}

static gint ot_clkmgr_compare_clock_by_name(gconstpointer a, gconstpointer b)
{
    const OtClkMgrClock *ca = a;
    const OtClkMgrClock *cb = b;

    return strcmp(ca->name, cb->name);
}

static gint ot_clkmgr_compare_clock_by_name_with_data(
    gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    return ot_clkmgr_compare_clock_by_name(a, b);
}


static OtClkMgrClock *ot_clkmgr_find_clock(GList *clock_list, const char *name)
{
    OtClkMgrClock clk = { .name = (char *)name };

    GList *glist =
        g_list_find_custom(clock_list, &clk, &ot_clkmgr_compare_clock_by_name);

    return glist ? glist->data : NULL;
}

static gint ot_clkmgr_match_output(gconstpointer a, gconstpointer b)
{
    const OtClkMgrClockOutput *oa = a;
    const OtClkMgrClockOutput *ob = b;

    return (int)((oa->clock != ob->clock) || (oa->group != ob->group));
}

static OtClkMgrClockOutput *ot_clkmgr_find_output(
    GList *outlist, const OtClkMgrClockGroup *group, const OtClkMgrClock *clock)
{
    const OtClkMgrClockOutput output = {
        .clock = (OtClkMgrClock *)clock,
        .group = (OtClkMgrClockGroup *)group,
    };

    GList *glist =
        g_list_find_custom(outlist, &output, &ot_clkmgr_match_output);

    return glist ? glist->data : NULL;
}

static OtClkMgrClockOutput *ot_clkmgr_get_output(
    OtClkMgrState *s, const OtClkMgrClockGroup *group, OtClkMgrClock *clock)
{
    OtClkMgrClockOutput *clk_out =
        ot_clkmgr_find_output(s->outputs, group, clock);

    if (!clk_out) {
        clk_out = g_new0(OtClkMgrClockOutput, 1u);
        clk_out->clock = clock;
        clk_out->group = group;
        s->outputs = g_list_append(s->outputs, clk_out);
        clock->outputs = g_list_append(clock->outputs, clk_out);
        char *outname = g_strdup_printf("%s.%s", group->name, clock->name);
        trace_ot_clkmgr_create(s->ot_id, "output", outname);
        g_free(outname);
    }

    return clk_out;
}

static gint ot_clkmgr_match_sink(gconstpointer a, gconstpointer b)
{
    const OtClkMgrClockSink *sa = a;
    const OtClkMgrClockSink *sb = b;

    return (int)(sa->dev != sb->dev);
}

static OtClkMgrClockSink *
ot_clkmgr_find_sink(OtClkMgrClockOutput *clkout, const DeviceState *dev)
{
    const OtClkMgrClockSink sink = {
        .dev = dev,
    };

    GList *glist =
        g_list_find_custom(clkout->sinks, &sink, &ot_clkmgr_match_sink);

    return glist ? glist->data : NULL;
}

static const char *
ot_clkmgr_get_clock_source(IbexClockSrcIf *ifd, const char *name,
                           const DeviceState *sinkdev, Error **errp)
{
    OtClkMgrState *s = OT_CLKMGR(ifd);

    gchar **parts = g_strsplit(name, ".", 2);

    if (g_strv_length(parts) < 2) {
        g_strfreev(parts);
        /* clock manager always require a group name */
        error_setg(errp, "%s: %s: group not defined: %s", __func__, s->ot_id,
                   name);
        return NULL;
    }

    OtClkMgrClockGroup *group = ot_clkmgr_find_group(s, parts[0]);
    if (!group) {
        error_setg(errp, "%s: %s: no such group: %s", __func__, s->ot_id,
                   parts[0]);
        g_strfreev(parts);
        return NULL;
    };

    OtClkMgrClock *clock = ot_clkmgr_find_clock(group->clocks, parts[1]);
    if (!clock) {
        error_setg(errp, "%s: %s: no such clock %s.%s", __func__, s->ot_id,
                   parts[0], parts[1]);
        g_strfreev(parts);
        return NULL;
    };

    g_strfreev(parts);

    OtClkMgrClockOutput *clk_out = ot_clkmgr_get_output(s, group, clock);

    OtClkMgrClockSink *clk_sink = NULL;
    bool first = g_list_length(clk_out->sinks) == 0;
    if (first) {
        clk_sink = ot_clkmgr_find_sink(clk_out, sinkdev);
    }
    if (!clk_sink) {
        if (!first) {
            if (clk_out->group->hint) {
                error_setg(errp,
                           "%s: %s: hintable clock %s can only have one sink, "
                           "deny sink %s",
                           __func__, s->ot_id, clk_out->clock->name,
                           object_get_typename(OBJECT(sinkdev)));
                return NULL;
            }
        }
        clk_sink = g_new0(OtClkMgrClockSink, 1u);
        clk_sink->irq_name =
            g_strdup_printf("clock-out-%s-%s-%d", group->name, clock->name,
                            g_list_length(clk_out->sinks));
        ibex_qdev_init_irq(OBJECT(s), &clk_sink->out, clk_sink->irq_name);
        clk_out->sinks = g_list_append(clk_out->sinks, clk_sink);
        const char *sinktype = object_get_typename(OBJECT(sinkdev));
        char *ot_id =
            object_property_get_str(OBJECT(sinkdev), OT_COMMON_DEV_ID, NULL);
        trace_ot_clkmgr_register_sink(s->ot_id, clk_sink->irq_name, sinktype,
                                      ot_id ?: "?");
        g_free(ot_id);
    };

    return clk_sink->irq_name;
}

static void ot_clkmgr_parse_top_clocks(OtClkMgrState *s, Error **errp)
{
    if (!s->cfg_topclocks) {
        error_setg(errp, "%s: topclocks config not defined", __func__);
        return;
    }

    char *config = g_strdup(s->cfg_topclocks);
    for (char *clkbrk, *clkdesc = strtok_r(config, CFGSEP, &clkbrk); clkdesc;
         clkdesc = strtok_r(NULL, CFGSEP, &clkbrk)) {
        char clkname[16];
        unsigned clkratio = 0;
        unsigned length = 0;
        int ret;
        /* NOLINTNEXTLINE(cert-err34-c) */
        ret = sscanf(clkdesc, "%15[a-z0-9_]:%u%n", clkname, &clkratio, &length);
        if (ret != 2) {
            error_setg(errp, "%s: %s: invalid top clock %s format: %d",
                       __func__, s->ot_id, s->cfg_topclocks, ret);
            break;
        }
        if (clkdesc[length]) {
            error_setg(errp, "%s: %s: trailing chars in top clock %s", __func__,
                       s->ot_id, clkdesc);
            break;
        }
        if (!clkratio || clkratio > UINT16_MAX) {
            error_setg(errp, "%s: %s: invalid ratio in top clock %s", __func__,
                       s->ot_id, clkdesc);
            break;
        }
        if (ot_clkmgr_find_clock(s->clocks, clkname)) {
            error_setg(errp, "%s: %s: top clock redefinition '%s'", __func__,
                       s->ot_id, clkname);
            break;
        }
        OtClkMgrClock *clk = g_new0(OtClkMgrClock, 1u);
        clk->name = g_strdup(clkname);
        clk->ref = s->cfg_refclock && !strcmp(clkname, s->cfg_refclock);
        clk->ratio = clkratio;
        s->clocks = g_list_append(s->clocks, clk);
        trace_ot_clkmgr_create(s->ot_id, "clock", clk->name);
    }
    g_free(config);
}

static void ot_clkmgr_parse_loose_clocks(OtClkMgrState *s, Error **errp)
{
    if (!s->cfg_looseclocks) {
        return;
    }

    char *config = g_strdup(s->cfg_looseclocks);
    for (char *clkbrk, *clkname = strtok_r(config, CFGSEP, &clkbrk); clkname;
         clkname = strtok_r(NULL, CFGSEP, &clkbrk)) {
        OtClkMgrClock *clk = ot_clkmgr_find_clock(s->clocks, clkname);
        if (!clk) {
            error_setg(errp, "%s: %s: no such loose clock '%s'", __func__,
                       s->ot_id, clkname);
            break;
        }
        clk->loose = true;
        trace_ot_clkmgr_create(s->ot_id, "loose", clk->name);
    }
    g_free(config);
}

static void ot_clkmgr_parse_derived_clocks(OtClkMgrState *s, Error **errp)
{
    if (!s->cfg_subclocks) {
        error_setg(errp, "%s: subclocks config not defined", __func__);
        return;
    }

    /* parse derived clocks */
    char *config = g_strdup(s->cfg_subclocks);
    for (char *clkbrk, *clkdesc = strtok_r(config, CFGSEP, &clkbrk); clkdesc;
         clkdesc = strtok_r(NULL, CFGSEP, &clkbrk)) {
        char subclkname[16];
        char clkname[16];
        unsigned clkdiv = 0;
        unsigned length = 0;
        /* NOLINTNEXTLINE(cert-err34-c) */
        int ret = sscanf(clkdesc, "%15[a-z0-9_]:%15[a-z0-9_]:%u%n", subclkname,
                         clkname, &clkdiv, &length);
        if (ret != 3) {
            error_setg(errp, "%s: %s: invalid subclock %s format: %d", __func__,
                       s->ot_id, clkdesc, ret);
            break;
        }
        if (clkdesc[length]) {
            error_setg(errp, "%s: %s: trailing chars in subclock %s", __func__,
                       s->ot_id, clkdesc);
            break;
        }
        if (!clkdiv || clkdiv > UINT16_MAX) {
            error_setg(errp, "%s: %s: invalid divider in subclock %s", __func__,
                       s->ot_id, clkdesc);
            break;
        }
        if (ot_clkmgr_find_clock(s->clocks, subclkname)) {
            error_setg(errp, "%s: %s: derived clock redefinition '%s'",
                       __func__, s->ot_id, subclkname);
            break;
        }
        OtClkMgrClock *parclk = ot_clkmgr_find_clock(s->clocks, clkname);
        if (!parclk) {
            error_setg(errp, "%s: %s: invalid parent clock '%s' for %s",
                       __func__, s->ot_id, clkname, subclkname);
            break;
        }
        OtClkMgrClock *clk = g_new0(OtClkMgrClock, 1u);
        clk->name = g_strdup(subclkname);
        clk->divider = clkdiv;
        clk->ratio = parclk->ratio / clk->divider;
        s->clocks = g_list_append(s->clocks, clk);
        parclk->subclocks = g_list_append(parclk->subclocks, clk);
        trace_ot_clkmgr_create(s->ot_id, "subclock", clk->name);
    }
    g_free(config);
}

static void ot_clkmgr_parse_groups(OtClkMgrState *s, Error **errp)
{
    if (!s->cfg_groups) {
        error_setg(errp, "%s: groups config not defined", __func__);
        return;
    }

    char *config = g_strdup(s->cfg_groups);
    for (char *clkbrk, *clkdesc = strtok_r(config, CFGSEP, &clkbrk); clkdesc;
         clkdesc = strtok_r(NULL, CFGSEP, &clkbrk)) {
        char groupname[16];
        unsigned length = 0;
        int ret = sscanf(clkdesc, "%15[a-z0-9_]:%n", groupname, &length);
        if (ret != 1 || !clkdesc[length]) {
            error_setg(errp, "%s: %s: invalid group %s format: %d", __func__,
                       s->ot_id, clkdesc, ret);
            break;
        }

        if (ot_clkmgr_find_group(s, groupname)) {
            error_setg(errp, "%s: %s: multiple group definitions '%s'",
                       __func__, s->ot_id, groupname);
            break;
        }

        OtClkMgrClockGroup *grp = g_new0(OtClkMgrClockGroup, 1u);
        grp->name = g_strdup(groupname);
        trace_ot_clkmgr_create(s->ot_id, "group", grp->name);

        const char *gsep = "+";
        for (char *grpbrk, *clkname = strtok_r(&clkdesc[length], gsep, &grpbrk);
             clkname; clkname = strtok_r(NULL, gsep, &grpbrk)) {
            OtClkMgrClock *clk = ot_clkmgr_find_clock(s->clocks, clkname);
            if (!clk) {
                error_setg(errp, "%s: %s: invalid group clock '%s' for %s",
                           __func__, s->ot_id, clkname, groupname);
                g_free(grp->name);
                g_free(grp);
                grp = NULL;
                break;
            }
            grp->clocks = g_list_append(grp->clocks, clk);
            trace_ot_clkmgr_add_group(s->ot_id, clk->name, grp->name);
        }

        if (grp) {
            s->groups = g_list_append(s->groups, grp);
        }
    }
    g_free(config);
}

static void ot_clkmgr_parse_sw_cg(OtClkMgrState *s, Error **errp)
{
    if (!s->cfg_swcg) {
        error_setg(errp, "%s: sw configurable clocks not defined", __func__);
        return;
    }

    char *config = g_strdup(s->cfg_swcg);
    for (char *grpbrk, *grpname = strtok_r(config, CFGSEP, &grpbrk); grpname;
         grpname = strtok_r(NULL, CFGSEP, &grpbrk)) {
        OtClkMgrClockGroup *grp = ot_clkmgr_find_group(s, grpname);
        if (!grp) {
            error_setg(errp, "%s: %s: invalid group '%s' for sw_cg", __func__,
                       s->ot_id, grpname);
            break;
        }

        grp->sw_cg = true;
    }
    g_free(config);
}

static unsigned ot_clkmgr_parse_hint(OtClkMgrState *s, Error **errp)
{
    unsigned hint_count = 0;

    if (!s->cfg_hint) {
        error_setg(errp, "%s: hintable clocks not defined", __func__);
        return hint_count;
    }

    char *config = g_strdup(s->cfg_hint);
    for (char *grpbrk, *grpname = strtok_r(config, CFGSEP, &grpbrk); grpname;
         grpname = strtok_r(NULL, CFGSEP, &grpbrk)) {
        OtClkMgrClockGroup *grp = ot_clkmgr_find_group(s, grpname);
        if (!grp) {
            error_setg(errp, "%s: %s: invalid group '%s' for hint", __func__,
                       s->ot_id, grpname);
            break;
        }

        grp->hint = true;
        hint_count = g_list_length(grp->clocks);
    }
    g_free(config);

    return hint_count;
}

static void ot_clkmgr_assign_top(gpointer data, gpointer user_data)
{
    OtClkMgrState *s = user_data;
    OtClkMgrClock *clk = data;

    s->tops[s->top_count++] = clk;
}

static void ot_clkmgr_connect_input_clocks(OtClkMgrState *s)
{
    IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_GET_CLASS(s->clock_src);
    IbexClockSrcIf *ii = IBEX_CLOCK_SRC_IF(s->clock_src);

    for (unsigned cix = 0; cix < s->top_count; cix++) {
        OtClkMgrClock *clk = s->tops[cix];
        if (clk->loose) {
            /* do not attempt to connect this clock */
            continue;
        }
        const char *irq_name =
            ic->get_clock_source(ii, clk->name, DEVICE(s), &error_fatal);
        qemu_irq in_irq =
            qdev_get_gpio_in_named(DEVICE(s), "clock-in", (int)cix);
        qdev_connect_gpio_out_named(s->clock_src, irq_name, 0, in_irq);
        trace_ot_clkmgr_connect_input_clock(s->ot_id, irq_name, cix);
    }
}

static void ot_clkmgr_configure_groups(gpointer data, gpointer user_data)
{
    OtClkMgrState *s = user_data;
    OtClkMgrClockGroup *group = data;

    if (group->hint) {
        for (GList *cnode = group->clocks; cnode; cnode = cnode->next) {
            OtClkMgrClock *clk = (OtClkMgrClock *)(cnode->data);

            OtClkMgrHintContext *hint_ctx = g_new0(OtClkMgrHintContext, 1u);
            hint_ctx->state = s;
            hint_ctx->hint_index = s->hint_count;

            char *hintname = g_strdup_printf(OT_CLOCK_HINT_PREFIX "%s.%s",
                                             group->name, clk->name);
            qdev_init_gpio_in_named_with_opaque(DEVICE(s),
                                                &ot_clkmgr_clock_hint, hint_ctx,
                                                hintname, 1);
            trace_ot_clkmgr_create(s->ot_id, "hint", hintname);
            g_free(hintname);

            s->hints[s->hint_count++] = clk;
        }
    }
}

static void ot_clkmgr_add_clock(gpointer data, gpointer user_data)
{
    OtClkMgrClock *clk = data;
    OtClkMgrState *s = user_data;

    /* discard reference clock and duplicates */
    if (!clk->ref && !ot_clkmgr_find_clock(s->ordered, clk->name)) {
        bool hint = false;
        for (unsigned hix = 0; hix < s->hint_count; hix++) {
            if (clk == s->hints[hix]) {
                hint = true;
                break;
            }
        }
        /* hint clocks are only aliases, not main clocks */
        if (!hint) {
            s->ordered = g_list_append(s->ordered, clk);
        }
    }

    /* add derived clocks */
    g_list_foreach(clk->subclocks, &ot_clkmgr_add_clock, s);
}

static void ot_clkmgr_sort_clocks(OtClkMgrState *s)
{
    g_assert(!s->ordered);

    g_list_foreach(s->clocks, &ot_clkmgr_add_clock, s);

    s->ordered =
        g_list_sort_with_data(s->ordered,
                              &ot_clkmgr_compare_clock_by_name_with_data, NULL);
}

static gint ot_clkmgr_compare_swcg_by_name(gconstpointer a, gconstpointer b)
{
    const OtClkMgrSwCgClock *sa = a;
    const OtClkMgrSwCgClock *sb = b;

    return strcmp(sa->name, sb->name);
}

static gint ot_clkmgr_compare_swcg_by_name_with_data(
    gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    return ot_clkmgr_compare_swcg_by_name(a, b);
}

static GList *
ot_clkmgr_build_swcg_clock_name(OtClkMgrState *s, GList *swcgs,
                                const OtClkMgrClockGroup *group, GList *clocks)
{
    for (GList *node = clocks; node; node = node->next) {
        OtClkMgrClock *clk = (OtClkMgrClock *)(node->data);
        /*
         * hack to remove AON from the configurable list, there is something
         * weird to address as peri_aon is defined as sw_cg but not present in
         * the CLK_ENABLES register...
         * */
        if (!strcmp(clk->name, s->cfg_refclock)) {
            continue;
        }

        OtClkMgrSwCgClock *swcg_clk = g_new0(OtClkMgrSwCgClock, 1u);
        swcg_clk->name = g_strdup_printf("%s_%s", clk->name, group->name);
        swcg_clk->output = ot_clkmgr_get_output(s, group, clk);
        g_assert(swcg_clk->output);

        /* ignore duplicates */
        if (g_list_find_custom(swcgs, swcg_clk,
                               &ot_clkmgr_compare_swcg_by_name)) {
            g_free(swcg_clk->name);
            g_free(swcg_clk);
            continue;
        }
        swcgs = g_list_append(swcgs, swcg_clk);
    }

    return swcgs;
}

static void ot_clkmgr_generate_swcg_clocks(OtClkMgrState *s)
{
    GList *swcgs = NULL;

    for (GList *node = s->groups; node; node = node->next) {
        const OtClkMgrClockGroup *group =
            (const OtClkMgrClockGroup *)(node->data);
        if (!group->sw_cg) {
            continue;
        }

        swcgs = ot_clkmgr_build_swcg_clock_name(s, swcgs, group, group->clocks);
    }

    swcgs =
        g_list_sort_with_data(swcgs, &ot_clkmgr_compare_swcg_by_name_with_data,
                              NULL);

    s->swcg_count = g_list_length(swcgs);
    s->swcgs = g_new0(OtClkMgrSwCgClock *, s->swcg_count);
    unsigned ix = 0;
    for (GList *node = swcgs; node; node = node->next, ix++) {
        OtClkMgrSwCgClock *swcg = (OtClkMgrSwCgClock *)(node->data);
        s->swcgs[ix] = swcg;
        trace_ot_clkmgr_define_swcg(s->ot_id, ix, swcg->name);
    }

    /* discard the list, not the items that have been moved to the array */
    g_list_free(swcgs);
}

static void ot_clkmgr_create_measure_regs(OtClkMgrState *s)
{
    /* generate the ordered list of clocks for register/field indexed access */
    ot_clkmgr_sort_clocks(s);

    s->measure_count = g_list_length(s->ordered);

    /* the following registers are indexed by the s->ordered list */
    s->measure_regs = g_new0(OtClkMgrMeasureRegs, s->measure_count);

    unsigned ix = 0;
    for (GList *node = s->ordered; node; node = node->next, ix++) {
        OtClkMgrClock *clk = (OtClkMgrClock *)(node->data);

        trace_ot_clkmgr_define_meas(s->ot_id, ix, clk->name);

        s->measure_regs[ix].clock = clk;
    }
}

static void ot_clkmgr_reset_measure_regs(OtClkMgrState *s)
{
    for (unsigned ix = 0; ix < s->measure_count; ix++) {
        OtClkMgrMeasureRegs *mreg = &s->measure_regs[ix];

        mreg->ctrl_en = OT_MULTIBITBOOL4_TRUE;

        uint32_t hi = mreg->clock->ratio + 10u;
        uint32_t lo = (uint32_t)MAX(0, ((int)mreg->clock->ratio) - 10);

        uint32_t value = 0;
        value = SHARED_FIELD_DP32(value, MEAS_CTRL_SHADOWED_HI, hi);
        value = SHARED_FIELD_DP32(value, MEAS_CTRL_SHADOWED_LO, lo);

        trace_ot_clkmgr_reset_meas(s->ot_id, ix, mreg->clock->name, lo, hi);

        ot_shadow_reg_init(&s->measure_regs[ix].ctrl, value);
    }
}

static uint64_t ot_clkmgr_read(void *opaque, hwaddr addr, unsigned size)
{
    OtClkMgrState *s = opaque;
    (void)size;

    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_EXTCLK_CTRL_REGWEN:
    case R_EXTCLK_CTRL:
    case R_EXTCLK_STATUS:
    case R_JITTER_REGWEN:
    case R_JITTER_ENABLE:
    case R_CLK_ENABLES:
    case R_CLK_HINTS:
    case R_MEASURE_CTRL_REGWEN:
        val32 = s->regs[reg];
        break;
    case R_CLK_HINTS_STATUS:
        val32 = ot_clkmgr_get_clock_hints(s);
        break;
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: W/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    /* statically defined register offsets, handled with switch statement */
    if (reg < R_MEASURE_REG_BASE) {
        trace_ot_clkmgr_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg),
                                    val32, pc);
        return (uint64_t)val32;
    }

    if (reg < (R_MEASURE_REG_BASE + (s->measure_count * 2u))) {
        /* measure registers */
        unsigned measure = (reg - R_MEASURE_REG_BASE) >> 1u;
        unsigned offset = (reg - R_MEASURE_REG_BASE) & 1u;
        g_assert(measure < s->measure_count);
        OtClkMgrMeasureRegs *mreg = &s->measure_regs[measure];
        GList *node = g_list_nth(s->ordered, measure);
        g_assert(node);
        const OtClkMgrClock *clk = (const OtClkMgrClock *)node->data;

        char reg_name[40u];
        unsigned ix = 0;
        for (; clk->name[ix] && ix < 16u; ix++) {
            reg_name[ix] = (char)toupper(clk->name[ix]);
        }
        strbcpy(reg_name, &reg_name[ix], "_MEAS_CTRL_");
        strbcpy(reg_name, &reg_name[ix] + ARRAY_SIZE("_MEAS_CTRL_") - 1u,
                offset ? "SHADOWED" : "EN");

        switch (offset) {
        case 0u:
            val32 = mreg->ctrl_en;
            break;
        case 1u:
            val32 = ot_shadow_reg_read(&mreg->ctrl);
            break;
        default:
            g_assert_not_reached();
            break;
        }

        trace_ot_clkmgr_io_read_out(s->ot_id, (uint32_t)addr, reg_name, val32,
                                    pc);
        return (uint64_t)val32;
    }

    /* remaining registers after the dynamic register range */
    unsigned reg_err = reg - (s->measure_count * 2u);

    switch (reg_err) {
    case R_RECOV_ERR_CODE:
    case R_FATAL_ERR_CODE:
        val32 = s->regs[reg_err];
        break;
    default:
        val32 = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        break;
    }

    trace_ot_clkmgr_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg_err),
                                val32, pc);

    return (uint64_t)val32;
};

static void ot_clkmgr_write(void *opaque, hwaddr addr, uint64_t val64,
                            unsigned size)
{
    OtClkMgrState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();

    /* statically defined register offsets, handled with switch statement */
    if (reg < R_MEASURE_REG_BASE) {
        trace_ot_clkmgr_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                 pc);
    }

    switch (reg) {
    case R_ALERT_TEST:
        val32 &= ALERT_TEST_MASK;
        for (unsigned ix = 0; ix < ALERT_COUNT; ix++) {
            ibex_irq_set(&s->alerts[ix], (int)((val32 >> ix) & 0x1u));
        }
        break;
    case R_EXTCLK_CTRL_REGWEN:
        val32 &= R_EXTCLK_CTRL_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_EXTCLK_CTRL:
        if (s->regs[R_EXTCLK_CTRL_REGWEN]) {
            val32 &= R_EXTCLK_CTRL_SEL_MASK | R_EXTCLK_CTRL_HI_SPEED_SEL_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: EXTCLK_CTRL protected w/ REGWEN\n", __func__);
        }
        break;
    case R_JITTER_REGWEN:
        val32 &= R_JITTER_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_JITTER_ENABLE:
        if (s->regs[R_JITTER_REGWEN] ||
            s->version == OT_CLKMGR_VERSION_EG_1_0_0) {
            if (s->version == OT_CLKMGR_VERSION_EG_1_0_0) {
                qemu_log_mask(
                    LOG_GUEST_ERROR,
                    "%s: JITTER_ENABLE should be protected w/ REGWEN,\n"
                    "but is allowed due to a known bug in Earlgrey 1.0.0\n",
                    __func__);
            }
            val32 &= R_JITTER_ENABLE_VAL_MASK;
            s->regs[reg] = val32;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: JITTER_ENABLE protected w/ REGWEN\n", __func__);
        }
        break;
    case R_CLK_ENABLES: {
        uint32_t prev = s->regs[reg];
        val32 &= (1u << s->swcg_count) - 1u;
        s->regs[reg] = val32;
        ot_clkmgr_update_swcg(s, prev ^ s->regs[reg]);
    } break;
    case R_CLK_HINTS:
        val32 &= (1u << s->hint_count) - 1u;
        s->regs[reg] = val32;
        break;
    case R_MEASURE_CTRL_REGWEN:
        val32 &= R_MEASURE_CTRL_REGWEN_EN_MASK;
        s->regs[reg] &= val32;
        break;
    case R_EXTCLK_STATUS:
    case R_CLK_HINTS_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        break;
    default:
        break;
    }

    /* low, statically defined register offsets, handled above */
    if (reg < R_MEASURE_REG_BASE) {
        return;
    }

    if (reg < (R_MEASURE_REG_BASE + (s->measure_count * 2u))) {
        /* measure registers */
        unsigned measure = (reg - R_MEASURE_REG_BASE) >> 1u;
        unsigned offset = (reg - R_MEASURE_REG_BASE) & 1u;
        g_assert(measure < s->measure_count);
        OtClkMgrMeasureRegs *mreg = &s->measure_regs[measure];
        GList *node = g_list_nth(s->ordered, measure);
        g_assert(node);
        const OtClkMgrClock *clk = (const OtClkMgrClock *)(node->data);

        char reg_name[40u];
        unsigned ix = 0;
        for (; clk->name[ix] && ix < 20u; ix++) {
            reg_name[ix] = (char)toupper(clk->name[ix]);
        }
        strbcpy(reg_name, &reg_name[ix], "_MEAS_CTRL_");
        strbcpy(reg_name, &reg_name[ix] + ARRAY_SIZE("_MEAS_CTRL_") - 1u,
                offset ? "SHADOWED" : "EN");
        trace_ot_clkmgr_io_write(s->ot_id, (uint32_t)addr, reg_name, val32, pc);

        if (!s->regs[R_MEASURE_CTRL_REGWEN]) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ REGWEN\n",
                          __func__, s->ot_id, reg_name);
            return;
        }

        switch (offset) {
        case 0u:
            if (mreg->ctrl_en == OT_MULTIBITBOOL4_TRUE) {
                val32 &= MEAS_CTRL_EN_MASK;
                mreg->ctrl_en = val32;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ EN\n",
                              __func__, s->ot_id, reg_name);
            }
            break;
        case 1u:
            if (mreg->ctrl_en) {
                val32 &=
                    MEAS_CTRL_SHADOWED_HI_MASK | MEAS_CTRL_SHADOWED_LO_MASK;
                switch (ot_shadow_reg_write(&mreg->ctrl, val32)) {
                case OT_SHADOW_REG_STAGED:
                case OT_SHADOW_REG_COMMITTED:
                    break;
                case OT_SHADOW_REG_ERROR:
                default:
                    s->regs[R_RECOV_ERR_CODE] |=
                        R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_MASK;
                    ot_clkmgr_update_alerts(s);
                }
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: %s protected w/ EN\n",
                              __func__, s->ot_id, reg_name);
            }
            break;
        default:
            g_assert_not_reached();
            break;
        }

        return;
    }

    /* remaining registers after the dynamic register range */
    unsigned reg_err = reg - (s->measure_count * 2u);

    trace_ot_clkmgr_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg_err), val32,
                             pc);

    switch (reg_err) {
    case R_RECOV_ERR_CODE:
        val32 &= (1u << ((R_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_SHIFT + 1u +
                          (s->measure_count * 2u)))) -
                 1u;
        s->regs[reg_err] &= ~val32; /* RW1C */
        break;
    case R_FATAL_ERR_CODE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: R/O register 0x%02x (%s)\n",
                      __func__, (uint32_t)addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%02x\n", __func__,
                      (uint32_t)addr);
        break;
    }
};

static const Property ot_clkmgr_properties[] = {
    DEFINE_PROP_STRING(OT_COMMON_DEV_ID, OtClkMgrState, ot_id),
    DEFINE_PROP_LINK("clock-src", OtClkMgrState, clock_src, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_STRING("topclocks", OtClkMgrState, cfg_topclocks),
    DEFINE_PROP_STRING("refclock", OtClkMgrState, cfg_refclock),
    DEFINE_PROP_STRING("looseclocks", OtClkMgrState, cfg_looseclocks),
    DEFINE_PROP_STRING("subclocks", OtClkMgrState, cfg_subclocks),
    DEFINE_PROP_STRING("groups", OtClkMgrState, cfg_groups),
    DEFINE_PROP_STRING("swcg", OtClkMgrState, cfg_swcg),
    DEFINE_PROP_STRING("hint", OtClkMgrState, cfg_hint),
    DEFINE_PROP_UINT8("version", OtClkMgrState, version, UINT8_MAX),
};

static const MemoryRegionOps ot_clkmgr_regs_ops = {
    .read = &ot_clkmgr_read,
    .write = &ot_clkmgr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_clkmgr_reset_enter(Object *obj, ResetType type)
{
    OtClkMgrClass *c = OT_CLKMGR_GET_CLASS(obj);
    OtClkMgrState *s = OT_CLKMGR(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    memset(s->regs, 0, sizeof(s->regs));

    s->regs[R_EXTCLK_CTRL_REGWEN] = 0x1u;
    s->regs[R_EXTCLK_CTRL] = 0x99u;
    s->regs[R_EXTCLK_STATUS] = 0x9u;
    s->regs[R_JITTER_REGWEN] = 0x1u;
    s->regs[R_JITTER_ENABLE] = 0x9u;
    s->regs[R_CLK_ENABLES] = 0xfu;
    s->regs[R_CLK_HINTS] = 0xfu;
    s->regs[R_CLK_HINTS_STATUS] = 0xfu;
    s->regs[R_MEASURE_CTRL_REGWEN] = 0x1u;

    for (unsigned ix = 0; ix < s->swcg_count; ix++) {
        g_assert(s->swcgs[ix] && s->swcgs[ix]->output);
        s->swcgs[ix]->output->disabled = 0;
    }

    ot_clkmgr_reset_measure_regs(s);

    for (unsigned ix = 0; ix < ALERT_COUNT; ix++) {
        ibex_irq_set(&s->alerts[ix], 0);
    }

    if (!s->input_clock_connected) {
        ot_clkmgr_connect_input_clocks(s);
        s->input_clock_connected = true;
    }
}

static void ot_clkmgr_realize(DeviceState *dev, Error **errp)
{
    OtClkMgrState *s = OT_CLKMGR(dev);
    (void)errp; /* do not want to use abort */

    g_assert(s->clock_src);
    OBJECT_CHECK(IbexClockSrcIf, s->clock_src, TYPE_IBEX_CLOCK_SRC_IF);

    g_assert(s->version < OT_CLKMGR_VERSION_COUNT);

    ot_clkmgr_parse_top_clocks(s, &error_fatal);
    unsigned top_count = g_list_length(s->clocks);
    qdev_init_gpio_in_named(DEVICE(s), &ot_clkmgr_clock_input, "clock-in",
                            (int)top_count);
    s->tops = g_new0(OtClkMgrClock *, top_count);
    s->top_count = 0u;
    g_list_foreach(s->clocks, &ot_clkmgr_assign_top, s);
    g_assert(s->top_count == top_count);

    Error *err_warn;

    ot_clkmgr_parse_loose_clocks(s, &error_fatal);
    /* not fatal per se, but highly likely to lead to missing clocks */
    err_warn = NULL;
    ot_clkmgr_parse_derived_clocks(s, &err_warn);
    if (err_warn) {
        warn_report_err(err_warn);
    }
    /* at least one group is required */
    ot_clkmgr_parse_groups(s, &error_fatal);
    /* following configs are not mandatory, however always defined */
    err_warn = NULL;
    ot_clkmgr_parse_sw_cg(s, &err_warn);
    if (err_warn) {
        warn_report_err(err_warn);
    }
    err_warn = NULL;
    unsigned hint_count = ot_clkmgr_parse_hint(s, &err_warn);
    if (err_warn) {
        warn_report_err(err_warn);
    }

    s->clock_count = g_list_length(s->clocks);

    s->hints = g_new0(OtClkMgrClock *, hint_count);
    s->hint_count = 0u;
    g_list_foreach(s->groups, &ot_clkmgr_configure_groups, s);
    g_assert(s->hint_count == hint_count);

    ot_clkmgr_generate_swcg_clocks(s);

    ot_clkmgr_create_measure_regs(s);

    memory_region_init_io(&s->mmio, OBJECT(dev), &ot_clkmgr_regs_ops, s,
                          TYPE_OT_CLKMGR,
                          REGS_SIZE + s->measure_count * 2u * sizeof(uint32_t));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void ot_clkmgr_init(Object *obj)
{
    OtClkMgrState *s = OT_CLKMGR(obj);

    for (unsigned ix = 0; ix < ALERT_COUNT; ix++) {
        ibex_qdev_init_irq(obj, &s->alerts[ix], OT_DEVICE_ALERT);
    }
}

static void ot_clkmgr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_clkmgr_realize;
    device_class_set_props(dc, ot_clkmgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    ResettableClass *rc = RESETTABLE_CLASS(klass);
    OtClkMgrClass *cc = OT_CLKMGR_CLASS(klass);
    resettable_class_set_parent_phases(rc, &ot_clkmgr_reset_enter, NULL, NULL,
                                       &cc->parent_phases);

    IbexClockSrcIfClass *ic = IBEX_CLOCK_SRC_IF_CLASS(klass);
    ic->get_clock_source = &ot_clkmgr_get_clock_source;
}

static const TypeInfo ot_clkmgr_info = {
    .name = TYPE_OT_CLKMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtClkMgrState),
    .instance_init = &ot_clkmgr_init,
    .class_size = sizeof(OtClkMgrClass),
    .class_init = &ot_clkmgr_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_IBEX_CLOCK_SRC_IF },
            {},
        },
};

static void ot_clkmgr_register_types(void)
{
    type_register_static(&ot_clkmgr_info);
}

type_init(ot_clkmgr_register_types);
