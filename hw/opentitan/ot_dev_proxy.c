/*
 * QEMU OpenTitan Device Proxy
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
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
#include "qemu/fifo8.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc.h"
#include "qom/object.h"
#include "chardev/char-fe.h"
#include "exec/memory.h"
#include "hw/core/cpu.h"
#include "hw/irq.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_dev_proxy.h"
#include "hw/opentitan/ot_mbx.h"
#include "hw/opentitan/ot_soc_proxy.h"
#include "hw/opentitan/ot_sram_ctrl.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "sysemu/runstate.h"
#include "trace.h"

/* ------------------------------------------------------------------------ */
/* Register definitions */
/* ------------------------------------------------------------------------ */

/* clang-format off */
/*
 * External DoE interface, as seen from the requester.
 * Should match PCIe 6.1 section 7.9.24
 */
REG32(MBX_DOE_CONTROL, 0x08u)
    SHARED_FIELD(MBX_CONTROL_ABORT, 0u, 1u)
    SHARED_FIELD(MBX_CONTROL_INT_EN, 1u, 1u)
    FIELD(MBX_DOE_CONTROL, GO, 31u, 1u)
REG32(MBX_DOE_STATUS, 0x0cu)
    SHARED_FIELD(MBX_STATUS_BUSY, 0u, 1u)
    SHARED_FIELD(MBX_STATUS_INT_STATUS, 1u, 1u)
    SHARED_FIELD(MBX_STATUS_ERROR, 2u, 1u)
    SHARED_FIELD(MBX_STATUS_READY, 31u, 1u)
REG32(MBX_DOE_WRITE_DATA, 0x010u)
REG32(MBX_DOE_READ_DATA, 0x014u)

/* clang-format on */

/* ------------------------------------------------------------------------ */
/* Mailbox proxy */
/* ------------------------------------------------------------------------ */

#define DEV_PROXY_DESC_LEN 16u

typedef struct _DevProxyHeader {
    uint16_t command;
    uint16_t length;
    uint32_t uid;
} DevProxyHeader;

static_assert(sizeof(DevProxyHeader) == 8u, "Invalid header size");

typedef struct {
    MemoryRegion *mr; /* memory region to access */
    /* the following fields are only meaningful for SUS_BUS_DEVICE items */
    unsigned reg_count; /* count of accessible device registers */
    uint32_t irq_mask; /* mask of routable IRQs on this device */
} OtDevProxyCaps;

typedef struct {
    Object *obj; /* proxied object */
    OtDevProxyCaps caps; /* object capabilities */
    const char *prefix; /* prefix name for idenfifying the device */
    GHashTable *iirq_ht; /* intercepted IRQs, may be NULL */
    char desc[DEV_PROXY_DESC_LEN]; /* user friendly name, for debug purposes */
} OtDevProxyItem;

typedef struct {
    qemu_irq irq_orig; /* original IRQ destination (to QEMU device) */
    unsigned dev_num; /* device number (in device array) */
    uint16_t irq_num; /* IRQ number (in proxied device) */
    uint8_t grp_num; /* IRQ group (in proxied device) */
    bool assigned; /* Proxy IRQ slot in use */
} OtDevProxyIrq;

typedef struct {
    MemoryRegion *mr;
    BusState *bus;
} OtDevProxySystem;

struct OtDevProxyWatcherState {
    DeviceState parent_obj;
    MemoryRegion mmio;

    QSIMPLEQ_ENTRY(OtDevProxyWatcherState) watcher;

    OtDevProxyState *devproxy;
    MemoryRegion *root;
    unsigned wid;
    unsigned address;
    unsigned size;
    unsigned priority;
    unsigned stop;
    bool read;
    bool write;
};

struct OtDevProxyState {
    DeviceState parent_obj;

    OtDevProxyItem *items; /* proxied devices */
    OtDevProxyIrq *proxy_irq_map; /* IRQ interception mapping */
    OtDevProxySystem *subsys; /*subsystem array */
    QSIMPLEQ_HEAD(, OtDevProxyWatcherState) watchers;
    unsigned dev_count; /* count of IRQ in items */
    unsigned subsys_count; /* count of memory roots */
    unsigned last_wid;

    Fifo8 rx_fifo; /* input FIFO */
    DevProxyHeader rx_hdr; /* received proxy header */
    unsigned requester_uid; /* requester input counter */
    unsigned initiator_uid; /* initiator output counter */
    uint32_t *rx_buffer; /* received payload */

    CharBackend chr; /* communication device */
    guint watch_tag; /* tracker for comm device change */
};

typedef void ot_dev_proxy_register_device_fn(GArray *array, Object *obj);

typedef struct {
    const char *typename;
    ot_dev_proxy_register_device_fn *reg_dev;
} OtDevProxyDevice;

enum OtDevProxyErr {
    /* No error */
    PE_NO_ERROR,
    /* Undefined errors */
    PE_UNKNOWN = 0x1,
    /* Request errors */
    PE_INVALID_COMMAND_LENGTH = 0x101,
    PE_INVALID_COMMAND_CODE,
    PE_INVALID_REQUEST_ID,
    PE_INVALID_SPECIFIER_ID,
    PE_INVALID_DEVICE_ID,
    PE_INVALID_IRQ,
    PE_INVALID_REG_ADDRESS,
    /* State error */
    PE_DEVICE_IN_ERROR = 0x201,
    /* Local error */
    PE_CANNOT_READ_DEVICE = 0x401,
    PE_CANNOT_WRITE_DEVICE,
    PE_TRUNCATED_RESPONSE,
    PE_INCOMPLETE_WRITE,
    PE_OOM, /* out of resources */
    /* internal error */
    PE_UNSUPPORTED_DEVICE = 0x801,
};

#define PROXY_VER_MAJ 0
#define PROXY_VER_MIN 14u

#define PROXY_IRQ_INTERCEPT_COUNT 32u
#define PROXY_IRQ_INTERCEPT_NAME  "irq-intercept"

#define PROXY_DISABLED_ROLE 0xfu

#define PROXY_COMMAND(_a_, _b_) ((((uint16_t)(_a_)) << 8u) | ((uint8_t)(_b_)))
#define PROXY_UID(_u_)          ((_u_) & ~(1u << 31u))
#define PROXY_MAKE_UID(_uid_, _req_) \
    (((_uid_) & ~(1u << 31u)) | (((uint32_t)(bool)(_req_)) << 31u))

static void ot_dev_proxy_reg_mr(GArray *array, Object *obj);
static void ot_dev_proxy_reg_mbx(GArray *array, Object *obj);
static void ot_dev_proxy_reg_soc_proxy(GArray *array, Object *obj);
static void ot_dev_proxy_reg_sram_ctrl(GArray *array, Object *obj);

static OtDevProxyDevice SUPPORTED_DEVICES[] = {
    {
        .typename = TYPE_OT_MBX,
        .reg_dev = &ot_dev_proxy_reg_mbx,
    },
    {
        .typename = TYPE_OT_SOC_PROXY,
        .reg_dev = &ot_dev_proxy_reg_soc_proxy,
    },
    {
        .typename = TYPE_OT_SRAM_CTRL,
        .reg_dev = &ot_dev_proxy_reg_sram_ctrl,
    },
};

static void ot_dev_proxy_send(OtDevProxyState *s, unsigned uid, int dir,
                              uint16_t command, const void *payload,
                              size_t length)
{
    DevProxyHeader tx_hdr = { command, length, PROXY_MAKE_UID(uid, dir) };
    const uint8_t *buf = (const uint8_t *)&tx_hdr;
    int len = (int)sizeof(tx_hdr);
    int ret;

    /* "synchronous" write */
    while (len > 0) {
        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            return;
        }
        ret = qemu_chr_fe_write(&s->chr, buf, len);
        if (ret < 0) {
            trace_ot_dev_proxy_fe_error(ret);
            return;
        }
        len -= ret;
        buf += ret;
    }

    len = (int)length;
    buf = (const uint8_t *)payload;
    while (len > 0) {
        if (!qemu_chr_fe_backend_connected(&s->chr)) {
            return;
        }
        ret = qemu_chr_fe_write(&s->chr, buf, len);
        if (ret < 0) {
            trace_ot_dev_proxy_fe_error(ret);
            return;
        }
        len -= ret;
        buf += ret;
    }
}

static void ot_dev_proxy_reply_payload(OtDevProxyState *s, uint16_t command,
                                       const void *payload, size_t length)
{
    ot_dev_proxy_send(s, s->requester_uid, 0, command, payload, length);
}

static void ot_dev_proxy_signal(OtDevProxyState *s, uint16_t command,
                                const OtDevProxyIrq *proxy_irq, int value)
{
    uint32_t buffer[3u] = { ((uint32_t)(proxy_irq->dev_num & 0xfffu) << 16u),
                            ((uint32_t)(proxy_irq->irq_num)) |
                                ((uint32_t)(proxy_irq->grp_num) << 16u),
                            (uint32_t)value };

    ot_dev_proxy_send(s, s->initiator_uid, 1, command, buffer, sizeof(buffer));

    /* as a signal, do not expect a peer response */
    s->initiator_uid++;
}

static void ot_dev_proxy_reply_error(OtDevProxyState *s, uint32_t error,
                                     const char *msg)
{
    if (msg) {
        size_t len = strlen(msg);
        size_t size = sizeof(uint32_t) + ROUND_UP(len, sizeof(uint32_t));
        uint32_t *buf = g_new0(uint32_t, size / sizeof(uint32_t));
        buf[0] = error;
        memcpy((char *)&buf[1], msg, len);
        ot_dev_proxy_reply_payload(s, PROXY_COMMAND('x', 'x'), buf, size);
        g_free(buf);
    } else {
        ot_dev_proxy_reply_payload(s, PROXY_COMMAND('x', 'x'), &error,
                                   sizeof(error));
    }
}

static void ot_dev_proxy_handshake(OtDevProxyState *s)
{
    /* initial client connection, reset uid trackers */
    s->requester_uid = PROXY_UID(s->rx_hdr.uid);
    s->initiator_uid = 0;
    uint32_t payload = (PROXY_VER_MIN << 0u) | (PROXY_VER_MAJ << 16u);
    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('h', 's'), &payload,
                               sizeof(payload));
}

static void ot_dev_proxy_enumerate_devices(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 0) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    struct entry {
        uint32_t header;
        uint32_t base;
        uint32_t count;
        char desc[DEV_PROXY_DESC_LEN];
    };
    g_assert(sizeof(struct entry) == 7u * sizeof(uint32_t));

    struct entry *entries = g_new0(struct entry, s->dev_count);
    unsigned count = 0;
    unsigned mrcount = 0;
    char desc[32u];
    for (unsigned ix = 0; ix < s->dev_count; ix++) {
        OtDevProxyItem *item = &s->items[ix];
        const OtDevProxyCaps *caps = &item->caps;
        struct entry *entry = &entries[count];
        memset(entry, 0, sizeof(*entry));
        memset(desc, 0, sizeof(desc));
        const char *oid = NULL;
        for (unsigned dix = 0; dix < ARRAY_SIZE(SUPPORTED_DEVICES); dix++) {
            if (object_dynamic_cast(item->obj,
                                    SUPPORTED_DEVICES[dix].typename)) {
                oid = object_property_get_str(item->obj, "ot_id", &error_fatal);
                (void)snprintf(desc, sizeof(desc), "%s%s", item->prefix, oid);
                break;
            }
        }
        if (!oid) {
            if (object_dynamic_cast(item->obj, TYPE_MEMORY_REGION)) {
                char name[16u];
                unsigned pos = 0;
                const char *src = item->caps.mr->name;
                memset(name, 0, sizeof(name));
                while (src && *src) {
                    switch (*src) {
                    case '-':
                    case '.':
                    case '_':
                    case ' ':
                        break;
                    default:
                        if (pos < sizeof(name)) {
                            name[pos++] = *src;
                        }
                        break;
                    }
                    src++;
                }
                (void)snprintf(desc, sizeof(desc), "%s%s%u", item->prefix, name,
                               mrcount);
                mrcount++;
            }
            if (!desc[0]) {
                warn_report("%s: ignoring discovered device: %s\n", __func__,
                            object_get_typename(item->obj));
                continue;
            }
        }
        /*
         * desc field does not need to be zero-ended, but its content should not
         * exceed the field storage, otherwise truncature may occur with
         * multiple instances ending up with the same descriptor.
         */
        if (desc[sizeof(entry->desc)]) {
            error_setg(&error_fatal, "Device %s cannot be described: %s\n",
                       object_get_typename(item->obj), desc);
        }
        memcpy(entry->desc, desc, sizeof(entry->desc));
        strncpy(item->desc, desc, sizeof(entry->desc));
        item->desc[sizeof(entry->desc) - 1] = '\0';
        entry->header = ix << 16u;
        entry->base = (uint32_t)caps->mr->addr;
        entry->count = caps->reg_count;
        count++;
        if (ix >= 0xfffu) {
            break;
        }
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('e', 'd'), entries,
                               count * sizeof(struct entry));
    g_free(entries);
}

static void ot_dev_proxy_enumerate_memory_spaces(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 0) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    struct entry {
        uint32_t header;
        uint32_t address;
        uint32_t size;
        char desc[32u];
    };
    g_assert(sizeof(struct entry) == 11u * sizeof(uint32_t));

    struct entry *entries = g_new0(struct entry, s->subsys_count);
    unsigned count = 0;
    for (unsigned ix = 0; ix < s->subsys_count; ix++) {
        const OtDevProxySystem *subsys = &s->subsys[ix];
        struct entry *entry = &entries[count];
        entry->header = ix << 24u;
        entry->address = subsys->mr->addr;
        uint64_t size = memory_region_size(subsys->mr);
        entry->size = (uint32_t)MIN(size, UINT32_MAX);
        const char *name = memory_region_name(subsys->mr);
        size_t namelen = strlen(name);
        if (namelen > sizeof(entry->desc)) {
            memcpy(entry->desc, &name[namelen - sizeof(entry->desc)],
                   sizeof(entry->desc));
        } else {
            memcpy(entry->desc, name, namelen);
        }
        count++;
        if (ix >= 0xffu) {
            /* only 256 root regions are supported for now */
            break;
        }
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('e', 's'), entries,
                               count * sizeof(struct entry));
    g_free(entries);
}

static void ot_dev_proxy_enumerate_interrupts(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 1u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];

    if (!object_dynamic_cast(item->obj, TYPE_DEVICE)) {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    DeviceState *dev = DEVICE(item->obj);

    unsigned group_count = 0;
    NamedGPIOList *ngl;
    QLIST_FOREACH(ngl, &dev->gpios, node) {
        group_count++;
    }

    struct irq_id {
        uint16_t count;
        uint8_t group;
        uint8_t dir;
        char name[32u];
    };
    static_assert(sizeof(struct irq_id) == 9u * sizeof(uint32_t),
                  "invalid struct irq_id, need packing");

    struct irq_id *entries;

    if (group_count) {
        entries = g_new0(struct irq_id, group_count);
        struct irq_id *irq_id = entries;
        unsigned group = 0;
        QLIST_FOREACH(ngl, &dev->gpios, node) {
            if (group > UINT8_MAX) {
                /* cannot handle more groups (unlikely) */
                break;
            }
            if (ngl->num_out) {
                irq_id->count = ngl->num_out;
                irq_id->dir = (1u << 7u);
            } else {
                irq_id->count = ngl->num_in;
                irq_id->dir = (0u << 7u);
            }
            /* input sysbus IRQs are typically unnamed */
            strncpy(irq_id->name, ngl->name ?: "", sizeof(irq_id->name) - 1u);
            irq_id->group = group++;
            irq_id++;
        }
    } else {
        entries = NULL;
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('i', 'e'), entries,
                               group_count * sizeof(struct irq_id));
    g_free(entries);
}

static void ot_dev_proxy_read_reg(OtDevProxyState *s)
{
    if (s->rx_hdr.length != sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    hwaddr reg = (hwaddr)(s->rx_buffer[0] & 0xffffu);
    unsigned role = s->rx_buffer[0] >> 28u;
    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    OtDevProxyCaps *caps = &item->caps;
    if (reg >= caps->reg_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_REG_ADDRESS, NULL);
        return;
    }

    MemoryRegion *mr = caps->mr;
    if (!mr) {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    trace_ot_dev_proxy_read_reg(item->desc, reg);

    const MemoryRegionOps *ops = mr->ops;
    if (role != PROXY_DISABLED_ROLE ? !ops->read_with_attrs : !ops->read) {
        ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
        return;
    }

    uint64_t tmp;

    if (role != PROXY_DISABLED_ROLE) {
        MemTxAttrs attrs = MEMTXATTRS_WITH_ROLE(role);
        MemTxResult res;

        res = ops->read_with_attrs(mr->opaque, reg << 2u, &tmp,
                                   sizeof(uint32_t), attrs);
        if (res != MEMTX_OK) {
            ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, NULL);
            return;
        }
    } else {
        tmp = ops->read(mr->opaque, reg << 2u, sizeof(uint32_t));
    }

    uint32_t buf = (uint32_t)tmp;
    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('r', 'w'), &buf, sizeof(buf));
}

static void ot_dev_proxy_write_reg(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 3u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    hwaddr reg = (hwaddr)(s->rx_buffer[0] & 0xffffu);
    unsigned role = s->rx_buffer[0] >> 28u;
    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;
    uint32_t value = s->rx_buffer[1u];
    uint32_t mask = s->rx_buffer[2u];

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    OtDevProxyCaps *caps = &item->caps;
    if (reg >= caps->reg_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_REG_ADDRESS, NULL);
        return;
    }

    MemoryRegion *mr = caps->mr;
    if (!mr) {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    trace_ot_dev_proxy_write_reg(item->desc, reg, value);

    const MemoryRegionOps *ops = mr->ops;
    if (role != PROXY_DISABLED_ROLE ? !ops->write_with_attrs : !ops->write) {
        ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
        return;
    }

    MemTxAttrs attrs = MEMTXATTRS_WITH_ROLE(role);
    MemTxResult res;
    uint64_t tmp;

    if (mask != 0xffffffffu) {
        if (role != PROXY_DISABLED_ROLE ? !ops->read_with_attrs : !ops->read) {
            ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
            return;
        }
        if (role != PROXY_DISABLED_ROLE) {
            res = ops->read_with_attrs(mr->opaque, reg << 2u, &tmp,
                                       sizeof(uint32_t), attrs);
            if (res != MEMTX_OK) {
                ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, NULL);
                return;
            }
        } else {
            tmp = ops->read(mr->opaque, reg << 2u, sizeof(uint32_t));
        }

        tmp &= (uint64_t)~mask;
        tmp |= (uint64_t)(value & mask);
    } else {
        tmp = (uint64_t)value;
    }

    if (role != PROXY_DISABLED_ROLE) {
        res = ops->write_with_attrs(mr->opaque, reg << 2u, tmp,
                                    sizeof(uint32_t), attrs);
        if (res != MEMTX_OK) {
            ot_dev_proxy_reply_error(s, PE_CANNOT_WRITE_DEVICE, NULL);
            return;
        }
    } else {
        ops->write(mr->opaque, reg << 2u, tmp, sizeof(uint32_t));
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('w', 'w'), NULL, 0);
}

static void ot_dev_proxy_read_buffer(OtDevProxyState *s, bool mbx_mode)
{
    if (s->rx_hdr.length != 2u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    hwaddr reg = (hwaddr)(s->rx_buffer[0] & 0xffffu);
    unsigned role = s->rx_buffer[0] >> 28u;
    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;
    unsigned count = s->rx_buffer[1u];

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    OtDevProxyCaps *caps = &item->caps;
    Object *obj = item->obj;
    MemTxAttrs attrs = MEMTXATTRS_WITH_ROLE(role);
    MemoryRegion *mr = NULL;
    MemTxResult res = MEMTX_OK;

    if (object_dynamic_cast(obj, TYPE_OT_MBX)) {
        if (mbx_mode && reg != R_MBX_DOE_READ_DATA) {
            ot_dev_proxy_reply_error(s, PE_INVALID_REG_ADDRESS, NULL);
            return;
        }
        mr = caps->mr;
    }
    if (!mr) {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    trace_ot_dev_proxy_read_buffer(item->desc, mbx_mode, reg, count);

    const MemoryRegionOps *ops = mr->ops;
    if (role != PROXY_DISABLED_ROLE ? !ops->read_with_attrs : !ops->read) {
        ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
        return;
    }
    if (mbx_mode) {
        if (role != PROXY_DISABLED_ROLE ? !ops->write_with_attrs :
                                          !ops->write) {
            ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
            return;
        }
    }

    uint32_t *buf = g_new0(uint32_t, count);
    hwaddr addr = reg << 2u;
    for (unsigned ix = 0; ix < count; ix++) {
        uint64_t tmp;
        if (mbx_mode) {
            /* read DOE status */
            if (role != PROXY_DISABLED_ROLE) {
                res = ops->read_with_attrs(mr->opaque, A_MBX_DOE_STATUS, &tmp,
                                           sizeof(uint32_t), attrs);
                if (res != MEMTX_OK) {
                    ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, NULL);
                    goto end;
                }
            } else {
                tmp = ops->read(mr->opaque, A_MBX_DOE_STATUS, sizeof(uint32_t));
            }
            uint32_t status = (uint32_t)tmp;
            if (status & MBX_STATUS_ERROR_MASK) {
                ot_dev_proxy_reply_error(s, PE_DEVICE_IN_ERROR, NULL);
                goto end;
            }
            if (!(status & MBX_STATUS_READY_MASK)) {
                /* update requested count with actual count */
                count = ix;
                break;
            }
        }
        /* read value */
        if (role != PROXY_DISABLED_ROLE) {
            res = ops->read_with_attrs(mr->opaque, addr, &tmp, sizeof(uint32_t),
                                       attrs);
            if (res != MEMTX_OK) {
                ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, NULL);
                break;
            }
        } else {
            tmp = ops->read(mr->opaque, addr, sizeof(uint32_t));
        }
        buf[ix] = (uint32_t)tmp;
        if (mbx_mode) {
            /* mark as read */
            if (role != PROXY_DISABLED_ROLE) {
                res = ops->write_with_attrs(mr->opaque, addr, tmp,
                                            sizeof(uint32_t), attrs);
                if (res != MEMTX_OK) {
                    ot_dev_proxy_reply_error(s, PE_CANNOT_WRITE_DEVICE, NULL);
                    goto end;
                }
            } else {
                ops->write(mr->opaque, addr, tmp, sizeof(uint32_t));
            }
        } else {
            addr += sizeof(uint32_t);
        }
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('r', mbx_mode ? 'x' : 's'), buf,
                               count * sizeof(uint32_t));
end:
    g_free(buf);
}

static void ot_dev_proxy_write_buffer(OtDevProxyState *s, bool mbx_mode)
{
    if (s->rx_hdr.length < 2u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    hwaddr reg = (hwaddr)(s->rx_buffer[0] & 0xffffu);
    unsigned role = s->rx_buffer[0] >> 28u;
    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;
    unsigned count = s->rx_hdr.length / sizeof(uint32_t) - 1u;

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    OtDevProxyCaps *caps = &item->caps;
    Object *obj = item->obj;
    MemTxAttrs attrs = MEMTXATTRS_WITH_ROLE(role);
    MemoryRegion *mr = NULL;
    MemTxResult res = MEMTX_OK;

    if (object_dynamic_cast(obj, TYPE_OT_MBX)) {
        if (mbx_mode && reg != R_MBX_DOE_WRITE_DATA) {
            ot_dev_proxy_reply_error(s, PE_INVALID_REG_ADDRESS, NULL);
            return;
        }
        mr = caps->mr;
    }

    if (!mr) {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    trace_ot_dev_proxy_write_buffer(item->desc, mbx_mode, reg, count);

    const MemoryRegionOps *ops = mr->ops;
    if (role != PROXY_DISABLED_ROLE ? !ops->write_with_attrs : !ops->write) {
        ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
        return;
    }
    if (mbx_mode) {
        if (role != PROXY_DISABLED_ROLE ? !ops->read_with_attrs : !ops->read) {
            ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, "no accessor");
            return;
        }
    }

    uint32_t *buf = &s->rx_buffer[1];
    hwaddr addr = reg << 2u;
    uint64_t tmp;
    for (unsigned ix = 0; ix < count; ix++) {
        if (mbx_mode) {
            /* read DOE status */
            if (role != PROXY_DISABLED_ROLE) {
                res = ops->read_with_attrs(mr->opaque, A_MBX_DOE_STATUS, &tmp,
                                           sizeof(uint32_t), attrs);
                if (res != MEMTX_OK) {
                    ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, NULL);
                    return;
                }
            } else {
                tmp = ops->read(mr->opaque, A_MBX_DOE_STATUS, sizeof(uint32_t));
            }
            uint32_t status = (uint32_t)tmp;
            if (status & MBX_STATUS_BUSY_MASK) {
                count = ix;
                break;
            }
            if (status & MBX_STATUS_ERROR_MASK) {
                ot_dev_proxy_reply_error(s, PE_DEVICE_IN_ERROR, NULL);
                return;
            }
        }
        /* write data */
        tmp = (uint64_t)*buf++;

        if (role != PROXY_DISABLED_ROLE) {
            res = ops->write_with_attrs(mr->opaque, addr, tmp, sizeof(uint32_t),
                                        attrs);
            if (res != MEMTX_OK) {
                ot_dev_proxy_reply_error(s, PE_CANNOT_WRITE_DEVICE, NULL);
                return;
            }
        } else {
            ops->write(mr->opaque, addr, tmp, sizeof(uint32_t));
        }
    }
    if (mbx_mode) {
        /* update GO */
        if (role != PROXY_DISABLED_ROLE) {
            res = ops->read_with_attrs(mr->opaque, A_MBX_DOE_CONTROL, &tmp,
                                       sizeof(uint32_t), attrs);

            if (res != MEMTX_OK) {
                ot_dev_proxy_reply_error(s, PE_CANNOT_READ_DEVICE, NULL);
                return;
            }
        } else {
            tmp = ops->read(mr->opaque, A_MBX_DOE_CONTROL, sizeof(uint32_t));
        }
        tmp |= (uint64_t)R_MBX_DOE_CONTROL_GO_MASK;
        if (role != PROXY_DISABLED_ROLE) {
            res = ops->write_with_attrs(mr->opaque, A_MBX_DOE_CONTROL, tmp,
                                        sizeof(uint32_t), attrs);
            if (res != MEMTX_OK) {
                ot_dev_proxy_reply_error(s, PE_CANNOT_WRITE_DEVICE, NULL);
                return;
            }
        } else {
            ops->write(mr->opaque, A_MBX_DOE_CONTROL, tmp, sizeof(uint32_t));
        }
    }

    uint32_t obuf[1] = { count };
    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('w', mbx_mode ? 'x' : 's'),
                               &obuf, sizeof(obuf));
}

static void ot_dev_proxy_read_memory(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 3u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;
    unsigned offset = s->rx_buffer[1u];
    unsigned count = s->rx_buffer[2u];

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    OtDevProxyCaps *caps = &item->caps;
    Object *obj = item->obj;
    unsigned woffset = offset / sizeof(uint32_t);
    if (woffset > item->caps.reg_count) {
        count = 0;
    } else {
        unsigned maxcount = item->caps.reg_count - woffset;
        if (count > maxcount) {
            count = maxcount;
        }
    }

    trace_ot_dev_proxy_read_memory(item->desc, offset, count);

    uint32_t *buf = g_new0(uint32_t, count);
    if (count) {
        if (object_dynamic_cast(obj, TYPE_OT_SRAM_CTRL)) {
            MemoryRegion *mr = caps->mr;
            if (!mr) {
                ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
                goto end;
            }
            uintptr_t base = (uintptr_t)memory_region_get_ram_ptr(mr);
            base += offset;
            /* for now, there is no way to control role access */
            memcpy(buf, (const void *)base, count * sizeof(uint32_t));
        } else {
            ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
            goto end;
        }
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('r', 'm'), buf,
                               count * sizeof(uint32_t));
end:
    g_free(buf);
}

static void ot_dev_proxy_write_memory(OtDevProxyState *s)
{
    if (s->rx_hdr.length < 3u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;
    unsigned offset = s->rx_buffer[1];
    unsigned count = s->rx_hdr.length / sizeof(uint32_t) - 2u;
    const uint32_t *buffer = &s->rx_buffer[2];

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    OtDevProxyCaps *caps = &item->caps;
    Object *obj = item->obj;
    unsigned woffset = offset / sizeof(uint32_t);
    if (woffset > item->caps.reg_count) {
        count = 0;
    } else {
        unsigned maxcount = item->caps.reg_count - woffset;
        if (count > maxcount) {
            count = maxcount;
        }
    }

    trace_ot_dev_proxy_write_memory(item->desc, offset, count);

    if (object_dynamic_cast(obj, TYPE_OT_SRAM_CTRL)) {
        MemoryRegion *mr = caps->mr;
        if (!mr) {
            ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
            return;
        }
        uintptr_t base = (uintptr_t)memory_region_get_ram_ptr(mr);
        base += offset;
        /* for now, there is no way to control role access */
        memcpy((void *)base, buffer, count * sizeof(uint32_t));
        if (mr->ram_block) {
            memory_region_set_dirty(mr, (hwaddr)offset * sizeof(uint32_t),
                                    (hwaddr)count * sizeof(uint32_t));
        }
    } else {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    uint32_t obuf[1] = { count };
    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('w', 'm'), &obuf, sizeof(obuf));
}

static void
ot_dev_proxy_route_interrupt(OtDevProxyState *s, OtDevProxyItem *item,
                             const char *group, unsigned grp_n, unsigned irq_n)
{
    const char *dev_name = object_get_typename(item->obj);
    char *dev_id = object_property_get_str(item->obj, "ot_id", NULL);
    char *irq_name = g_strdup_printf("%s[%u]", group, irq_n);

    OtDevProxyIrq *proxy_irq =
        item->iirq_ht ? g_hash_table_lookup(item->iirq_ht, irq_name) : NULL;
    /* do not reroute IRQ if it is already routed */
    if (proxy_irq) {
        g_free(irq_name);
        g_free(dev_id);
        return;
    }

    unsigned six;
    for (six = 0; six < PROXY_IRQ_INTERCEPT_COUNT; six++) {
        if (!s->proxy_irq_map[six].assigned) {
            proxy_irq = &s->proxy_irq_map[six];
            break;
        }
    }
    /* caller should have verified that there are enough free slots */
    g_assert(proxy_irq);

    DeviceState *dev = DEVICE(item->obj);

    qemu_irq icpt_irq;
    icpt_irq =
        qdev_get_gpio_in_named(DEVICE(s), PROXY_IRQ_INTERCEPT_NAME, (int)six);
    proxy_irq->assigned = true;
    proxy_irq->irq_orig =
        qdev_intercept_gpio_out(dev, icpt_irq, group, (int)irq_n);
    proxy_irq->dev_num = (unsigned)(uintptr_t)(item - s->items);
    proxy_irq->grp_num = grp_n;
    proxy_irq->irq_num = irq_n;
    trace_ot_dev_proxy_intercept_irq(dev_name, dev_id ?: "?", irq_name, true);
    if (!item->iirq_ht) {
        /*
         * delete key (g_char strings), but never delete value that are
         * persisent, reassigned items
         */
        item->iirq_ht =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    char *ht_key = g_strdup(irq_name);
    g_hash_table_insert(item->iirq_ht, ht_key, proxy_irq);

    g_free(irq_name);
    g_free(dev_id);
}

static void ot_dev_proxy_restore_interrupt(OtDevProxyItem *item,
                                           const char *group, unsigned irq_n)
{
    const char *dev_name = object_get_typename(item->obj);
    char *dev_id = object_property_get_str(item->obj, "ot_id", NULL);
    char *irq_name = g_strdup_printf("%s[%u]", group, irq_n);

    if (!item->iirq_ht) {
        warn_report("Cannot restore interrupt, none intercepted: %s %s %s",
                    dev_name, dev_id ?: "?", irq_name);
        g_free(irq_name);
        g_free(dev_id);
        return;
    }

    OtDevProxyIrq *proxy_irq = g_hash_table_lookup(item->iirq_ht, irq_name);
    if (proxy_irq) {
        DeviceState *dev = DEVICE(item->obj);

        /* irq_orig == NULL is a valid use case */
        qdev_intercept_gpio_out(dev, proxy_irq->irq_orig, group, (int)irq_n);
        /* hash table takes care of deleting the key */
        g_hash_table_remove(item->iirq_ht, irq_name);
        memset(proxy_irq, 0, sizeof(*proxy_irq)); /* mark as free_slot */
        trace_ot_dev_proxy_intercept_irq(dev_name, dev_id ?: "?", irq_name,
                                         false);
    } else {
        warn_report("Cannot restore interrupt, not intercepted: %s %s %s",
                    dev_name, dev_id ?: "?", irq_name);
    }

    g_free(irq_name);
    g_free(dev_id);
}

static void ot_dev_proxy_intercept_interrupts(OtDevProxyState *s, bool enable)
{
    if (s->rx_hdr.length < 2u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];
    if (!object_dynamic_cast(item->obj, TYPE_DEVICE)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    unsigned group = s->rx_buffer[0] & 0xffu;

    /* check that the group identifier is actually valid for the device */
    DeviceState *dev = DEVICE(item->obj);
    NamedGPIOList *ngl = NULL;
    NamedGPIOList *tngl;
    unsigned grp = 0;
    QLIST_FOREACH(tngl, &dev->gpios, node) {
        if (!tngl->name) {
            /* anonymous IRQs are ignored, see enumerate_interrupts */
            continue;
        }
        if (grp++ < group) {
            continue;
        }
        ngl = tngl;
        break;
    }

    if (!ngl) {
        ot_dev_proxy_reply_error(s, PE_INVALID_IRQ, NULL);
        return;
    }

    /* check that all selected interrupts exits for the selected group */
    unsigned mask_count;
    mask_count = (s->rx_hdr.length - sizeof(uint32_t)) / sizeof(uint32_t);
    unsigned max_irq = 0;
    unsigned irq_count = 0;
    uint32_t *irqbms = &s->rx_buffer[1u];
    for (unsigned ix = 0; ix < mask_count; ix++) {
        uint32_t bm = irqbms[ix];
        if (bm) {
            unsigned hi = ctz32(bm);
            max_irq = ix * 32u + hi;
            while (bm) {
                irq_count += 1;
                bm &= ~(1u << ctz32(bm));
            }
        }
    }

    /*
     * count how many IRQ can be intercepted and tracked. Already intercepted
     * IRQs may be counted twice, remote peer should be more careful.
     */
    unsigned free_slot = 0;
    for (unsigned ix = 0; ix < PROXY_IRQ_INTERCEPT_COUNT; ix++) {
        if (!s->proxy_irq_map[ix].assigned) {
            free_slot += 1;
        }
    }
    if (irq_count > free_slot) {
        warn_report("IRQ interception slots exhausted %u for %u free",
                    irq_count, free_slot);
        ot_dev_proxy_reply_error(s, PE_OOM, NULL);
        return;
    }

    char *irq_name;
    irq_name = g_strdup_printf("%s[%u]", ngl->name, max_irq);
    ObjectProperty *prop = object_property_find(item->obj, irq_name);
    g_free(irq_name);
    irq_name = NULL;

    if (!prop) {
        ot_dev_proxy_reply_error(s, PE_INVALID_IRQ, NULL);
        return;
    }

    /* reroute all marked IRQs */
    for (unsigned ix = 0; ix < mask_count; ix++) {
        while (irqbms[ix]) {
            unsigned irq_n = ctz32(irqbms[ix]);
            irqbms[ix] &= ~(1u << irq_n);
            if (enable) {
                ot_dev_proxy_route_interrupt(s, item, ngl->name, group, irq_n);
            } else {
                ot_dev_proxy_restore_interrupt(item, ngl->name, irq_n);
            }
        }
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('i', enable ? 'i' : 'r'), NULL,
                               0);
}

static void ot_dev_proxy_signal_interrupt(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 3u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned devix = (s->rx_buffer[0] >> 16u) & 0xfffu;
    unsigned gid = s->rx_buffer[0u] & 0xffffu;

    if (devix >= s->dev_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, NULL);
        return;
    }

    OtDevProxyItem *item = &s->items[devix];

    if (!object_dynamic_cast(item->obj, TYPE_DEVICE)) {
        ot_dev_proxy_reply_error(s, PE_UNSUPPORTED_DEVICE, NULL);
        return;
    }

    DeviceState *dev = DEVICE(item->obj);

    unsigned irq_num = s->rx_buffer[1u] & 0xffffu;
    int irq_level = (int)s->rx_buffer[2u];

    NamedGPIOList *gl = NULL;
    NamedGPIOList *ngl;
    QLIST_FOREACH(ngl, &dev->gpios, node) {
        if (!gid) {
            gl = ngl;
            break;
        }
        gid--;
    }

    if (!gl) {
        ot_dev_proxy_reply_error(s, PE_INVALID_SPECIFIER_ID, "no such group");
        return;
    }

    if (irq_num >= gl->num_in) {
        ot_dev_proxy_reply_error(s, PE_INVALID_IRQ, "no such irq");
        return;
    }

    const char *dev_name = object_get_typename(item->obj);
    Error *errp = NULL;
    char *dev_id = object_property_get_str(item->obj, "ot_id", &errp);
    if (errp) {
        error_free(errp);
    }

    trace_ot_dev_proxy_signal_irq(dev_name, dev_id ?: "?", irq_num, irq_level);

    qemu_irq irq = gl->in[irq_num];
    qemu_set_irq(irq, irq_level);

    g_free(dev_id);

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('i', 's'), NULL, 0);
}

static void ot_dev_proxy_intercept_mmio(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 3u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned mspc = s->rx_buffer[0] >> 24u;
    if (mspc >= s->subsys_count) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, "Invalid MSpc");
        return;
    }

    MemoryRegion *mr = s->subsys[mspc].mr;
    g_assert(mr->addr == 0);

    uint64_t lmrsize = memory_region_size(mr);
    uint64_t mrsize = MAX(lmrsize, UINT32_MAX);

    uint32_t address = s->rx_buffer[1];
    uint32_t size = s->rx_buffer[2];

    if (((uint64_t)address + (uint64_t)size) > mrsize) {
        ot_dev_proxy_reply_error(s, PE_INVALID_REG_ADDRESS,
                                 "Invalid addr/size");
        return;
    }

    bool read = (bool)(s->rx_buffer[0] & 0b01);
    bool write = (bool)(s->rx_buffer[0] & 0b10);
    if (!(read || write)) {
        /* nothing to intercept */
        ot_dev_proxy_reply_error(s, PE_INVALID_SPECIFIER_ID,
                                 "Neither read nor write");
        return;
    }

    OtDevProxyWatcherState *watcher;
    watcher = (OtDevProxyWatcherState *)qdev_new(TYPE_OT_DEV_PROXY_WATCHER);

    unsigned prio = (s->rx_buffer[0] >> 2u) & 0x3f;
    unsigned stop = (s->rx_buffer[0] >> 8u) & 0x7f;
    if (!stop) {
        stop = UINT32_MAX;
    }

    object_property_set_link(OBJECT(watcher), "devproxy", OBJECT(s),
                             &error_fatal);
    object_property_set_link(OBJECT(watcher), "root", OBJECT(mr), &error_fatal);
    object_property_set_uint(OBJECT(watcher), "wid", s->last_wid, &error_fatal);
    object_property_set_uint(OBJECT(watcher), "address", address, &error_fatal);
    object_property_set_uint(OBJECT(watcher), "size", size, &error_fatal);
    object_property_set_uint(OBJECT(watcher), "priority", prio, &error_fatal);
    object_property_set_uint(OBJECT(watcher), "stop", stop, &error_fatal);
    object_property_set_bool(OBJECT(watcher), "read", read, &error_fatal);
    object_property_set_bool(OBJECT(watcher), "write", write, &error_fatal);

    Error *err = NULL;
    char *name =
        g_strdup_printf("%s.%u", TYPE_OT_DEV_PROXY_WATCHER, s->last_wid);
    object_property_add_child(OBJECT(s), name, OBJECT(watcher));

    qdev_realize_and_unref(DEVICE(watcher), s->subsys[mspc].bus, &err);
    g_free(name);
    if (err) {
        const char *msg = error_get_pretty(err);
        ot_dev_proxy_reply_error(s, PE_UNKNOWN, msg);
        error_free(err);
        return;
    }

    QSIMPLEQ_INSERT_TAIL(&s->watchers, watcher, watcher);

    uint32_t obuf[1] = { s->last_wid << 16u };

    s->last_wid += 1u;

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('m', 'i'), &obuf, sizeof(obuf));
}

static void ot_dev_proxy_release_mmio(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 3u * sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    unsigned wid = (s->rx_buffer[0] >> 16u) & 0xfffu;

    OtDevProxyWatcherState *watcher = NULL;
    OtDevProxyWatcherState *node;
    QSIMPLEQ_FOREACH(node, &s->watchers, watcher) {
        if (node->wid == wid) {
            watcher = node;
            break;
        }
    }
    if (!watcher) {
        ot_dev_proxy_reply_error(s, PE_INVALID_DEVICE_ID, "unkwown watcher");
        return;
    }

    qdev_unrealize(DEVICE(watcher));

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('m', 'r'), NULL, 0);
}


static void ot_dev_proxy_cont(OtDevProxyState *s)
{
    if (s->rx_hdr.length != 0) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    Error *err = NULL;
    qmp_cont(&err);
    if (err) {
        const char *msg = error_get_pretty(err);
        ot_dev_proxy_reply_error(s, PE_UNKNOWN, msg);
        error_free(err);
        return;
    }

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('c', 'x'), NULL, 0);
}

static void ot_dev_proxy_quit(OtDevProxyState *s)
{
    if (s->rx_hdr.length != sizeof(uint32_t)) {
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_LENGTH, NULL);
        return;
    }

    int code = (int)s->rx_buffer[0];

    ot_dev_proxy_reply_payload(s, PROXY_COMMAND('q', 't'), NULL, 0);
    usleep(200000);

    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN, code);
}

static void ot_dev_proxy_intercepted_irq(void *opaque, int irq, int level)
{
    OtDevProxyState *s = opaque;
    g_assert(irq < PROXY_IRQ_INTERCEPT_COUNT);

    OtDevProxyIrq *proxy_irq = &s->proxy_irq_map[irq];
    if (!proxy_irq->assigned) {
        trace_ot_dev_proxy_unassigned_irq(irq);
        return;
    }
    g_assert(proxy_irq->dev_num < s->dev_count);

    OtDevProxyItem *item = &s->items[proxy_irq->dev_num];

    const char *dev_name = object_get_typename(item->obj);
    Error *errp = NULL;
    char *dev_id = object_property_get_str(item->obj, "ot_id", &errp);
    if (errp) {
        error_free(errp);
    }

    trace_ot_dev_proxy_route_irq(dev_name, dev_id, proxy_irq->irq_num, level);

    ot_dev_proxy_signal(s, PROXY_COMMAND('^', 'W'), proxy_irq, level);

    g_free(dev_id);
}

static void ot_dev_proxy_notify_mmio_access(
    OtDevProxyState *s, unsigned wid, bool write, unsigned role,
    unsigned address, unsigned size, uint32_t val32)
{
    g_assert(wid < s->last_wid);

    uint32_t buffer[3] = { 0, address, val32 };

    buffer[0] |= (uint32_t)(write << 1u);
    buffer[0] |= (uint32_t)(size << 4u);
    buffer[0] |= (uint32_t)(wid << 16u);
    buffer[0] |= (uint32_t)(role << 28u);

    ot_dev_proxy_send(s, s->initiator_uid, 1, PROXY_COMMAND('^', 'R'), buffer,
                      sizeof(buffer));

    /* as a signal, do not expect a peer response */
    s->initiator_uid++;
}

static void ot_dev_proxy_dispatch_request(OtDevProxyState *s)
{
    trace_ot_dev_proxy_dispatch_request((char)(s->rx_hdr.command >> 8u),
                                        (char)(s->rx_hdr.command & 0xffu));

    switch (s->rx_hdr.command) {
    case PROXY_COMMAND('H', 'S'):
        ot_dev_proxy_handshake(s);
        break;
    case PROXY_COMMAND('E', 'D'):
        ot_dev_proxy_enumerate_devices(s);
        break;
    case PROXY_COMMAND('E', 'S'):
        ot_dev_proxy_enumerate_memory_spaces(s);
        break;
    case PROXY_COMMAND('R', 'W'):
        ot_dev_proxy_read_reg(s);
        break;
    case PROXY_COMMAND('W', 'W'):
        ot_dev_proxy_write_reg(s);
        break;
    case PROXY_COMMAND('R', 'S'):
        ot_dev_proxy_read_buffer(s, false);
        break;
    case PROXY_COMMAND('W', 'S'):
        ot_dev_proxy_write_buffer(s, false);
        break;
    case PROXY_COMMAND('R', 'X'):
        ot_dev_proxy_read_buffer(s, true);
        break;
    case PROXY_COMMAND('W', 'X'):
        ot_dev_proxy_write_buffer(s, true);
        break;
    case PROXY_COMMAND('R', 'M'):
        ot_dev_proxy_read_memory(s);
        break;
    case PROXY_COMMAND('W', 'M'):
        ot_dev_proxy_write_memory(s);
        break;
    case PROXY_COMMAND('I', 'I'):
        ot_dev_proxy_intercept_interrupts(s, true);
        break;
    case PROXY_COMMAND('I', 'R'):
        ot_dev_proxy_intercept_interrupts(s, false);
        break;
    case PROXY_COMMAND('I', 'S'):
        ot_dev_proxy_signal_interrupt(s);
        break;
    case PROXY_COMMAND('I', 'E'):
        ot_dev_proxy_enumerate_interrupts(s);
        break;
    case PROXY_COMMAND('M', 'I'):
        ot_dev_proxy_intercept_mmio(s);
        break;
    case PROXY_COMMAND('M', 'R'):
        ot_dev_proxy_release_mmio(s);
        break;
    case PROXY_COMMAND('C', 'X'):
        ot_dev_proxy_cont(s);
        break;
    case PROXY_COMMAND('Q', 'T'):
        ot_dev_proxy_quit(s);
        break;
    default:
        ot_dev_proxy_reply_error(s, PE_INVALID_COMMAND_CODE, NULL);
        break;
    }
}

static void ot_dev_proxy_dispatch_response(OtDevProxyState *s) {}

static int ot_dev_proxy_can_receive(void *opaque)
{
    OtDevProxyState *s = opaque;

    return (int)fifo8_num_free(&s->rx_fifo);
}

static void ot_dev_proxy_receive(void *opaque, const uint8_t *buf, int size)
{
    OtDevProxyState *s = opaque;

    if (fifo8_num_free(&s->rx_fifo) < size) {
        error_report("%s: Unexpected chardev receive\n", __func__);
        return;
    }

    for (unsigned ix = 0; ix < size; ix++) {
        fifo8_push(&s->rx_fifo, buf[ix]);
    }

    uint32_t length = fifo8_num_used(&s->rx_fifo);

    if (!s->rx_hdr.length) {
        /* header has not been popped out yet */
        if (length < sizeof(DevProxyHeader)) {
            /* no full header in input FIFO */
            return;
        }
        uint8_t *hdr = (uint8_t *)&s->rx_hdr;
        for (unsigned ix = 0; ix < sizeof(DevProxyHeader); ix++) {
            *hdr++ = fifo8_pop(&s->rx_fifo);
        }
        length -= sizeof(DevProxyHeader);
    }

    if (length < (uint32_t)s->rx_hdr.length) {
        /* no full command in input FIFO */
        return;
    }

    uint8_t *rxbuf = (uint8_t *)s->rx_buffer;
    for (unsigned ix = 0; ix < (unsigned)s->rx_hdr.length; ix++) {
        *rxbuf++ = fifo8_pop(&s->rx_fifo);
    }

    bool resp = (bool)(s->rx_hdr.uid >> 31u);
    unsigned uid = PROXY_UID(s->rx_hdr.uid);
    if (!resp) {
        /* request */
        if ((uid != s->requester_uid + 1u) &&
            (s->rx_hdr.command != PROXY_COMMAND('H', 'S'))) {
            trace_ot_dev_proxy_uid_error("request", s->requester_uid, uid);
            ot_dev_proxy_reply_error(s, PE_INVALID_REQUEST_ID, NULL);
        } else {
            s->requester_uid++;
            ot_dev_proxy_dispatch_request(s);
        }
    } else {
        /* response */
        if (uid != s->initiator_uid) {
            trace_ot_dev_proxy_uid_error("response", s->requester_uid, uid);
        } else {
            ot_dev_proxy_dispatch_response(s);
        }
    }

    memset(&s->rx_hdr, 0, sizeof(s->rx_hdr));
}

static gboolean ot_dev_proxy_watch_cb(void *do_not_use, GIOCondition cond,
                                      void *opaque)
{
    OtDevProxyState *s = opaque;
    (void)do_not_use;
    (void)cond;

    s->watch_tag = 0;

    return FALSE;
}

static int ot_dev_proxy_be_change(void *opaque)
{
    OtDevProxyState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, &ot_dev_proxy_can_receive,
                             &ot_dev_proxy_receive, NULL,
                             &ot_dev_proxy_be_change, s, NULL, true);

    if (s->watch_tag > 0) {
        g_source_remove(s->watch_tag);
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             &ot_dev_proxy_watch_cb, s);
    }

    return 0;
}

static int ot_dev_proxy_discover_device(Object *child, void *opaque)
{
    GArray *array = opaque;

    for (unsigned ix = 0; ix < ARRAY_SIZE(SUPPORTED_DEVICES); ix++) {
        const OtDevProxyDevice *pd = &SUPPORTED_DEVICES[ix];
        if (object_dynamic_cast(child, pd->typename)) {
            (*pd->reg_dev)(array, child);
            return 0;
        }
    }
    if (object_dynamic_cast(child, TYPE_MEMORY_REGION)) {
        ot_dev_proxy_reg_mr(array, child);
        return 0;
    }

    return 0;
}

static void ot_dev_proxy_reg_mr(GArray *array, Object *obj)
{
    MemoryRegion *mr = MEMORY_REGION(obj);
    if (mr->ram && obj->parent) {
        if (!object_dynamic_cast(obj->parent, TYPE_OT_SRAM_CTRL)) {
            OtDevProxyItem *item = g_new0(OtDevProxyItem, 1);
            object_ref(obj);
            item->obj = obj;
            item->caps.mr = mr;
            g_assert(item->caps.mr);
            item->caps.reg_count = memory_region_size(mr) / sizeof(uint32_t);
            item->prefix = "M/";
            g_array_append_val(array, item);
        }
    }
}

static void ot_dev_proxy_reg_mbx(GArray *array, Object *obj)
{
    SysBusDevice *sysdev = SYS_BUS_DEVICE(obj);
    g_assert(sysdev->num_mmio == 2u);
    OtDevProxyItem *item;
    /* host side */
    item = g_new0(OtDevProxyItem, 1);
    object_ref(obj);
    item->obj = obj;
    item->caps.mr = sysdev->mmio[0u].memory; /* 0: host */
    item->caps.reg_count = OT_MBX_HOST_REGS_COUNT;
    item->caps.irq_mask = UINT32_MAX; /* all IRQs can be routed */
    item->prefix = "MBH/";
    g_array_append_val(array, item);
    /* sys side */
    item = g_new0(OtDevProxyItem, 1);
    object_ref(obj);
    item->obj = obj;
    item->caps.mr = sysdev->mmio[1u].memory; /* 1: sys */
    item->caps.reg_count = OT_MBX_SYS_REGS_COUNT;
    item->caps.irq_mask = 0; /* no IRQ on sys side */
    item->prefix = "MBS/";
    g_array_append_val(array, item);
}

static void ot_dev_proxy_reg_soc_proxy(GArray *array, Object *obj)
{
    OtDevProxyItem *item = g_new0(OtDevProxyItem, 1);
    object_ref(obj);
    item->obj = obj;
    SysBusDevice *sysdev = SYS_BUS_DEVICE(obj);
    g_assert(sysdev->num_mmio == 1u);
    item->caps.mr = sysdev->mmio[0u].memory;
    item->caps.reg_count = OT_SOC_PROXY_REGS_COUNT; /* per slot */
    item->caps.irq_mask = UINT32_MAX; /* all IRQs can be routed */
    item->prefix = "SOC/";
    g_array_append_val(array, item);
}

static void ot_dev_proxy_reg_sram_ctrl(GArray *array, Object *obj)
{
    SysBusDevice *sysdev = SYS_BUS_DEVICE(obj);
    if (sysdev->mmio[0].memory && /* ctrl */
        sysdev->mmio[1].memory /* mem */) {
        OtDevProxyItem *item;
        item = g_new0(OtDevProxyItem, 1);
        object_ref(obj);
        item->obj = obj;
        item->caps.mr = sysdev->mmio[0].memory;
        item->caps.reg_count =
            memory_region_size(item->caps.mr) / sizeof(uint32_t);
        item->prefix = "SRC/"; /* SRAM control */
        g_array_append_val(array, item);
        item = g_new0(OtDevProxyItem, 1);
        object_ref(obj);
        item->obj = obj;
        item->caps.mr = sysdev->mmio[1].memory;
        item->caps.reg_count =
            memory_region_size(item->caps.mr) / sizeof(uint32_t);
        item->prefix = "SRM/"; /* SRAM memory */
        g_array_append_val(array, item);
    }
}

static int ot_dev_proxy_discover_memory_root(Object *child, void *opaque)
{
    GArray *array = opaque;

    if (object_dynamic_cast(child, TYPE_MEMORY_REGION)) {
        MemoryRegion *mr = MEMORY_REGION(child);
        /*
         * This is a hack. A proper implementation would require to search
         * the address spaces for memory root regions, unfortunately QEMU APIs
         * do not expose address_spaces, which are hidden in memory.c
         */
        if (mr->container || mr->ram || mr->mapped_via_alias) {
            /* not a root memory region */
            return 0;
        }
        if (memory_region_size(mr) == 0) {
            /* empty region, useless */
            return 0;
        }
        if (mr->addr != 0) {
            /* not supported for a root region */
            return 0;
        }
        const char *name = memory_region_name(mr);
        if (!g_strcmp0(name, "io")) {
            /*
             * io region is a legacy region that is automatically created and
             * useless (should be ignored)
             */
            return 0;
        }
        g_array_append_val(array, mr);
    }

    return 0;
}

static int ot_dev_proxy_find_bus(Object *child, void *opaque)
{
    if (object_dynamic_cast(child, TYPE_BUS)) {
        BusState **bus = opaque;
        *bus = BUS(child);
        return 1;
    }

    return 0;
}

static int ot_dev_proxy_map_bus(Object *child, void *opaque)
{
    if (!object_dynamic_cast(child, TYPE_CPU)) {
        return 0;
    }

    OtDevProxyState *s = opaque;
    CPUState *cpu = CPU(child);
    MemoryRegion *mr = cpu->memory;

    for (unsigned ix = 0; ix < s->subsys_count; ix++) {
        OtDevProxySystem *sys = &s->subsys[ix];
        if (sys->bus) {
            continue;
        }
        if (sys->mr == mr) {
            Object *obj = OBJECT(&cpu->parent_obj)->parent;
            BusState *bus = NULL;
            while (obj) {
                object_child_foreach(obj, &ot_dev_proxy_find_bus, &bus);
                if (bus) {
                    sys->bus = bus;
                    break;
                }
                obj = obj->parent;
            }
        }
    }

    return 0;
}

static gint ot_dev_proxy_device_compare(gconstpointer a, gconstpointer b)
{
    const OtDevProxyItem **ia = (const OtDevProxyItem **)a;
    const OtDevProxyItem **ib = (const OtDevProxyItem **)b;

    const MemoryRegion *ma = (*ia)->caps.mr;
    const MemoryRegion *mb = (*ib)->caps.mr;

    /* increasing host addresses */
    return (gint)(ma->addr > mb->addr);
}

static void ot_dev_proxy_discover(OtDevProxyState *s)
{
    Object *ms = qdev_get_machine();
    /* search for 'proxyfi-able' devices */
    GArray *array;

    array = g_array_new(FALSE, TRUE, sizeof(OtDevProxyItem *));
    object_child_foreach_recursive(ms, &ot_dev_proxy_discover_device, array);

    g_array_sort(array, &ot_dev_proxy_device_compare);

    s->dev_count = array->len;
    s->items = g_new0(OtDevProxyItem, s->dev_count);

    for (unsigned ix = 0; ix < array->len; ix++) {
        OtDevProxyItem *item;
        item = (OtDevProxyItem *)(g_array_index(array, OtDevProxyItem *, ix));
        /* deep copy */
        s->items[ix] = *item;
        /* allocated from ot_dev_proxy_discover */
        g_free(item);
    }
    g_array_free(array, TRUE);

    s->proxy_irq_map = g_new0(OtDevProxyIrq, PROXY_IRQ_INTERCEPT_COUNT);
    qdev_init_gpio_in_named(DEVICE(s), &ot_dev_proxy_intercepted_irq,
                            PROXY_IRQ_INTERCEPT_NAME,
                            PROXY_IRQ_INTERCEPT_COUNT);

    array = g_array_new(FALSE, TRUE, sizeof(MemoryRegion *));
    object_child_foreach_recursive(ms, &ot_dev_proxy_discover_memory_root,
                                   array);

    s->subsys_count = array->len;
    s->subsys = g_new0(OtDevProxySystem, s->subsys_count);
    for (unsigned ix = 0; ix < array->len; ix++) {
        MemoryRegion *mr;
        mr = (MemoryRegion *)(g_array_index(array, MemoryRegion *, ix));
        s->subsys[ix].mr = mr;
        object_ref(OBJECT(mr));
    }
    g_array_free(array, TRUE);

    object_child_foreach_recursive(ms, &ot_dev_proxy_map_bus, s);
}

static Property ot_dev_proxy_properties[] = {
    DEFINE_PROP_CHR("chardev", OtDevProxyState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void ot_dev_proxy_reset(DeviceState *dev)
{
    OtDevProxyState *s = OT_DEV_PROXY(dev);

    if (!s->items) {
        /* only done once */
        ot_dev_proxy_discover(s);
    }

    fifo8_reset(&s->rx_fifo);
    memset(&s->rx_hdr, 0, sizeof(s->rx_hdr));
    s->requester_uid = 0;
    s->initiator_uid = 0;
}

static void ot_dev_proxy_realize(DeviceState *dev, Error **errp)
{
    OtDevProxyState *s = OT_DEV_PROXY(dev);
    (void)errp;

    qemu_chr_fe_set_handlers(&s->chr, &ot_dev_proxy_can_receive,
                             &ot_dev_proxy_receive, NULL,
                             &ot_dev_proxy_be_change, s, NULL, true);
}

static void ot_dev_proxy_init(Object *obj)
{
    OtDevProxyState *s = OT_DEV_PROXY(obj);

    fifo8_create(&s->rx_fifo, 256u);
    s->rx_buffer = g_new(uint32_t, 256u / sizeof(uint32_t));
    QSIMPLEQ_INIT(&s->watchers);
}

static void ot_dev_proxy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->reset = &ot_dev_proxy_reset;
    dc->realize = &ot_dev_proxy_realize;
    device_class_set_props(dc, ot_dev_proxy_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_dev_proxy_info = {
    .name = TYPE_OT_DEV_PROXY,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtDevProxyState),
    .instance_init = &ot_dev_proxy_init,
    .class_init = &ot_dev_proxy_class_init,
};

static void ot_dev_proxy_register_types(void)
{
    type_register_static(&ot_dev_proxy_info);
}

type_init(ot_dev_proxy_register_types);

/* ------------------------------------------------------------------------ */
/* OtDevProxyStateWatcher */
/* ------------------------------------------------------------------------ */

static MemTxResult ot_dev_proxy_watcher_read_with_attrs(
    void *opaque, hwaddr addr, uint64_t *val64, unsigned size, MemTxAttrs attrs)
{
    OtDevProxyWatcherState *s = opaque;

    if (s->read && s->stop) {
        unsigned role = MEMTXATTRS_GET_ROLE(attrs);
        uint32_t address = s->address + (uint32_t)addr;
        s->stop -= 1u;
        ot_dev_proxy_notify_mmio_access(s->devproxy, s->wid, false, role,
                                        address, size, 0);
    }

    *val64 = 0;
    return MEMTX_OK;
}

static MemTxResult ot_dev_proxy_watcher_write_with_attrs(
    void *opaque, hwaddr addr, uint64_t val64, unsigned size, MemTxAttrs attrs)
{
    OtDevProxyWatcherState *s = opaque;

    if (s->write && s->stop) {
        unsigned role = MEMTXATTRS_GET_ROLE(attrs);
        uint32_t address = s->address + (uint32_t)addr;
        ot_dev_proxy_notify_mmio_access(s->devproxy, s->wid, true, role,
                                        address, size, (uint32_t)val64);
        s->stop -= 1u;
    }

    return MEMTX_OK;
}

static Property ot_dev_proxy_watcher_properties[] = {
    DEFINE_PROP_LINK("devproxy", OtDevProxyWatcherState, devproxy,
                     TYPE_OT_DEV_PROXY, OtDevProxyState *),
    DEFINE_PROP_LINK("root", OtDevProxyWatcherState, root, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("wid", OtDevProxyWatcherState, wid, UINT32_MAX),
    DEFINE_PROP_UINT32("address", OtDevProxyWatcherState, address, UINT32_MAX),
    DEFINE_PROP_UINT32("size", OtDevProxyWatcherState, size, 0),
    DEFINE_PROP_UINT32("priority", OtDevProxyWatcherState, priority, 1),
    DEFINE_PROP_UINT32("stop", OtDevProxyWatcherState, stop, UINT32_MAX),
    DEFINE_PROP_BOOL("read", OtDevProxyWatcherState, read, UINT32_MAX),
    DEFINE_PROP_BOOL("write", OtDevProxyWatcherState, write, UINT32_MAX),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_dev_proxy_watcher_ops = {
    .read_with_attrs = &ot_dev_proxy_watcher_read_with_attrs,
    .write_with_attrs = &ot_dev_proxy_watcher_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1u,
    .impl.max_access_size = 4u,
};

static void ot_dev_proxy_watcher_realize(DeviceState *dev, Error **errp)
{
    OtDevProxyWatcherState *s = OT_DEV_PROXY_WATCHER(dev);
    (void)errp;

    g_assert(s->devproxy);
    g_assert(s->root);
    g_assert(s->wid != UINT32_MAX);
    g_assert(s->address != UINT32_MAX);
    g_assert(s->size != 0);

    char *name = g_strdup_printf("%s.%u", TYPE_OT_DEV_PROXY_WATCHER, s->wid);
    memory_region_init_io(&s->mmio, OBJECT(dev), &ot_dev_proxy_watcher_ops, s,
                          name, s->size);
    g_free(name);
    memory_region_add_subregion_overlap(s->root, (hwaddr)s->address, &s->mmio,
                                        (int)s->priority);
}

static void ot_dev_proxy_watcher_unrealize(DeviceState *dev)
{
    OtDevProxyWatcherState *s = OT_DEV_PROXY_WATCHER(dev);
    OtDevProxyState *proxy = s->devproxy;

    memory_region_del_subregion(s->root, &s->mmio);

    /* remove self from proxy watcher list */
    QSIMPLEQ_REMOVE(&proxy->watchers, s, OtDevProxyWatcherState, watcher);
}

static void ot_dev_proxy_watcher_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_dev_proxy_watcher_realize;
    dc->unrealize = &ot_dev_proxy_watcher_unrealize;
    device_class_set_props(dc, ot_dev_proxy_watcher_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_dev_proxy_watcher_info = {
    .name = TYPE_OT_DEV_PROXY_WATCHER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtDevProxyWatcherState),
    .class_init = &ot_dev_proxy_watcher_class_init,
};

static void ot_dev_proxy_watcher_register_types(void)
{
    type_register_static(&ot_dev_proxy_watcher_info);
}

type_init(ot_dev_proxy_watcher_register_types);
