/*
 * QEMU Debug Transport Module
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
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
 * Generated code for absract commands has been extracted from the PULP Debug
 * module whose file (dm_mem.sv) contains the following copyright. This piece of
 * code is self contained within the `riscv_dtm_dm_access_register` function:
 *
 *   Copyright and related rights are licensed under the Solderpad Hardware
 *   License, Version 0.51 (the “License”); you may not use this file except in
 *   compliance with the License. You may obtain a copy of the License at
 *   http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
 *   or agreed to in writing, software, hardware and materials distributed under
 *   this License is distributed on an “AS IS” BASIS, WITHOUT WARRANTIES OR
 *   CONDITIONS OF ANY KIND, either express or implied. See the License for the
 *   specific language governing permissions and limitations under the License.
 */

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/jtag/tap_ctrl.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/debug.h"
#include "hw/riscv/dtm.h"
#include "sysemu/runstate.h"
#include "trace.h"

/*
 * Register definitions
 */

/* clang-format off */

/* Debug Module Interface and Control */
REG32(DTMCS, 0x10u)
    FIELD(DTMCS, VERSION, 0u, 4u)
    FIELD(DTMCS, ABITS, 4u, 6u)
    FIELD(DTMCS, DMISTAT, 10u, 2u)
    FIELD(DTMCS, IDLE, 12u, 3u)
    FIELD(DTMCS, DMIRESET, 16u, 1u)
    FIELD(DTMCS, DMIHARDRESET, 17u, 1u)
REG64(DMI, 0x11u)
    FIELD(DMI, OP, 0u, 2u)
    FIELD(DMI, DATA, 2u, 32u)
    FIELD(DMI, ADDRESS, 34u, 64u-34u) /* real width is a runtime property */

#define xtrace_riscv_dtm_error(_msg_) \
    trace_riscv_dtm_error(__func__, __LINE__, _msg_)
#define xtrace_riscv_dtm_info(_msg_, _val_) \
    trace_riscv_dtm_info(__func__, __LINE__, _msg_, _val_)

/*
 * DMI register operations
 * see RISC-V debug spec secttion 6.1.5 Debug Module Interface Access
 */
typedef enum {
    DMI_IGNORE,
    DMI_READ,
    DMI_WRITE,
    DMI_RESERVED,
} RISCVDMIOperation;

typedef QLIST_HEAD(, RISCVDebugModule) RISCVDebugModuleList;

typedef struct RISCVDebugModule {
    QLIST_ENTRY(RISCVDebugModule) entry;
    RISCVDebugDeviceState *dev;
    RISCVDebugDeviceClass *dc;
    uint32_t base;
    uint32_t size;
} RISCVDebugModule;

/** Debug Module Interface */
struct RISCVDTMState {
    DeviceState parent;

    RISCVDebugModuleList dms;
    RISCVDebugModule *last_dm; /* last selected DM */

    uint32_t address; /* last updated address */
    RISCVDebugResult dmistat; /* Operation result */
    bool cmd_busy; /* A command is being executed */

    /* properties */
    DeviceState *tap_ctrl;
    unsigned abits; /* address bit count */
};

/*
 * Forward declarations
 */

static void riscv_dtm_reset(DeviceState *dev);
static RISCVDebugModule* riscv_dtm_get_dm(RISCVDTMState *s, uint32_t addr);
static void riscv_dtm_sort_dms(RISCVDTMState *s);

static void riscv_dtm_tap_dtmcs_capture(TapDataHandler *tdh);
static void riscv_dtm_tap_dtmcs_update(TapDataHandler *tdh);
static void riscv_dtm_tap_dmi_capture(TapDataHandler *tdh);
static void riscv_dtm_tap_dmi_update(TapDataHandler *tdh);

/*
 * Constants
 */

#define RISCV_DEBUG_DMI_VERSION  1u /* RISC-V Debug spec 0.13.x & 1.0 */
#define RISCVDMI_DTMCS_IR        0x10u
#define RISCVDMI_DMI_IR          0x11u

static const TapDataHandler RISCVDMI_DTMCS = {
    .name = "dtmcs",
    .length = 32u,
    .value = RISCV_DEBUG_DMI_VERSION, /* abits updated at runtime */
    .capture = &riscv_dtm_tap_dtmcs_capture,
    .update = &riscv_dtm_tap_dtmcs_update,
};

static const TapDataHandler RISCVDMI_DMI = {
    .name = "dmi",
    /* data, op; abits updated at runtime */
    .length = R_DMI_OP_LENGTH + R_DMI_DATA_LENGTH,
    .capture = &riscv_dtm_tap_dmi_capture,
    .update = &riscv_dtm_tap_dmi_update,
};

#define MAKE_RUNSTATE_ENTRY(_ent_) [RUN_STATE_##_ent_] = stringify(_ent_)
static const char *RISCVDMI_RUNSTATE_NAMES[] = {
    /* clang-format off */
    MAKE_RUNSTATE_ENTRY(DEBUG),
    MAKE_RUNSTATE_ENTRY(INMIGRATE),
    MAKE_RUNSTATE_ENTRY(INTERNAL_ERROR),
    MAKE_RUNSTATE_ENTRY(IO_ERROR),
    MAKE_RUNSTATE_ENTRY(PAUSED),
    MAKE_RUNSTATE_ENTRY(POSTMIGRATE),
    MAKE_RUNSTATE_ENTRY(PRELAUNCH),
    MAKE_RUNSTATE_ENTRY(FINISH_MIGRATE),
    MAKE_RUNSTATE_ENTRY(RESTORE_VM),
    MAKE_RUNSTATE_ENTRY(RUNNING),
    MAKE_RUNSTATE_ENTRY(SAVE_VM),
    MAKE_RUNSTATE_ENTRY(SHUTDOWN),
    MAKE_RUNSTATE_ENTRY(SUSPENDED),
    MAKE_RUNSTATE_ENTRY(WATCHDOG),
    MAKE_RUNSTATE_ENTRY(GUEST_PANICKED),
    MAKE_RUNSTATE_ENTRY(COLO),
    /* clang-format on */
};
#undef MAKE_RUNSTATE_ENTRY
#define RUNSTATE_NAME(_reg_) \
    ((((_reg_) <= ARRAY_SIZE(RISCVDMI_RUNSTATE_NAMES)) && \
      RISCVDMI_RUNSTATE_NAMES[_reg_]) ? \
         RISCVDMI_RUNSTATE_NAMES[_reg_] : \
         "?")

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

bool riscv_dtm_register_dm(DeviceState *dev, RISCVDebugDeviceState *dbgdev,
                           hwaddr base_addr, hwaddr size)
{
    RISCVDTMState *s = RISCV_DTM(dev);

    g_assert(dev->realized);

    if ((base_addr + size - 1u) > (1u << s->abits)) {
        error_setg(&error_fatal,
                   "DM address range cannot be encoded in %u address bits",
                   s->abits);
    }

    if (!s->tap_ctrl) {
        xtrace_riscv_dtm_info("TAP controller not available", 0);
        return false;
    }

    TapCtrlIfClass *tapcls = TAP_CTRL_IF_GET_CLASS(s->tap_ctrl);
    TapCtrlIf *tap = TAP_CTRL_IF(s->tap_ctrl);

    /* may fail if TAP controller is not active */
    bool tap_ok = tapcls->is_enabled(tap);

    RISCVDebugModule *node;
    unsigned count = 0;
    QLIST_FOREACH(node, &s->dms, entry) {
        count += 1u;
        if ((node->dev == dbgdev) && (node->base == base_addr) &&
            (node->size == size)) {
            /* already registered */
            return tap_ok;
        }
        if (base_addr > (node->base + node->size - 1u)) {
            continue;
        }
        if ((base_addr + size - 1u) < node->base) {
            continue;
        }

        error_setg(&error_fatal, "Debug Module overlap\n");
    }

    object_ref(OBJECT(dbgdev));

    RISCVDebugModule *dm = g_new(RISCVDebugModule, 1u);
    dm->dev = dbgdev;
    dm->dc = RISCV_DEBUG_DEVICE_GET_CLASS(OBJECT(dbgdev));
    dm->base = base_addr;
    dm->size = size;

    QLIST_INSERT_HEAD(&s->dms, dm, entry);
    s->last_dm = dm;

    trace_riscv_dtm_register_dm(count, base_addr, base_addr + size - 1u,
                                tap_ok);

    riscv_dtm_sort_dms(s);

    return tap_ok;
}

/* -------------------------------------------------------------------------- */
/* DTMCS/DMI implementation */
/* -------------------------------------------------------------------------- */

static void riscv_dtm_tap_dtmcs_capture(TapDataHandler *tdh)
{
    RISCVDTMState *s = tdh->opaque;

    tdh->value = (s->abits << 4u) | (RISCV_DEBUG_DMI_VERSION << 0u) |
                 ((uint64_t)s->dmistat << 10u); /* see DMI op result */
}

static void riscv_dtm_tap_dtmcs_update(TapDataHandler *tdh)
{
    RISCVDTMState *s = tdh->opaque;
    if (tdh->value & (1u << 16u)) {
        /* dmireset */
        trace_riscv_dtm_dtmcs_reset();
        s->dmistat = RISCV_DEBUG_NOERR;
    }
    if (tdh->value & (1u << 17u)) {
        /* dmi hardreset */
        qemu_log_mask(LOG_UNIMP, "%s: DMI hard reset\n", __func__);
    }
}

static void riscv_dtm_tap_dmi_capture(TapDataHandler *tdh)
{
    RISCVDTMState *s = tdh->opaque;

    uint32_t addr = s->address;
    uint32_t value = 0;

    if (s->dmistat == RISCV_DEBUG_NOERR) {
        unsigned op = (unsigned)(tdh->value & 0b11);
        if (op == DMI_READ) {
            RISCVDebugModule *dm = riscv_dtm_get_dm(s, addr);
            if (!dm) {
                s->dmistat = RISCV_DEBUG_FAILED;
                value = 0;
                qemu_log_mask(LOG_UNIMP, "%s: Unknown DM address 0x%x\n",
                              __func__, addr);
            } else {
                value = dm->dc->read_value(dm->dev);
            }
        }
    }

    /*
     * In Capture-DR, the DTM updates data with the result from [the previous
     * update] operation, updating op if the current op isn’t sticky.
     */
    tdh->value = (((uint64_t)addr) << (32u + 2u)) | (((uint64_t)value) << 2u) |
                 ((uint64_t)(s->dmistat & 0b11));
}

static void riscv_dtm_tap_dmi_update(TapDataHandler *tdh)
{
    RISCVDTMState *s = tdh->opaque;

    uint32_t value;
    uint32_t addr =
        (uint32_t)extract64(tdh->value, R_DMI_ADDRESS_SHIFT, (int)s->abits);
    unsigned op = (unsigned)(tdh->value & 0b11);

    if (op == DMI_IGNORE) {
        /*
         * Don’t send anything over the DMI during Update-DR. This operation
         * should never result in a busy or error response. The address and data
         * reported in the following Capture-DR are undefined.
         */
        return;
    }

    /* store address for next read back */
    s->address = addr;

    RISCVDebugModule *dm = riscv_dtm_get_dm(s, addr);
    if (!dm) {
        s->dmistat = RISCV_DEBUG_FAILED;
        qemu_log_mask(LOG_UNIMP, "%s: Unknown DM address 0x%x, op %u\n",
                      __func__, addr, op);
        return;
    }

    /*
     * In Update-DR, the DTM starts the operation specified in op unless the
     * current status reported in op is sticky.
     */
    switch (op) {
    case DMI_IGNORE: /* NOP */
        g_assert_not_reached();
        return;
    case DMI_READ:
        s->dmistat = dm->dc->read_rq(dm->dev, addr - dm->base);
        break;
    case DMI_WRITE:
        value = (uint32_t)FIELD_EX64(tdh->value, DMI, DATA);
        s->dmistat = dm->dc->write_rq(dm->dev, addr - dm->base, value);
        break;
    case DMI_RESERVED:
    default:
        s->dmistat = RISCV_DEBUG_FAILED;
        qemu_log_mask(LOG_UNIMP, "%s: Unknown operation %u\n", __func__, op);
    }
}

static void riscv_dtm_register_tap_handlers(RISCVDTMState *s)
{
    if (!s->tap_ctrl) {
        return;
    }

    TapCtrlIfClass *tapcls = TAP_CTRL_IF_GET_CLASS(s->tap_ctrl);
    TapCtrlIf *tap = TAP_CTRL_IF(s->tap_ctrl);

    /*
     * copy the template to update the opaque value
     * no lifetime issue as the data handler is copied by the TAP controller
     */
    TapDataHandler tdh;

    memcpy(&tdh, &RISCVDMI_DTMCS, sizeof(TapDataHandler));
    tdh.value |= s->abits << 4u; /* add address bit count */
    tdh.opaque = s;
    if (tapcls->register_instruction(tap, RISCVDMI_DTMCS_IR, &tdh)) {
        xtrace_riscv_dtm_error("cannot register DMTCS");
        return;
    }

    memcpy(&tdh, &RISCVDMI_DMI, sizeof(TapDataHandler));
    tdh.length += s->abits; /* add address bit count */
    tdh.opaque = s;
    /* the data handler is copied by the TAP controller */
    if (tapcls->register_instruction(tap, RISCVDMI_DMI_IR, &tdh)) {
        xtrace_riscv_dtm_error("cannot register DMI");
        return;
    }
}

static RISCVDebugModule *riscv_dtm_get_dm(RISCVDTMState *s, uint32_t addr)
{
    RISCVDebugModule *dm = s->last_dm;

    if (dm && (addr >= dm->base) && (addr < dm->base + dm->size)) {
        return dm;
    }

    QLIST_FOREACH(dm, &s->dms, entry) {
        if ((addr >= dm->base) && (addr < dm->base + dm->size)) {
            s->last_dm = dm;
            return dm;
        }
    }

    s->last_dm = NULL;
    return NULL;
}

static void riscv_dtm_vm_state_change(void *opaque, bool running,
                                      RunState state)
{
    (void)opaque;
    (void)running;
    trace_riscv_dtm_vm_state_change(RUNSTATE_NAME(state), state);
}

static int riscv_dtm_order_dm(const void *pdm1, const void *pdm2)
{
    return ((int)(*(RISCVDebugModule **)pdm1)->base) -
           ((int)(*(RISCVDebugModule **)pdm2)->base);
}

static void riscv_dtm_sort_dms(RISCVDTMState *s)
{
    RISCVDebugModule *dm;

    /* iterate once to get the count of managed DMs */
    unsigned count = 0;
    QLIST_FOREACH(dm, &s->dms, entry) {
        count += 1;
    }

    /* create an array of sortable DM references */
    RISCVDebugModule **dma = g_new(RISCVDebugModule *, count);
    RISCVDebugModule **cdm = dma;
    QLIST_FOREACH(dm, &s->dms, entry) {
        *cdm++ = dm;
    }

    /* sort DM reference by increasing base address */
    qsort(dma, count, sizeof(RISCVDebugModule *), &riscv_dtm_order_dm);

    /* create a new list of ordered, managed DMs */
    RISCVDebugModuleList sorted;
    QLIST_INIT(&sorted);
    RISCVDebugModule *cur = QLIST_FIRST(&sorted);
    for (unsigned ix = 0; ix < count; ix++) {
        QLIST_REMOVE(dma[ix], entry);
        if (!cur) {
            QLIST_INSERT_HEAD(&sorted, dma[ix], entry);
        } else {
            QLIST_INSERT_AFTER(cur, dma[ix], entry);
        }
        cur = dma[ix];
    }
    /* replace current list head with the new ordered one */
    s->dms = sorted;

    g_free(dma);
}

static Property riscv_dtm_properties[] = {
    DEFINE_PROP_UINT32("abits", RISCVDTMState, abits, 0x7u),
    DEFINE_PROP_LINK("tap_ctrl", RISCVDTMState, tap_ctrl, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void riscv_dtm_reset(DeviceState *dev)
{
    RISCVDTMState *s = RISCV_DTM(dev);

    s->address = 0;
    s->last_dm = NULL;
}

static void riscv_dtm_realize(DeviceState *dev, Error **errp)
{
    RISCVDTMState *s = RISCV_DTM(dev);

    if (s->abits < 7u || s->abits > 30u) {
        error_setg(errp, "Invalid address bit count");
        return;
    }

    riscv_dtm_register_tap_handlers(s);
}

static void riscv_dtm_init(Object *obj)
{
    RISCVDTMState *s = RISCV_DTM(obj);

    qemu_add_vm_change_state_handler(&riscv_dtm_vm_state_change, s);

    QLIST_INIT(&s->dms);
}

static void riscv_dtm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &riscv_dtm_reset;
    dc->realize = &riscv_dtm_realize;
    device_class_set_props(dc, riscv_dtm_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo riscv_dtm_info = {
    .name = TYPE_RISCV_DTM,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(RISCVDTMState),
    .instance_init = &riscv_dtm_init,
    .class_init = &riscv_dtm_class_init,
};

static void riscv_dtm_register_types(void)
{
    type_register_static(&riscv_dtm_info);
}

type_init(riscv_dtm_register_types);
