/*
 * QEMU OpenTitan utilities
 *
 * Copyright (c) 2023 Rivos, Inc.
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
#include "qemu/error-report.h"
#include "qemu/queue.h"
#include "qom/object.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rom_ctrl_img.h"

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

/*
 * Unfortunately, there is no QEMU API to properly disable serial control lines
 */
#ifndef _WIN32
#include <termios.h>
#include "chardev/char-fd.h"
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

int ot_common_string_ends_with(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    return (str_len >= suffix_len) &&
           (!memcmp(str + str_len - suffix_len, suffix, suffix_len));
}
