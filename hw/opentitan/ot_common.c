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
#include "qom/object.h"
#include "hw/opentitan/ot_common.h"

typedef struct {
    const char *type; /* which type of device should be matched */
    DeviceState *device; /* matched device if any */
} OtCommonNodeEntry;

static int ot_common_node_child_walker(Object *child, void *opaque)
{
    OtCommonNodeEntry *entry = opaque;
    if (!object_dynamic_cast(child, entry->type)) {
        /* continue walking the children hierarchy */
        return 0;
    }

    entry->device = DEVICE(child);

    /* stop walking the hierarchy immediately */
    return 1;
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

    OtCommonNodeEntry entry = {
        .type = TYPE_CPU,
        .device = NULL,
    };

    /* find one of the closest CPU (should be only one with OT platforms) */
    if (object_child_foreach_recursive(OBJECT(parent),
                                       &ot_common_node_child_walker, &entry)) {
        return CPU(entry.device);
    }

    return NULL;
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
