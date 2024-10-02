/*
 * QEMU JTAG TAP controller for the OpenOCD/Spike Remote BigBang protocol
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *
 * For details check the documentation here:
 *    https://github.com/openocd-org/openocd/blob/master/
 *       doc/manual/jtag/drivers/remote_bitbang.txt
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
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "chardev/char-fe.h"
#include "chardev/char.h"
#include "hw/jtag/tap_ctrl.h"
#include "hw/jtag/tap_ctrl_rbb.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "sysemu/runstate.h"
#include "trace.h"


typedef enum {
    TEST_LOGIC_RESET,
    RUN_TEST_IDLE,
    SELECT_DR_SCAN,
    CAPTURE_DR,
    SHIFT_DR,
    EXIT1_DR,
    PAUSE_DR,
    EXIT2_DR,
    UPDATE_DR,
    SELECT_IR_SCAN,
    CAPTURE_IR,
    SHIFT_IR,
    EXIT1_IR,
    PAUSE_IR,
    EXIT2_IR,
    UPDATE_IR,
    _TAP_STATE_COUNT
} TAPState;

typedef TapDataHandler *tap_ctrl_data_reg_extender_t(uint64_t value);

typedef struct TapCtrlRbbState {
    DeviceState parent;

    TAPState state; /* Current state */

    /* signals */
    bool trst; /* TAP controller reset */
    bool srst; /* System reset */
    bool tck; /* JTAG clock */
    bool tms; /* JTAG state machine selector */
    bool tdi; /* Register input */
    bool tdo; /* Register output */

    /* registers */
    uint64_t ir; /* instruction register value */
    uint64_t ir_hold; /* IR hold register */
    uint64_t dr; /* current data register value */
    size_t dr_len; /* count of meaningful bits in dr */

    /* handlers */
    TapDataHandler *tdh; /* Current data register handler */
    GHashTable *tdhtable; /* Registered handlers */

    guint watch_tag; /* tracker for comm device change */

    /* properties */
    CharBackend chr;
    uint32_t idcode; /* TAP controller identifier */
    uint8_t ir_length; /* count of meaningful bits in ir */
    uint8_t idcode_inst; /* instruction to get ID code */
    bool enable_quit; /* whether VM quit can be remotely triggered */
} TapCtrlRbbState;

typedef struct _TAPRegisterState {
    int base_reg;
    int num_regs;
    struct TAPRegisterState *next;
} TAPRegisterState;

typedef struct _TAPProcess {
    uint32_t pid;
    bool attached;
} TAPProcess;

#define STRINGIFY_(_val_)   #_val_
#define STRINGIFY(_val_)    STRINGIFY_(_val_)
#define NAME_FSMSTATE(_st_) [_st_] = STRINGIFY(_st_)

#define DEFAULT_JTAG_BITBANG_PORT "3335"
#define MAX_PACKET_LENGTH         4096u

#define TAP_CTRL_BYPASS_INST 0u

/*
 * TAP controller state machine state/event matrix
 *
 * Current state -> Next States for either TMS == 0 or TMS == 1
 */
static const TAPState TAPFSM[_TAP_STATE_COUNT][2] = {
    [TEST_LOGIC_RESET] = { RUN_TEST_IDLE, TEST_LOGIC_RESET },
    [RUN_TEST_IDLE] = { RUN_TEST_IDLE, SELECT_DR_SCAN },
    [SELECT_DR_SCAN] = { CAPTURE_DR, SELECT_IR_SCAN },
    [CAPTURE_DR] = { SHIFT_DR, EXIT1_DR },
    [SHIFT_DR] = { SHIFT_DR, EXIT1_DR },
    [EXIT1_DR] = { PAUSE_DR, UPDATE_DR },
    [PAUSE_DR] = { PAUSE_DR, EXIT2_DR },
    [EXIT2_DR] = { SHIFT_DR, UPDATE_DR },
    [UPDATE_DR] = { RUN_TEST_IDLE, SELECT_DR_SCAN },
    [SELECT_IR_SCAN] = { CAPTURE_IR, TEST_LOGIC_RESET },
    [CAPTURE_IR] = { SHIFT_IR, EXIT1_IR },
    [SHIFT_IR] = { SHIFT_IR, EXIT1_IR },
    [EXIT1_IR] = { PAUSE_IR, UPDATE_IR },
    [PAUSE_IR] = { PAUSE_IR, EXIT2_IR },
    [EXIT2_IR] = { SHIFT_IR, UPDATE_IR },
    [UPDATE_IR] = { RUN_TEST_IDLE, SELECT_DR_SCAN }
};

static const char TAPFSM_NAMES[_TAP_STATE_COUNT][18U] = {
    NAME_FSMSTATE(TEST_LOGIC_RESET), NAME_FSMSTATE(RUN_TEST_IDLE),
    NAME_FSMSTATE(SELECT_DR_SCAN),   NAME_FSMSTATE(CAPTURE_DR),
    NAME_FSMSTATE(SHIFT_DR),         NAME_FSMSTATE(EXIT1_DR),
    NAME_FSMSTATE(PAUSE_DR),         NAME_FSMSTATE(EXIT2_DR),
    NAME_FSMSTATE(UPDATE_DR),        NAME_FSMSTATE(SELECT_IR_SCAN),
    NAME_FSMSTATE(CAPTURE_IR),       NAME_FSMSTATE(SHIFT_IR),
    NAME_FSMSTATE(EXIT1_IR),         NAME_FSMSTATE(PAUSE_IR),
    NAME_FSMSTATE(EXIT2_IR),         NAME_FSMSTATE(UPDATE_IR),
};

static void tap_ctrl_rbb_idcode_capture(TapDataHandler *tdh);

/* Common TAP instructions */
static const TapDataHandler TAP_CTRL_RBB_BYPASS = {
    .name = "bypass",
    .length = 1,
    .value = 0,
};

static const TapDataHandler TAP_CTRL_RBB_IDCODE = {
    .name = "idcode",
    .length = 32,
    .capture = &tap_ctrl_rbb_idcode_capture,
};

/*
 * TAP State Machine implementation
 */

static void tap_ctrl_rbb_dump_register(const char *msg, const char *iname,
                                       uint64_t value, size_t length)
{
    char buf[80];
    if (length > 64u) {
        length = 64u;
    }
    unsigned ix = 0;
    while (ix < length) {
        buf[ix] = (char)('0' + ((value >> (length - ix - 1)) & 0b1));
        ix++;
    }
    buf[ix] = '\0';

    if (iname) {
        trace_tap_ctrl_rbb_idump_register(msg, iname, value, length, buf);
    } else {
        trace_tap_ctrl_rbb_dump_register(msg, value, length, buf);
    }
}

static bool tap_ctrl_rbb_has_data_handler(TapCtrlRbbState *tap, unsigned code)
{
    return (bool)g_hash_table_contains(tap->tdhtable, GINT_TO_POINTER(code));
}

static TapDataHandler *
tap_ctrl_rbb_get_data_handler(TapCtrlRbbState *tap, unsigned code)
{
    TapDataHandler *tdh;
    tdh = (TapDataHandler *)g_hash_table_lookup(tap->tdhtable,
                                                GINT_TO_POINTER(code));
    return tdh;
}

static void tap_ctrl_rbb_idcode_capture(TapDataHandler *tdh)
{
    /* special case for ID code: opaque contains the ID code value */
    tdh->value = (uint64_t)(uintptr_t)tdh->opaque;
}

static void tap_ctrl_rbb_tap_reset(TapCtrlRbbState *tap)
{
    tap->state = TEST_LOGIC_RESET;
    tap->trst = false;
    tap->srst = false;
    tap->tck = false;
    tap->tms = false;
    tap->tdi = false;
    tap->tdo = false;
    tap->ir = tap->idcode_inst;
    tap->ir_hold = tap->idcode_inst;
    tap->dr = 0u;
    tap->dr_len = 0u;
    tap->tdh = tap_ctrl_rbb_get_data_handler(tap, tap->idcode_inst);
    g_assert(tap->tdh);
}

static void tap_ctrl_rbb_system_reset(TapCtrlRbbState *tap)
{
    Object *mc = qdev_get_machine();
    ObjectClass *oc = object_get_class(mc);
    (void)tap;

    if (!object_class_dynamic_cast(oc, TYPE_RESETTABLE_INTERFACE)) {
        qemu_log_mask(LOG_UNIMP, "%s: Machine %s is not resettable\n", __func__,
                      object_get_typename(mc));
        return;
    }

    trace_tap_ctrl_rbb_system_reset();
    resettable_reset(mc, RESET_TYPE_COLD);
}

static TAPState tap_ctrl_rbb_get_next_state(TapCtrlRbbState *tap, bool tms)
{
    tap->state = TAPFSM[tap->state][(unsigned)tms];
    return tap->state;
}

static void tap_ctrl_rbb_capture_ir(TapCtrlRbbState *tap)
{
    tap->ir = tap->idcode_inst;
}

static void tap_ctrl_rbb_shift_ir(TapCtrlRbbState *tap, bool tdi)
{
    tap->ir >>= 1u;
    tap->ir |= ((uint64_t)(tdi)) << (tap->ir_length - 1u);
}

static void tap_ctrl_rbb_update_ir(TapCtrlRbbState *tap)
{
    tap->ir_hold = tap->ir;
    tap_ctrl_rbb_dump_register("Update IR", NULL, tap->ir_hold, tap->ir_length);
}

static void tap_ctrl_rbb_capture_dr(TapCtrlRbbState *tap)
{
    TapDataHandler *prev = tap->tdh;

    if (tap->ir_hold >= (1 << tap->ir_length)) {
        /* internal error, should never happen */
        error_setg(&error_fatal, "Invalid IR 0x%02x\n", (unsigned)tap->ir_hold);
        g_assert_not_reached();
    }

    TapDataHandler *tdh = tap_ctrl_rbb_get_data_handler(tap, tap->ir_hold);
    if (!tdh) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Unknown IR 0x%02x\n", __func__,
                      (unsigned)tap->ir_hold);
        tap->dr = 0;
        return;
    }

    if (tdh != prev) {
        trace_tap_ctrl_rbb_select_dr(tdh->name, tap->ir_hold);
    }

    tap->tdh = tdh;
    tap->dr_len = tdh->length;

    if (tdh->capture) {
        tdh->capture(tdh);
    }
    tap->dr = tdh->value;
    tap_ctrl_rbb_dump_register("Capture DR", tap->tdh->name, tap->dr,
                               tap->dr_len);
}

static void tap_ctrl_rbb_shift_dr(TapCtrlRbbState *tap, bool tdi)
{
    tap->dr >>= 1u;
    tap->dr |= ((uint64_t)(tdi)) << (tap->dr_len - 1u);
}

static void tap_ctrl_rbb_update_dr(TapCtrlRbbState *tap)
{
    tap_ctrl_rbb_dump_register("Update DR", tap->tdh->name, tap->dr,
                               tap->dr_len);
    TapDataHandler *tdh = tap->tdh;
    tdh->value = tap->dr;
    if (tdh->update) {
        tdh->update(tdh);
    }
}

static void tap_ctrl_rbb_step(TapCtrlRbbState *tap, bool tck, bool tms,
                              bool tdi)
{
    trace_tap_ctrl_rbb_step(tck, tms, tdi);

    if (tap->trst) {
        return;
    }

    if (!tap->tck && tck) {
        /* Rising clock edge */
        if (tap->state == SHIFT_IR) {
            tap_ctrl_rbb_shift_ir(tap, tap->tdi);
        } else if (tap->state == SHIFT_DR) {
            tap_ctrl_rbb_shift_dr(tap, tap->tdi);
        }
        TAPState prev = tap->state;
        TAPState new = tap_ctrl_rbb_get_next_state(tap, tms);
        if (prev != new) {
            trace_tap_ctrl_rbb_change_state(TAPFSM_NAMES[prev],
                                            TAPFSM_NAMES[new]);
        }
    } else {
        /* Falling clock edge */
        switch (tap->state) {
        case RUN_TEST_IDLE:
            /* do nothing */
            break;
        case TEST_LOGIC_RESET:
            tap_ctrl_rbb_tap_reset(tap);
            break;
        case CAPTURE_DR:
            tap_ctrl_rbb_capture_dr(tap);
            break;
        case SHIFT_DR:
            tap->tdo = tap->dr & 0b1;
            break;
        case UPDATE_DR:
            tap_ctrl_rbb_update_dr(tap);
            break;
        case CAPTURE_IR:
            tap_ctrl_rbb_capture_ir(tap);
            break;
        case SHIFT_IR:
            tap->tdo = tap->ir & 0b1;
            break;
        case UPDATE_IR:
            tap_ctrl_rbb_update_ir(tap);
            break;
        default:
            /* nothing to do on the other state transition */
            break;
        }
    }
    tap->tck = tck;
    tap->tdi = tdi;
    tap->tms = tms;
}

static void tap_ctrl_rbb_blink(TapCtrlRbbState *tap, bool light) {}

static void tap_ctrl_rbb_read(TapCtrlRbbState *tap)
{
    trace_tap_ctrl_rbb_read(tap->tdo);
}

static void tap_ctrl_rbb_quit(TapCtrlRbbState *tap)
{
    (void)tap;

    if (tap->enable_quit) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else {
        info_report("%s: JTAG termination disabled\n", __func__);
    }
}

static void tap_ctrl_rbb_write(TapCtrlRbbState *tap, bool tck, bool tms,
                               bool tdi)
{
    tap_ctrl_rbb_step(tap, tck, tms, tdi);
}

static void tap_ctrl_rbb_reset_tap(TapCtrlRbbState *tap, bool trst, bool srst)
{
    trace_tap_ctrl_rbb_reset(trst, srst);
    if (trst) {
        tap_ctrl_rbb_tap_reset(tap);
    }
    if (srst) {
        tap_ctrl_rbb_system_reset(tap);
    }
    tap->trst = trst;
    tap->srst = srst;
}

/*
 * TAP Server implementation
 */

static bool tap_ctrl_rbb_read_byte(TapCtrlRbbState *tap, uint8_t ch)
{
    switch ((char)ch) {
    case 'B':
        tap_ctrl_rbb_blink(tap, true);
        break;
    case 'b':
        tap_ctrl_rbb_blink(tap, false);
        break;
    case 'R':
        tap_ctrl_rbb_read(tap);
        break;
    case 'Q':
        tap_ctrl_rbb_quit(tap);
        break;
    case '0':
        tap_ctrl_rbb_write(tap, false, false, false);
        break;
    case '1':
        tap_ctrl_rbb_write(tap, false, false, true);
        break;
    case '2':
        tap_ctrl_rbb_write(tap, false, true, false);
        break;
    case '3':
        tap_ctrl_rbb_write(tap, false, true, true);
        break;
    case '4':
        tap_ctrl_rbb_write(tap, true, false, false);
        break;
    case '5':
        tap_ctrl_rbb_write(tap, true, false, true);
        break;
    case '6':
        tap_ctrl_rbb_write(tap, true, true, false);
        break;
    case '7':
        tap_ctrl_rbb_write(tap, true, true, true);
        break;
    case 'r':
        tap_ctrl_rbb_reset_tap(tap, false, false);
        break;
    case 's':
        tap_ctrl_rbb_reset_tap(tap, false, true);
        break;
    case 't':
        tap_ctrl_rbb_reset_tap(tap, true, false);
        break;
    case 'u':
        tap_ctrl_rbb_reset_tap(tap, true, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Unknown TAP code 0x%02x\n", __func__,
                      (unsigned)ch);
        break;
    }

    /* true if TDO level should be sent to the peer */
    return (int)ch == 'R';
}

static int tap_ctrl_rbb_chr_can_receive(void *opaque)
{
    TapCtrlRbbState *tap = (TapCtrlRbbState *)opaque;

    /* do not accept any input till a TAP controller is available */
    return qemu_chr_fe_backend_connected(&tap->chr) ? MAX_PACKET_LENGTH : 0;
}

static void tap_ctrl_rbb_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    TapCtrlRbbState *tap = (TapCtrlRbbState *)opaque;

    for (unsigned ix = 0; ix < size; ix++) {
        if (tap_ctrl_rbb_read_byte(tap, buf[ix])) {
            uint8_t outbuf[1] = { '0' + (unsigned)tap->tdo };
            qemu_chr_fe_write_all(&tap->chr, outbuf, (int)sizeof(outbuf));
        }
    }
}

static void tap_ctrl_rbb_chr_event_hander(void *opaque, QEMUChrEvent event)
{
    TapCtrlRbbState *tap = opaque;

    if (event == CHR_EVENT_OPENED) {
        if (!qemu_chr_fe_backend_connected(&tap->chr)) {
            return;
        }

        tap_ctrl_rbb_tap_reset(tap);
    }
}

static gboolean
tap_ctrl_rbb_chr_watch_cb(void *do_not_use, GIOCondition cond, void *opaque)
{
    TapCtrlRbbState *tap = opaque;
    (void)do_not_use;
    (void)cond;

    tap->watch_tag = 0;

    return FALSE;
}

static int tap_ctrl_rbb_chr_be_change(void *opaque)
{
    TapCtrlRbbState *tap = opaque;

    qemu_chr_fe_set_handlers(&tap->chr, &tap_ctrl_rbb_chr_can_receive,
                             &tap_ctrl_rbb_chr_receive,
                             &tap_ctrl_rbb_chr_event_hander,
                             &tap_ctrl_rbb_chr_be_change, tap, NULL, true);

    tap_ctrl_rbb_tap_reset(tap);

    if (tap->watch_tag > 0) {
        g_source_remove(tap->watch_tag);
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        tap->watch_tag = qemu_chr_fe_add_watch(&tap->chr, G_IO_OUT | G_IO_HUP,
                                               &tap_ctrl_rbb_chr_watch_cb, tap);
    }

    return 0;
}

static void tap_ctrl_rbb_verify_handler(const TapCtrlRbbState *tap,
                                        unsigned code, const char *name)
{
    if (code >= (1 << tap->ir_length)) {
        error_setg(&error_fatal, "JTAG: Invalid IR code: 0x%x for %s", code,
                   name);
        g_assert_not_reached();
    }
}

static void
tap_ctrl_rbb_verify_handler_fn(gpointer key, gpointer value, gpointer user_data)
{
    unsigned code = GPOINTER_TO_INT(key);
    const TapDataHandler *tdh = (const TapDataHandler *)value;
    const TapCtrlRbbState *tap = (const TapCtrlRbbState *)user_data;

    tap_ctrl_rbb_verify_handler(tap, code, tdh->name);
}

static void tap_ctrl_rbb_register_handler(TapCtrlRbbState *tap, unsigned code,
                                          const TapDataHandler *tdh, bool check)
{
    if (check) {
        tap_ctrl_rbb_verify_handler(tap, code, tdh->name);
    }

    if (tap_ctrl_rbb_has_data_handler(tap, code)) {
        warn_report("JTAG: IR code already registered: 0x%x", code);
        /* resume and override */
    }

    TapDataHandler *ltdh = g_new0(TapDataHandler, 1u);
    memcpy(ltdh, tdh, sizeof(*tdh));
    ltdh->name = g_strdup(tdh->name);
    g_hash_table_insert(tap->tdhtable, GINT_TO_POINTER(code), ltdh);

    trace_tap_ctrl_rbb_register(code, ltdh->name);
}

static void tap_ctrl_rbb_free_data_handler(gpointer entry)
{
    TapDataHandler *tdh = entry;
    if (!entry) {
        return;
    }
    g_free((char *)tdh->name);
    g_free(tdh);
}

static bool tap_ctrl_rbb_is_enabled(TapCtrlIf *dev)
{
    TapCtrlRbbState *tap = TAP_CTRL_RBB(dev);

    return qemu_chr_fe_backend_connected(&tap->chr);
}

static int tap_ctrl_rbb_register_instruction(TapCtrlIf *dev, unsigned code,
                                             const TapDataHandler *tdh)
{
    TapCtrlRbbState *tap = TAP_CTRL_RBB(dev);

    tap_ctrl_rbb_register_handler(tap, code, tdh, DEVICE(tap)->realized);

    return 0;
}

static Property tap_ctrl_rbb_properties[] = {
    DEFINE_PROP_UINT32("idcode", TapCtrlRbbState, idcode, 0),
    DEFINE_PROP_UINT8("ir_length", TapCtrlRbbState, ir_length, 0),
    DEFINE_PROP_UINT8("idcode_inst", TapCtrlRbbState, idcode_inst, 1u),
    DEFINE_PROP_BOOL("quit", TapCtrlRbbState, enable_quit, true),
    DEFINE_PROP_CHR("chardev", TapCtrlRbbState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void tap_ctrl_rbb_realize(DeviceState *dev, Error **errp)
{
    TapCtrlRbbState *tap = TAP_CTRL_RBB(dev);

    if (tap->ir_length == 0 || tap->ir_length > 8u) {
        error_setg(errp, "Unsupported IR length: %u", tap->ir_length);
        return;
    }

    if (tap->idcode == 0u) {
        error_setg(errp, "Invalid IDCODE: 0x%x", tap->idcode);
        return;
    }

    if (tap->idcode_inst == TAP_CTRL_BYPASS_INST) {
        error_setg(errp, "Invalid IDCODE instruction: 0x%x", tap->idcode_inst);
        return;
    }

    trace_tap_ctrl_rbb_realize(tap->ir_length, tap->idcode);

    /*
     * Handlers may be registered before the TAP controller is configured.
     * Need to check their configuration once the configuration is known
     */
    g_hash_table_foreach(tap->tdhtable, &tap_ctrl_rbb_verify_handler_fn,
                         (gpointer)tap);

    size_t irslots = 1u << tap->ir_length;
    tap_ctrl_rbb_register_handler(tap, TAP_CTRL_BYPASS_INST,
                                  &TAP_CTRL_RBB_BYPASS, true);
    tap_ctrl_rbb_register_handler(tap, tap->idcode_inst, &TAP_CTRL_RBB_IDCODE,
                                  true);
    tap_ctrl_rbb_register_handler(tap, irslots - 1u, &TAP_CTRL_RBB_BYPASS,
                                  true);
    /* special case for ID code: opaque store the constant idcode value */
    TapDataHandler *tdh = tap_ctrl_rbb_get_data_handler(tap, tap->idcode_inst);
    g_assert(tdh);
    tdh->opaque = (void *)(uintptr_t)tap->idcode;

    qemu_chr_fe_set_handlers(&tap->chr, &tap_ctrl_rbb_chr_can_receive,
                             &tap_ctrl_rbb_chr_receive,
                             &tap_ctrl_rbb_chr_event_hander,
                             &tap_ctrl_rbb_chr_be_change, tap, NULL, true);

    tap_ctrl_rbb_tap_reset(tap);

    qemu_chr_fe_accept_input(&tap->chr);
}

static void tap_ctrl_rbb_init(Object *obj)
{
    TapCtrlRbbState *tap = TAP_CTRL_RBB(obj);

    tap->tdhtable = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                          tap_ctrl_rbb_free_data_handler);
}

static void tap_ctrl_rbb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &tap_ctrl_rbb_realize;
    device_class_set_props(dc, tap_ctrl_rbb_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    TapCtrlIfClass *tcc = TAP_CTRL_IF_CLASS(klass);
    tcc->is_enabled = &tap_ctrl_rbb_is_enabled;
    tcc->register_instruction = &tap_ctrl_rbb_register_instruction;
}

static const TypeInfo tap_ctrl_rbb_info = {
    .name = TYPE_TAP_CTRL_RBB,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TapCtrlRbbState),
    .instance_init = &tap_ctrl_rbb_init,
    .class_init = &tap_ctrl_rbb_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { TYPE_TAP_CTRL_IF },
            {},
        },
};

static void register_types(void)
{
    type_register_static(&tap_ctrl_rbb_info);
}

type_init(register_types);
