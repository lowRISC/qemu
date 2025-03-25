/*
 * QEMU OpenTitan utilities
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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
 */

#include "qemu/osdep.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/option.h"
#include "qemu/option_int.h"
#include "qemu/queue.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qom/object.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rom_ctrl_img.h"
#include "hw/riscv/ibex_common.h"
#include "trace.h"

typedef struct OtCommonObjectNode {
    Object *obj;
    QSIMPLEQ_ENTRY(OtCommonObjectNode) node;
} OtCommonObjectNode;

typedef QSIMPLEQ_HEAD(OtCommonObjectList,
                      OtCommonObjectNode) OtCommonObjectList;

typedef struct {
    const char *type; /* which type of object should be matched */
    unsigned count; /* how many object should be matched */
    OtCommonObjectList list; /* list of matched objects */
} OtCommonObjectNodes;

static const char *OT_COMMON_PROP_STRINGS[] = {
    "str",
    "string",
};

static const char *OT_COMMON_PROP_UINT[] = {
    "uint8",
    "uint16",
    "uint32",
    "uint64",
};

static const char *OT_COMMON_PROP_BOOL[] = { "bool" };

static int ot_common_node_child_walker(Object *child, void *opaque)
{
    OtCommonObjectNodes *nodes = opaque;
    if (!object_dynamic_cast(child, nodes->type)) {
        /* continue walking the children hierarchy */
        return 0;
    }

    OtCommonObjectNode *node = g_new0(OtCommonObjectNode, 1u);
    node->obj = child;
    object_ref(child);

    QSIMPLEQ_INSERT_TAIL(&nodes->list, node, node);

    nodes->count--;

    /* stop walking the hierarchy immediately if max count has been reached */
    return nodes->count ? 0 : 1;
}

CPUState *ot_common_get_local_cpu(DeviceState *s)
{
    BusState *bus = s->parent_bus;
    if (!bus) {
        return NULL;
    }

    Object *parent;
    if (bus->parent) {
        parent = OBJECT(bus->parent);
    } else if (bus == sysbus_get_default()) {
        parent = qdev_get_machine();
    } else {
        return NULL;
    }

    OtCommonObjectNodes nodes = {
        .type = TYPE_CPU,
        .count = 1u,
    };
    QSIMPLEQ_INIT(&nodes.list);

    /* find one of the closest CPU (should be only one with OT platforms) */
    if (object_child_foreach_recursive(OBJECT(parent),
                                       &ot_common_node_child_walker, &nodes)) {
        g_assert(!QSIMPLEQ_EMPTY(&nodes.list));
        OtCommonObjectNode *node = QSIMPLEQ_FIRST(&nodes.list);
        object_unref(node->obj);
        CPUState *cpu = CPU(node->obj);
        g_free(node);
        return cpu;
    }

    return NULL;
}

unsigned ot_common_check_rom_configuration(void)
{
    /* find all ROM defined images */
    OtCommonObjectNodes img_nodes = {
        .type = TYPE_OT_ROM_IMG,
    };
    QSIMPLEQ_INIT(&img_nodes.list);

    object_child_foreach_recursive(object_get_objects_root(),
                                   &ot_common_node_child_walker, &img_nodes);

    /* find all ROM controllers */
    OtCommonObjectNodes ctrl_nodes = {
        .type = TYPE_OT_ROM_CTRL,
    };
    QSIMPLEQ_INIT(&ctrl_nodes.list);

    object_child_foreach_recursive(object_get_root(),
                                   &ot_common_node_child_walker, &ctrl_nodes);

    /* NOLINTBEGIN(clang-analyzer-unix.Malloc) */

    /*
     * For each declared ROM image, check if there is a ROM controller that
     * can make use of it
     */
    OtCommonObjectNode *img_node, *img_next;
    QSIMPLEQ_FOREACH_SAFE(img_node, &img_nodes.list, node, img_next) {
        const char *comp = object_get_canonical_path_component(img_node->obj);
        OtCommonObjectNode *ctrl_node, *ctrl_next;
        QSIMPLEQ_FOREACH_SAFE(ctrl_node, &ctrl_nodes.list, node, ctrl_next) {
            Error *errp;
            char *ot_id =
                object_property_get_str(ctrl_node->obj, "ot_id", &errp);
            bool remove_img = !ot_id;
            bool remove_ctrl = false;
            if (ot_id) {
                if (!strcmp(ot_id, comp)) {
                    /* ROM image matches current ROM controller image */
                    remove_img = true;
                    remove_ctrl = true;
                }
            }
            g_free(ot_id);
            if (remove_img) {
                object_unref(img_node->obj);
                QSIMPLEQ_REMOVE(&img_nodes.list, img_node, OtCommonObjectNode,
                                node);
                g_free(img_node);
            }
            if (remove_ctrl) {
                object_unref(ctrl_node->obj);
                QSIMPLEQ_REMOVE(&ctrl_nodes.list, ctrl_node, OtCommonObjectNode,
                                node);
                g_free(ctrl_node);
            }
        }
    }

    /*
     * Any ROM image that have not been "consumed" by a ROM controller is an
     * unexpected declaration (likely an error in CLI options)
     */
    QSIMPLEQ_FOREACH_SAFE(img_node, &img_nodes.list, node, img_next) {
        const char *comp = object_get_canonical_path_component(img_node->obj);
        warn_report("Unused ROM image: %s", comp);
        object_unref(img_node->obj);
        g_free(img_node);
    }

    /*
     * Cleanup and count remaining ROM controllers for which no image has been
     * declared
     */
    OtCommonObjectNode *ctrl_node, *ctrl_next;
    unsigned count = 0;
    QSIMPLEQ_FOREACH_SAFE(ctrl_node, &ctrl_nodes.list, node, ctrl_next) {
        object_unref(ctrl_node->obj);
        g_free(ctrl_node);
        count++;
    }

    /* NOLINTEND(clang-analyzer-unix.Malloc) */

    return count;
}

AddressSpace *ot_common_get_local_address_space(DeviceState *s)
{
    CPUState *cpu = ot_common_get_local_cpu(s);

    return cpu ? cpu->as : NULL;
}

void ot_common_configure_device_opts(DeviceState **devices, unsigned count)
{
    // TODO need to use qemu_find_opts_err if no config is ok
    QemuOptsList *optlist = qemu_find_opts("ot_device");
    if (!optlist) {
        qemu_log("%s: no config\n", __func__);
        return;
    }

    for (unsigned ix = 0; ix < count; ix++) {
        Object *obj = OBJECT(devices[ix]);
        if (!obj) {
            continue;
        }

        QemuOpts *opts = NULL;

        const char *typename = object_get_typename(obj);
        char *ot_id = object_property_get_str(obj, OT_COMMON_DEV_ID, NULL);
        char *obj_id = NULL;
        if (ot_id && ot_id[0]) {
            /* try to locate option with the <type>:<id> syntax */
            obj_id = g_strdup_printf("%s.%s", typename, ot_id);
            opts = qemu_opts_find(optlist, obj_id);
        }
        g_free(ot_id);

        if (!opts) {
            g_free(obj_id);
            /*
             * either there's no <type>:<id> option, or the device does not have
             * a unique identifier
             */
            opts = qemu_opts_find(optlist, typename);
            obj_id = opts ? g_strdup(typename) : NULL;
        }

        if (!opts) {
            g_free(obj_id);
            continue;
        }

        QemuOpt *opt = QTAILQ_FIRST(&opts->head);
        while (opt) {
            const char *type;
            type = object_property_get_type(obj, opt->name, NULL);
            if (!type) {
                error_setg(&error_fatal, "%s: unknown property %s for %s",
                           __func__, opt->name, obj_id);
                goto next;
            }
            for (unsigned tx = 0; tx < ARRAY_SIZE(OT_COMMON_PROP_STRINGS);
                 tx++) {
                if (!strcmp(type, OT_COMMON_PROP_STRINGS[tx])) {
                    object_property_set_str(obj, opt->name, opt->str,
                                            &error_fatal);
                    trace_ot_common_configure_device_str(obj_id, opt->name,
                                                         opt->str);
                    goto next;
                }
            }
            for (unsigned tx = 0; tx < ARRAY_SIZE(OT_COMMON_PROP_UINT); tx++) {
                if (!strcmp(type, OT_COMMON_PROP_UINT[tx])) {
                    uint64_t value;
                    if (qemu_strtou64(opt->str, NULL, 0, &value)) {
                        error_setg(
                            &error_fatal,
                            "%s: invalid unsigned integer property %s for %s",
                            __func__, opt->name, obj_id);
                        goto next;
                    }
                    object_property_set_uint(obj, opt->name, value,
                                             &error_fatal);
                    trace_ot_common_configure_device_uint(obj_id, opt->name,
                                                          opt->value.uint);
                    goto next;
                }
            }
            for (unsigned tx = 0; tx < ARRAY_SIZE(OT_COMMON_PROP_BOOL); tx++) {
                if (!strcmp(type, OT_COMMON_PROP_BOOL[tx])) {
                    bool value;
                    qapi_bool_parse(opt->name, opt->str, &value, &error_fatal);
                    object_property_set_bool(obj, opt->name, value,
                                             &error_fatal);
                    trace_ot_common_configure_device_bool(obj_id, opt->name,
                                                          opt->value.boolean);
                    goto next;
                }
            }

            g_free(obj_id);
            error_setg(&error_fatal,
                       "unsupported type %s for property %s of %s", type,
                       opt->name, obj_id);
            return;

        next:
            opt = QTAILQ_NEXT(opt, next);
        }

        g_free(obj_id);
    }
}

void ot_common_configure_devices_with_id(
    DeviceState **devices, BusState *bus, const char *id_value, bool id_prepend,
    const IbexDeviceDef *defs, size_t count)
{
    ibex_link_devices(devices, defs, count);
    ibex_define_device_props(devices, defs, count);
    if (id_value) {
        ibex_identify_devices(devices, OT_COMMON_DEV_ID, id_value, id_prepend,
                              count);
    }
    ot_common_configure_device_opts(devices, count);
    ibex_realize_devices(devices, bus, defs, count);
    ibex_connect_devices(devices, defs, count);
}

int ot_common_string_ends_with(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    return (str_len >= suffix_len) &&
           (!memcmp(str + str_len - suffix_len, suffix, suffix_len));
}

int ot_common_parse_hexa_str(uint8_t *out, const char *xstr, size_t olen,
                             bool reverse, bool exact)
{
    for (size_t ix = 0; ix < olen; ix++) {
        gint hi = g_ascii_xdigit_value(xstr[ix * 2u]);
        if (hi < 0) {
            return hi;
        }
        gint lo = g_ascii_xdigit_value(xstr[ix * 2u + 1u]);
        if (lo < 0) {
            return lo;
        }
        out[reverse ? olen - ix - 1u : ix] = (uint8_t)(hi << 4) | (uint8_t)(lo);
    }

    return (!exact || !xstr[olen * 2u]) ? 0 : 1;
}

/*
 * Unfortunately, there is no QEMU API to properly disable serial control lines
 */
#ifndef _WIN32
#include <termios.h>
#include "chardev/char-fd.h"
#include "chardev/char-fe.h"
#include "io/channel-file.h"
#endif

void ot_common_ignore_chr_status_lines(CharBackend *chr)
{
/* it might be useful to move this to char-serial.c */
#ifndef _WIN32
    FDChardev *cd = FD_CHARDEV(chr->chr);
    QIOChannelFile *fioc = QIO_CHANNEL_FILE(cd->ioc_in);

    struct termios tty = { 0 };
    tcgetattr(fioc->fd, &tty);
    tty.c_cflag |= CLOCAL; /* ignore modem status lines */
    tcsetattr(fioc->fd, TCSANOW, &tty);
#endif
}
