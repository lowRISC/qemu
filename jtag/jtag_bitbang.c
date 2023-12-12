/*
 * QEMU OpenTitan JTAG TAP controller
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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
#include "qemu/ctype.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "chardev/char.h"
#include "exec/gdbstub.h"
#include "exec/hwaddr.h"
#include "exec/jtagstub.h"
#include "hw/boards.h"
#include "hw/cpu/cluster.h"
#include "monitor/monitor.h"
#include "semihosting/semihost.h"
#include "sysemu/hw_accel.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "trace.h"


/*
 * Type definitions
 */

typedef enum _TAPState {
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

typedef TAPDataHandler *tapctrl_data_reg_extender_t(uint64_t value);

typedef struct _TAPController {
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
    size_t ir_len; /* count of meaningful bits in ir */
    uint64_t ir_hold; /* IR hold register */
    uint64_t dr; /* current data register value */
    size_t dr_len; /* count of meaningful bits in dr */
    /* handlers */
    TAPDataHandler *tdh; /* Current data register handler */
    TAPDataHandler *tdhs; /* Registered handlers */
    /* buffer */
    uint8_t *outbuf; /* TDO output */
    size_t outpos; /* Meaningful count of bytes in outbuf */
} TAPController;

typedef struct _TAPRegisterState {
    int base_reg;
    int num_regs;
    struct TAPRegisterState *next;
} TAPRegisterState;

typedef struct _TAPProcess {
    uint32_t pid;
    bool attached;
} TAPProcess;

enum RSState {
    RS_INACTIVE,
    RS_IDLE,
};

typedef struct _TAPServerState {
    TAPController *tap;
    CharBackend chr;
    bool init; /* have we been initialised? */
} TAPServerState;

/*
 * Macros
 */

#define STRINGIFY_(_val_)   #_val_
#define STRINGIFY(_val_)    STRINGIFY_(_val_)
#define NAME_FSMSTATE(_st_) [_st_] = STRINGIFY(_st_)

/*
 * Constants
 */

#define DEFAULT_JTAG_BITBANG_PORT "3335"
#define MAX_PACKET_LENGTH         4096u

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

/* Common TAP instructions */
static const TAPDataHandler tapctrl_bypass = {
    .name = "bypass",
    .length = 1,
    .value = 0,
};
static const TAPDataHandler tapctrl_idcode = {
    .name = "idcode",
    .length = 32,
};

/*
 * Variables
 */

/* Unique instance of the TAP server */
static TAPServerState tapserver_state;

/*
 * TAP State Machine implementation
 */

static void tapctrl_dump_register(const char *msg, uint64_t value,
                                  size_t length)
{
    char buf[80];
    if (length > 64u) {
        length = 64u;
    }
    unsigned ix = 0;
    while (ix < length) {
        buf[ix] = '0' + ((value >> (length - ix - 1)) & 0b1);
        ix++;
    }
    buf[ix] = '\0';

    trace_jtag_tapctr_dump_register(msg, value, length, buf);
}

static void tapctrl_reset(TAPController *tap)
{
    tap->state = TEST_LOGIC_RESET;
    tap->trst = false;
    tap->srst = false;
    tap->tck = false;
    tap->tms = false;
    tap->tdi = false;
    tap->tdo = false;
    tap->ir = 0b01;
    tap->ir_hold = 0b01;
    tap->dr = 0u;
    tap->dr_len = 0u;
    tap->tdh = &tap->tdhs[tap->ir];
    tap->outpos = 0u;
}

static void tapctrl_register_handler(TAPController *tap, unsigned code,
                                     const TAPDataHandler *tdh)
{
    memcpy(&tap->tdhs[code], tdh, sizeof(*tdh));
    if (code >= (1 << tap->ir_len)) {
        error_setg(&error_fatal, "JTAG: Invalid IR code: 0x%x", code);
    }
    tap->tdhs[code].name = g_strdup(tdh->name);
    trace_jtag_tapctrl_register(code, tap->tdhs[code].name);
}

static void tapctrl_init(TAPController *tap, size_t irlength, uint32_t idcode)
{
    tap->ir_len = irlength;
    tapctrl_reset(tap);
    if (!tap->outbuf) {
        tap->outbuf = g_new0(uint8_t, MAX_PACKET_LENGTH);
        tap->outpos = 0;
    }
    if (!tap->tdhs) {
        size_t irslots = 1u << irlength;
        tap->tdhs = g_new0(TAPDataHandler, irslots);
        tapctrl_register_handler(tap, 0x00, &tapctrl_bypass);
        tapctrl_register_handler(tap, 0x01, &tapctrl_idcode);
        tapctrl_register_handler(tap, irslots - 1u, &tapctrl_bypass);
        tap->tdhs[0x01].value = idcode;
    }
}

static void tapctrl_deinit(TAPController *tap)
{
    g_free(tap->outbuf);
    tap->outbuf = NULL;
    if (tap->tdhs) {
        unsigned irslots = 1 << tap->ir_len;
        for (unsigned ix = 0; ix < irslots; ix++) {
            if (tap->tdhs[ix].name) {
                g_free((char *)tap->tdhs[ix].name);
                tap->tdhs[ix].name = NULL;
            }
        }
    }
    g_free(tap->tdhs);
    tap->tdhs = NULL;
}

static TAPState tapctrl_get_next_state(TAPController *tap, bool tms)
{
    tap->state = TAPFSM[tap->state][(unsigned)tms];
    return tap->state;
}

static void tapctrl_capture_ir(TAPController *tap)
{
    tap->ir = 0b01;
}

static void tapctrl_shift_ir(TAPController *tap, bool tdi)
{
    tap->ir >>= 1u;
    tap->ir |= ((uint64_t)(tdi)) << (tap->ir_len - 1u);
}

static void tapctrl_update_ir(TAPController *tap)
{
    tap->ir_hold = tap->ir;
    tapctrl_dump_register("Update IR", tap->ir_hold, tap->ir_len);
}

static void tapctrl_capture_dr(TAPController *tap, uint64_t value)
{
    TAPDataHandler *prev = tap->tdh;

    if (value >= (1 << tap->ir_len)) {
        qemu_log_mask(LOG_UNIMP, "%s: Invalid IR 0x%02x\n", __func__,
                      (unsigned)value);
        abort();
        return;
    }

    TAPDataHandler *tdh = &tap->tdhs[value];
    if (!tdh->length) {
        qemu_log_mask(LOG_UNIMP, "%s: Unknown IR 0x%02x\n", __func__,
                      (unsigned)value);
        abort();
        return;
    }

    if (tdh != prev) {
        trace_jtag_tapctr_select_dr(tdh->name, value);
    }

    tap->tdh = tdh;
    tap->dr_len = tdh->length;

    if (tdh->capture) {
        tdh->capture(tdh);
    }
    tap->dr = tdh->value;
    tapctrl_dump_register("Capture DR", tap->dr, tap->dr_len);
}

static void tapctrl_shift_dr(TAPController *tap, bool tdi)
{
    tap->dr >>= 1u;
    tap->dr |= ((uint64_t)(tdi)) << (tap->dr_len - 1u);
}

static void tapctrl_update_dr(TAPController *tap)
{
    tapctrl_dump_register("Update DR", tap->dr, tap->dr_len);
    TAPDataHandler *tdh = tap->tdh;
    tdh->value = tap->dr;
    if (tdh->update) {
        tdh->update(tdh);
    }
}

static void tapctrl_step(TAPController *tap, bool tck, bool tms, bool tdi)
{
    trace_jtag_tapctrl_step(tck, tms, tdi);

    if (tap->trst) {
        return;
    }

    if (!tap->tck && tck) {
        /* Rising clock edge */
        if (tap->state == SHIFT_IR) {
            tapctrl_shift_ir(tap, tap->tdi);
        } else if (tap->state == SHIFT_DR) {
            tapctrl_shift_dr(tap, tap->tdi);
        }
        TAPState prev = tap->state;
        TAPState new = tapctrl_get_next_state(tap, tms);
        if (prev != new) {
            trace_jtag_tapctrl_change_state(TAPFSM_NAMES[prev],
                                            TAPFSM_NAMES[new]);
        }
    } else {
        /* Falling clock edge */
        switch (tap->state) {
        case RUN_TEST_IDLE:
            /* do nothing */
            break;
        case TEST_LOGIC_RESET:
            tapctrl_reset(tap);
            break;
        case CAPTURE_DR:
            tapctrl_capture_dr(tap, tap->ir_hold);
            break;
        case SHIFT_DR:
            tap->tdo = tap->dr & 0b1;
            break;
        case UPDATE_DR:
            tapctrl_update_dr(tap);
            break;
        case CAPTURE_IR:
            tapctrl_capture_ir(tap);
            break;
        case SHIFT_IR:
            tap->tdo = tap->ir & 0b1;
            break;
        case UPDATE_IR:
            tapctrl_update_ir(tap);
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

static void tapctrl_bb_blink(TAPController *tap, bool light) {}

static void tapctrl_bb_read(TAPController *tap)
{
    tap->outbuf[tap->outpos++] = '0' + (unsigned)tap->tdo;
}

static void tapctrl_bb_quit(TAPController *tap)
{
    tapctrl_reset(tap);
}

static void tapctrl_bb_write(TAPController *tap, bool tck, bool tms, bool tdi)
{
    tapctrl_step(tap, tck, tms, tdi);
}

static void tapctrl_bb_reset(TAPController *tap, bool trst, bool srst)
{
    trace_jtag_tapctrl_reset(trst, srst);
    if (trst) {
        tapctrl_reset(tap);
    }
    tap->trst = trst;
    tap->srst = srst;
}

/*
 * TAP Server implementation
 */

static void tap_read_byte(uint8_t ch)
{
    switch ((char)ch) {
    case 'B':
        tapctrl_bb_blink(tapserver_state.tap, true);
        break;
    case 'b':
        tapctrl_bb_blink(tapserver_state.tap, false);
        break;
    case 'R':
        tapctrl_bb_read(tapserver_state.tap);
        break;
    case 'Q':
        tapctrl_bb_quit(tapserver_state.tap);
        break;
    case '0':
        tapctrl_bb_write(tapserver_state.tap, false, false, false);
        break;
    case '1':
        tapctrl_bb_write(tapserver_state.tap, false, false, true);
        break;
    case '2':
        tapctrl_bb_write(tapserver_state.tap, false, true, false);
        break;
    case '3':
        tapctrl_bb_write(tapserver_state.tap, false, true, true);
        break;
    case '4':
        tapctrl_bb_write(tapserver_state.tap, true, false, false);
        break;
    case '5':
        tapctrl_bb_write(tapserver_state.tap, true, false, true);
        break;
    case '6':
        tapctrl_bb_write(tapserver_state.tap, true, true, false);
        break;
    case '7':
        tapctrl_bb_write(tapserver_state.tap, true, true, true);
        break;
    case 'r':
        tapctrl_bb_reset(tapserver_state.tap, false, false);
        break;
    case 's':
        tapctrl_bb_reset(tapserver_state.tap, false, true);
        break;
    case 't':
        tapctrl_bb_reset(tapserver_state.tap, true, false);
        break;
    case 'u':
        tapctrl_bb_reset(tapserver_state.tap, true, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Unknown TAP code 0x%02x\n", __func__,
                      (unsigned)ch);
        break;
    }
}

static int tap_chr_can_receive(void *opaque)
{
    /* do not accept any input till a TAP controller is available */
    return ((TAPServerState *)opaque)->tap ? MAX_PACKET_LENGTH : 0;
}

static void tap_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    TAPServerState *s = (TAPServerState *)opaque;

    for (unsigned ix = 0; ix < size; ix++) {
        tap_read_byte(buf[ix]);
    }

    if (s->tap->outpos) {
        qemu_chr_fe_write_all(&s->chr, s->tap->outbuf, s->tap->outpos);
        s->tap->outpos = 0u;
    }
}

static int tap_monitor_write(Chardev *chr, const uint8_t *buf, int len)
{
    return 0;
}

static void tap_monitor_open(Chardev *chr, ChardevBackend *backend,
                             bool *be_opened, Error **errp)
{
    *be_opened = false;
}

static void char_tap_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->internal = true;
    cc->open = tap_monitor_open;
    cc->chr_write = tap_monitor_write;
}

#define TYPE_CHARDEV_JTAG "chardev-jtag"

static const TypeInfo char_tap_type_info = {
    .name = TYPE_CHARDEV_JTAG,
    .parent = TYPE_CHARDEV,
    .class_init = char_tap_class_init,
};

static void init_tapserver_state(void)
{
    g_assert(!tapserver_state.init);
    memset(&tapserver_state, 0, sizeof(TAPServerState));
    tapserver_state.init = true;
}

int jtagserver_start(const char *device)
{
    char tapstub_device_name[128];
    Chardev *chr = NULL;

    if (!device) {
        return -1;
    }
    if (strcmp(device, "none") != 0) {
        if (strstart(device, "tcp:", NULL)) {
            snprintf(tapstub_device_name, sizeof(tapstub_device_name),
                     "%s,wait=off,nodelay=on,server=on", device);
            device = tapstub_device_name;
        }
        chr = qemu_chr_new_noreplay("tap", device, true, NULL);
        if (!chr) {
            return -1;
        }
    }

    if (!tapserver_state.init) {
        init_tapserver_state();
    } else {
        qemu_chr_fe_deinit(&tapserver_state.chr, true);
    }

    if (chr) {
        qemu_chr_fe_init(&tapserver_state.chr, chr, &error_abort);
        qemu_chr_fe_set_handlers(&tapserver_state.chr, tap_chr_can_receive,
                                 tap_chr_receive, NULL, NULL, &tapserver_state,
                                 NULL, true);
    }

    return 0;
}

void jtagserver_exit(int code)
{
    if (!tapserver_state.init) {
        return;
    }

    qemu_chr_fe_deinit(&tapserver_state.chr, true);

    tapctrl_deinit(tapserver_state.tap);
    g_free(tapserver_state.tap);
    tapserver_state.tap = NULL;
}

int jtag_register_handler(unsigned code, const TAPDataHandler *tdh)
{
    if (!tapserver_state.tap) {
        return -1;
    }

    tapctrl_register_handler(tapserver_state.tap, code, tdh);

    return 0;
}

int jtag_configure_tap(size_t irlength, uint32_t idcode)
{
    if (tapserver_state.init) {
        if (!tapserver_state.tap) {
            TAPController *tap = g_new0(TAPController, 1);
            tapctrl_init(tap, irlength, idcode);
            tapserver_state.tap = tap;
            qemu_chr_fe_accept_input(&tapserver_state.chr);
            return 0;
        }
    }

    return -1;
}

static void register_types(void)
{
    type_register_static(&char_tap_type_info);
}

type_init(register_types);
