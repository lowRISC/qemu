/*
 * QEMU RISC-V Helpers for LowRISC Ibex Demo System & OpenTitan EarlGrey
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "chardev/chardev-internal.h"
#include "cpu.h"
#include "disas/disas.h"
#include "elf.h"
#include "exec/hwaddr.h"
#include "hw/boards.h"
#include "hw/core/rust_demangle.h"
#include "hw/loader.h"
#include "hw/misc/unimp.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/ibex_common.h"
#include "hw/sysbus.h"
#include "monitor/monitor.h"
#include "sysemu/runstate.h"

static void rust_demangle_fn(const char *st_name, int st_info,
                             uint64_t st_value, uint64_t st_size);

static void ibex_mmio_map_device(SysBusDevice *dev, MemoryRegion *mr,
                                 unsigned nr, hwaddr addr, int priority)
{
    g_assert(nr < dev->num_mmio);
    g_assert(dev->mmio[nr].addr == (hwaddr)-1);
    dev->mmio[nr].addr = addr;
    if (priority) {
        memory_region_add_subregion_overlap(mr, addr, dev->mmio[nr].memory,
                                            priority);
    } else {
        memory_region_add_subregion(mr, addr, dev->mmio[nr].memory);
    }
}

DeviceState **ibex_create_devices(const IbexDeviceDef *defs, unsigned count,
                                  DeviceState *parent)
{
    DeviceState **devices = g_new0(DeviceState *, count);
    unsigned unimp_count = 0;
    for (unsigned idx = 0; idx < count; idx++) {
        const IbexDeviceDef *def = &defs[idx];
        if (!def->type) {
            devices[idx] = NULL;
            continue;
        }
        devices[idx] = qdev_new(def->type);

        char *name;
        if (!strcmp(def->type, TYPE_UNIMPLEMENTED_DEVICE)) {
            if (def->name) {
                name = g_strdup_printf("%s[%u]", def->name, def->instance);
            } else {
                name = g_strdup_printf(TYPE_UNIMPLEMENTED_DEVICE "[%u]",
                                       unimp_count);
            }
            unimp_count += 1u;
        } else {
            name = g_strdup_printf("%s[%u]", def->type, def->instance);
        }
        object_property_add_child(OBJECT(parent), name, OBJECT(devices[idx]));
        g_free(name);
    }
    return devices;
}

void ibex_link_remote_devices(DeviceState **devices, const IbexDeviceDef *defs,
                              unsigned count, DeviceState ***remotes)
{
    /* Link devices */
    DeviceState ***targets = remotes ?: &devices;
    for (unsigned idx = 0; idx < count; idx++) {
        DeviceState *dev = devices[idx];
        if (!dev) {
            continue;
        }
        const IbexDeviceLinkDef *link = defs[idx].link;
        if (link) {
            while (link->propname) {
                unsigned rix = IBEX_DEVLINK_REMOTE(link->index);
                unsigned dix = IBEX_DEVLINK_DEVICE(link->index);
                if (rix && !remotes) {
                    /*
                     * if no remote devices are specified, only local links
                     * can be performed, skip any remote definition.
                     */
                    link++;
                    continue;
                }
                if (!rix && remotes) {
                    /*
                     * if remote devices are specified, only remote links
                     * should be performed, skip any local definition.
                     */
                    link++;
                    continue;
                }
                DeviceState **tdevices = targets[rix];
                g_assert(tdevices);
                DeviceState *target = tdevices[dix];
                g_assert(target);
                object_property_set_link(OBJECT(dev), link->propname,
                                         OBJECT(target), &error_fatal);
                g_autofree char *plink;
                plink = object_property_get_str(OBJECT(dev), link->propname,
                                                &error_fatal);
                if (!plink || *plink == '\0') {
                    /*
                     * unfortunately, if an object is not parented, it is not
                     * possible to create a link (its canonical being NULL), but
                     * the `object_property_set_link` silently fails. Read back
                     * the property to ensure it has been really set.
                     */
                    error_setg(&error_fatal, "cannot create %s link",
                               link->propname);
                }
                link++;
            }
        }
    }
}

void ibex_apply_device_props(Object *obj, const IbexDevicePropDef *prop)
{
    if (prop) {
        while (prop->propname) {
            switch (prop->type) {
            case IBEX_PROP_TYPE_BOOL:
                object_property_set_bool(obj, prop->propname, prop->b,
                                         &error_fatal);
                break;
            case IBEX_PROP_TYPE_INT:
                object_property_set_int(obj, prop->propname, prop->i,
                                        &error_fatal);
                break;
            case IBEX_PROP_TYPE_UINT:
                object_property_set_uint(obj, prop->propname, prop->u,
                                         &error_fatal);
                break;
            case IBEX_PROP_TYPE_STR:
                object_property_set_str(obj, prop->propname, prop->s,
                                        &error_fatal);
                break;
            default:
                g_assert_not_reached();
                break;
            }
            prop++;
        }
    }
}

void ibex_define_device_props(DeviceState **devices, const IbexDeviceDef *defs,
                              unsigned count)
{
    for (unsigned idx = 0; idx < count; idx++) {
        DeviceState *dev = devices[idx];
        if (!dev) {
            continue;
        }
        ibex_apply_device_props(OBJECT(dev), defs[idx].prop);
    }
}

void ibex_realize_system_devices(DeviceState **devices,
                                 const IbexDeviceDef *defs, unsigned count)
{
    BusState *bus = sysbus_get_default();

    ibex_realize_devices(devices, bus, defs, count);

    MemoryRegion *mrs[] = { get_system_memory(), NULL, NULL, NULL };

    ibex_map_devices(devices, mrs, defs, count);
}

void ibex_realize_devices(DeviceState **devices, BusState *bus,
                          const IbexDeviceDef *defs, unsigned count)
{
    /* Realize devices */
    for (unsigned idx = 0; idx < count; idx++) {
        DeviceState *dev = devices[idx];
        if (!dev) {
            continue;
        }
        const IbexDeviceDef *def = &defs[idx];

        if (def->cfg) {
            def->cfg(dev, def, DEVICE(OBJECT(dev)->parent));
        }

        if (def->memmap) {
            SysBusDevice *busdev =
                (SysBusDevice *)object_dynamic_cast(OBJECT(dev),
                                                    TYPE_SYS_BUS_DEVICE);
            if (!busdev) {
                /* non-sysbus devices are not supported for now */
                g_assert_not_reached();
            }

            qdev_realize_and_unref(DEVICE(busdev), bus, &error_fatal);
        } else {
            /* device is not connected to a bus */
            qdev_realize_and_unref(dev, NULL, &error_fatal);
        }
    }
}

void ibex_map_devices_mask_offset(DeviceState **devices, MemoryRegion **mrs,
                                  const IbexDeviceDef *defs, unsigned count,
                                  uint32_t region_mask, uint32_t offset)
{
    for (unsigned idx = 0; idx < count; idx++) {
        DeviceState *dev = devices[idx];
        if (!dev) {
            continue;
        }
        const IbexDeviceDef *def = &defs[idx];

        if (def->memmap) {
            SysBusDevice *busdev =
                (SysBusDevice *)object_dynamic_cast(OBJECT(dev),
                                                    TYPE_SYS_BUS_DEVICE);
            if (busdev) {
                const IbexMemMapEntry *memmap = def->memmap;
                unsigned mem = 0;
                while (!IBEX_MEMMAP_IS_LAST(memmap)) {
                    unsigned region = IBEX_MEMMAP_GET_REGIDX(memmap->base);
                    if (region_mask & (1u << region)) {
                        MemoryRegion *mr = mrs[region];
                        if (mr) {
                            ibex_mmio_map_device(busdev, mr, mem,
                                                 IBEX_MEMMAP_GET_ADDRESS(
                                                     memmap->base) +
                                                     offset,
                                                 memmap->priority);
                        }
                    }
                    mem++;
                    memmap++;
                }
            }
        }
    }
}

void ibex_map_devices_ext_mask_offset(
    DeviceState *dev, MemoryRegion **mrs, const IbexDeviceMapDef *defs,
    unsigned count, uint32_t region_mask, uint32_t offset)
{
    for (unsigned ix = 0; ix < count; ix++) {
        const IbexDeviceMapDef *def = &defs[ix];
        g_assert(def->type);
        g_assert(def->memmap);

        char *name = g_strdup_printf("%s[%u]", def->type, def->instance);
        Object *child;
        child = object_property_get_link(OBJECT(dev), name, &error_fatal);
        SysBusDevice *sdev;
        sdev = OBJECT_CHECK(SysBusDevice, child, TYPE_SYS_BUS_DEVICE);
        g_free(name);

        const IbexMemMapEntry *memmap = def->memmap;
        unsigned mem = 0;
        while (!IBEX_MEMMAP_IS_LAST(memmap)) {
            if (!IBEX_MEMMAP_IGNORE(memmap)) {
                unsigned region = IBEX_MEMMAP_GET_REGIDX(memmap->base);
                if (region_mask & (1u << region)) {
                    MemoryRegion *mr = mrs[region];
                    if (mr) {
                        ibex_mmio_map_device(sdev, mr, mem,
                                             IBEX_MEMMAP_GET_ADDRESS(
                                                 memmap->base) +
                                                 offset,
                                             memmap->priority);
                    }
                }
            }
            mem++;
            memmap++;
        }
    }
}

void ibex_connect_devices(DeviceState **devices, const IbexDeviceDef *defs,
                          unsigned count)
{
    /* Connect GPIOs (in particular, IRQs) */
    for (unsigned idx = 0; idx < count; idx++) {
        DeviceState *dev = devices[idx];
        if (!dev) {
            continue;
        }
        const IbexDeviceDef *def = &defs[idx];

        if (def->gpio) {
            const IbexGpioConnDef *conn = def->gpio;
            while (conn->out.num >= 0 && conn->in.num >= 0) {
                if (conn->in.index >= 0) {
                    unsigned in_ix = IBEX_GPIO_GET_IDX(conn->in.index);
                    g_assert(devices[in_ix]);
                    qemu_irq in_gpio =
                        qdev_get_gpio_in_named(devices[in_ix], conn->in.name,
                                               conn->in.num);
                    if (!in_gpio) {
                        error_setg(&error_fatal, "no such GPIO '%s.%s[%d]'\n",
                                   object_get_typename(OBJECT(devices[in_ix])),
                                   conn->in.name, conn->in.num);
                    }
                    qdev_connect_gpio_out_named(dev, conn->out.name,
                                                conn->out.num, in_gpio);
                }
                conn++;
            }
        }
    }
}

/* List of exported GPIOs */
typedef QLIST_HEAD(, NamedGPIOList) IbexXGPIOList;

static NamedGPIOList *ibex_xgpio_list(IbexXGPIOList *xgpios, const char *name)
{
    /*
     * qdev_get_named_gpio_list is not a public API.
     * Use a clone implementation to manage a list of GPIOs
     */
    NamedGPIOList *ngl;

    QLIST_FOREACH(ngl, xgpios, node) {
        /* NULL is a valid and matchable name. */
        if (g_strcmp0(name, ngl->name) == 0) {
            return ngl;
        }
    }

    ngl = g_malloc0(sizeof(*ngl));
    ngl->name = g_strdup(name);
    QLIST_INSERT_HEAD(xgpios, ngl, node);
    return ngl;
}

void ibex_export_gpios(DeviceState **devices, DeviceState *parent,
                       const IbexDeviceDef *defs, unsigned count)
{
    /*
     * Use IbexXGPIOList as a circomvoluted way to obtain IRQ information that
     * may not be exposed through public APIs.
     */
    IbexXGPIOList pgpios = { 0 };

    for (unsigned idx = 0; idx < count; idx++) {
        DeviceState *dev = devices[idx];
        if (!dev) {
            continue;
        }
        const IbexDeviceDef *def = &defs[idx];

        if (def->gpio_export) {
            const IbexGpioExportDef *export;

            /* loop once to compute how the number of GPIO for each GPIO list */
            export = def->gpio_export;
            while (export->device.num >= 0 && export->parent.num >= 0) {
                const char *pname = export->parent.name;
                NamedGPIOList *pngl = ibex_xgpio_list(&pgpios, pname);

                pngl->num_in = MAX(pngl->num_in, export->parent.num);

                export ++;
            }

            NamedGPIOList *ngl, *ntmp;
            QLIST_FOREACH_SAFE(ngl, &pgpios, node, ntmp) {
                NamedGPIOList *pngl;
                QLIST_FOREACH(pngl, &parent->gpios, node) {
                    if (!g_strcmp0(ngl->name, pngl->name)) {
                        qemu_log("%s: duplicate GPIO export list %s for %s\n",
                                 __func__, ngl->name,
                                 object_get_typename(OBJECT(parent)));
                        g_assert_not_reached();
                    }
                }
                if (ngl->num_in) {
                    /* num_in is the max index, i.e. n-1 */
                    ngl->num_in += 1u;
                    ngl->in = g_new(qemu_irq, ngl->num_in);
                }
                QLIST_REMOVE(ngl, node);
                QLIST_INSERT_HEAD(&parent->gpios, ngl, node);
            }

            /*
             * Now the count of IRQ slots per IRQ name is known.
             * Allocate the required slots to store IRQs
             * Create alias from parent to devices
             * Shallow copy device IRQ into parent's slots, as we do not want
             * to alter the device IRQ list.
             */
            export = def->gpio_export;
            ngl = NULL;
            while (export->device.num >= 0 && export->parent.num >= 0) {
                const char *defname = "unnamed-gpio-in";
                const char *dname = export->device.name;
                const char *dm = dname ? dname : defname;
                char *dpname =
                    g_strdup_printf("%s[%d]", dm, export->device.num);
                const char *pname = export->parent.name;
                const char *pm = pname ? pname : defname;
                char *ppname =
                    g_strdup_printf("%s[%d]", pm, export->parent.num);
                qemu_irq devirq;
                devirq = qdev_get_gpio_in_named(dev, export->device.name,
                                                export->device.num);
                if (!ngl || g_strcmp0(ngl->name, pname)) {
                    ngl = NULL;
                    NamedGPIOList *pngl;
                    QLIST_FOREACH(pngl, &parent->gpios, node) {
                        if (!g_strcmp0(pngl->name, pname)) {
                            ngl = pngl;
                            break;
                        }
                    }
                }
                assert(ngl && ngl->in);
                ngl->in[export->parent.num] = devirq;
                (void)object_ref(OBJECT(devirq));
                object_property_add_alias(OBJECT(parent), ppname, OBJECT(dev),
                                          dpname);
                g_free(ppname);
                g_free(dpname);
                export ++;
            }
        }
    }
}

void ibex_connect_soc_devices(DeviceState **soc_devices, DeviceState **devices,
                              const IbexDeviceDef *defs, unsigned count)
{
    for (unsigned ix = 0; ix < count; ix++) {
        DeviceState *dev = devices[ix];
        if (!dev) {
            continue;
        }
        const IbexDeviceDef *def = &defs[ix];
        if (!def->type || !def->gpio) {
            continue;
        }

        const IbexGpioConnDef *conn = def->gpio;
        while (conn->out.num >= 0 && conn->in.num >= 0) {
            if (conn->in.index < 0) {
                unsigned grp = IBEX_GPIO_GET_GRP(conn->in.num);
                if (!(grp < count)) {
                    g_assert_not_reached();
                }
                DeviceState *socdev = soc_devices[grp];
                unsigned in_ix = IBEX_GPIO_GET_IDX(conn->in.num);
                qemu_irq in_gpio =
                    qdev_get_gpio_in_named(socdev, conn->in.name, (int)in_ix);
                if (!in_gpio) {
                    error_setg(
                        &error_fatal,
                        "cannot connect %s.%s[%d], no such IRQ '%s.%s[%d]'\n",
                        object_get_typename(OBJECT(dev)), conn->out.name,
                        conn->out.num, object_get_typename(OBJECT(socdev)),
                        conn->in.name, in_ix);
                }
                qdev_connect_gpio_out_named(dev, conn->out.name, conn->out.num,
                                            in_gpio);
            }
            conn++;
        }
    }
}

void ibex_identify_devices(DeviceState **devices, const char *id_prop,
                           const char *id_value, bool id_prepend,
                           unsigned count)
{
    for (unsigned ix = 0; ix < count; ix++) {
        DeviceState *dev = devices[ix];
        if (!dev) {
            continue;
        }
        Object *obj = OBJECT(dev);
        /* check if the device defines an identifcation string property */
        char *value = object_property_get_str(obj, id_prop, NULL);
        if (!value) {
            continue;
        }

        bool is_set = (bool)strcmp(value, "");
        if (is_set && !id_prepend) {
            /* do not override already defined property */
            g_free(value);
            continue;
        }

        bool res;
        if (is_set && id_prepend) {
            char *pvalue = g_strconcat(id_value, ".", value, NULL);
            res = object_property_set_str(obj, id_prop, pvalue, NULL);
            g_free(pvalue);
        } else {
            res = object_property_set_str(obj, id_prop, id_value, NULL);
        }
        g_free(value);
        if (!res) {
            error_report("%s: cannot apply identifier to %s\n", __func__,
                         object_get_typename(obj));
        }
    }
}

void ibex_configure_devices_with_id(DeviceState **devices, BusState *bus,
                                    const char *id_prop, const char *id_value,
                                    bool id_prepend, const IbexDeviceDef *defs,
                                    unsigned count)
{
    ibex_link_devices(devices, defs, count);
    ibex_define_device_props(devices, defs, count);
    if (id_prop && id_value) {
        ibex_identify_devices(devices, id_prop, id_value, id_prepend, count);
    }
    ibex_realize_devices(devices, bus, defs, count);
    ibex_connect_devices(devices, defs, count);
}

void ibex_configure_devices(DeviceState **devices, BusState *bus,
                            const IbexDeviceDef *defs, unsigned count)
{
    ibex_configure_devices_with_id(devices, bus, NULL, NULL, false, defs,
                                   count);
}

typedef struct {
    Object *child;
    const char *typename;
    unsigned instance;
} IbexChildMatch;

static int ibex_match_device(Object *child, void *opaque)
{
    IbexChildMatch *match = opaque;

    if (!object_dynamic_cast(child, match->typename)) {
        return 0;
    }
    if (match->instance) {
        match->instance -= 1;
        return 0;
    }

    match->child = child;
    return 1;
}

DeviceState *ibex_get_child_device(DeviceState *s, const char *typename,
                                   unsigned instance)
{
    IbexChildMatch match = {
        .child = NULL,
        .typename = typename,
        .instance = instance,
    };

    if (!object_child_foreach(OBJECT(s), &ibex_match_device, &match)) {
        return NULL;
    }

    if (!object_dynamic_cast(match.child, TYPE_DEVICE)) {
        return NULL;
    }

    return DEVICE(match.child);
}

typedef struct {
    Chardev *chr;
    const char *label;
} IbexChrMatch;

static int ibex_match_chardev(Object *child, void *opaque)
{
    IbexChrMatch *match = opaque;
    Chardev *chr = CHARDEV(child);

    if (strcmp(match->label, chr->label) != 0) {
        return 0;
    }

    match->chr = chr;
    return 1;
}

Chardev *ibex_get_chardev_by_id(const char *chrid)
{
    IbexChrMatch match = {
        .chr = NULL,
        .label = chrid,
    };

    /* "chardev-internal.h" inclusion is required for get_chardevs_root() */
    if (!object_child_foreach(get_chardevs_root(), &ibex_match_chardev,
                              &match)) {
        return NULL;
    }

    return match.chr;
}

void ibex_unimp_configure(DeviceState *dev, const IbexDeviceDef *def,
                          DeviceState *parent)
{
    (void)parent;

    if (def->name) {
        qdev_prop_set_string(dev, "name", def->name);
    }
    g_assert(def->memmap != NULL);
}

uint32_t ibex_load_kernel(CPUState *cpu)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    uint64_t kernel_entry;
    /* load kernel if provided */
    if (ms->kernel_filename) {
        AddressSpace *as = NULL;
        if (!cpu) {
            CPUState *cs;
            /* NOLINTNEXTLINE */
            CPU_FOREACH(cs) {
                if (cs->as) {
                    as = cs->as;
                    break;
                }
            }
        } else {
            as = cpu->as;
        }
        g_assert(as);
        if (load_elf_ram_sym(ms->kernel_filename, NULL, NULL, NULL,
                             &kernel_entry, NULL, NULL, NULL, 0, EM_RISCV, 1, 0,
                             as, true, &rust_demangle_fn) <= 0) {
            error_report("Cannot load ELF kernel %s", ms->kernel_filename);
            qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_HOST_ERROR,
                                                   EXIT_FAILURE);
        }

        if (((uint32_t)kernel_entry & 0xFFu) != 0x80u) {
            qemu_log("%s: invalid kernel entry address 0x%08x\n", __func__,
                     (uint32_t)kernel_entry);
        }

        kernel_entry &= ~0xFFull;
        Error *errp = NULL;
        bool no_set_pc =
            object_property_get_bool(OBJECT(ms), "ignore-elf-entry", &errp);
        if (!no_set_pc) {
            if (!cpu) {
                CPUState *cs;
                /* NOLINTNEXTLINE */
                CPU_FOREACH(cs) {
                    RISCV_CPU(cs)->env.resetvec = kernel_entry | 0x80ull;
                    RISCV_CPU(cs)->cfg.mtvec = kernel_entry | 0b1ull;
                }
            } else {
                RISCV_CPU(cpu)->env.resetvec = kernel_entry | 0x80ull;
                RISCV_CPU(cpu)->cfg.mtvec = kernel_entry | 0b1ull;
            }
        }
    } else {
        kernel_entry = UINT64_MAX;
    }

    return (uint32_t)kernel_entry;
}

uint32_t ibex_get_current_pc(void)
{
    CPUState *cs = current_cpu;

    return cs && cs->cc->get_pc ? (uint32_t)cs->cc->get_pc(cs) : 0u;
}

int ibex_get_current_cpu(void)
{
    CPUState *cs = current_cpu;

    return cs ? cs->cpu_index : -1;
}

/* x0 is replaced with PC */
static const char ibex_ireg_names[32u][4u] = {
    "pc", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
    "a1", "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

void ibex_log_vcpu_registers(uint64_t regbm)
{
    CPURISCVState *env = &RISCV_CPU(current_cpu)->env;
    qemu_log_mask(CPU_LOG_TB_IN_ASM, "\n....\n");
    if (regbm & 0x1u) {
        qemu_log_mask(CPU_LOG_TB_IN_ASM, "%4s: 0x" TARGET_FMT_lx "\n",
                      ibex_ireg_names[0], env->pc);
    }
    for (unsigned gix = 1u; gix < 32u; gix++) {
        uint64_t mask = 1u << gix;
        if (regbm & mask) {
            qemu_log_mask(CPU_LOG_TB_IN_ASM, "%4s: 0x" TARGET_FMT_lx "\n",
                          ibex_ireg_names[gix], env->gpr[gix]);
        }
    }
}

static void rust_demangle_fn(const char *st_name, int st_info,
                             uint64_t st_value, uint64_t st_size)
{
    (void)st_info;
    (void)st_value;

    if (!st_size) {
        return;
    }

    rust_demangle_replace((char *)st_name);
}

/*
 * Note: this is not specific to Ibex, and might apply to any vCPU.
 */
static void hmp_info_ibex(Monitor *mon, const QDict *qdict)
{
    CPUState *cpu;
    (void)qdict;

    /* NOLINTNEXTLINE */
    CPU_FOREACH(cpu) {
        vaddr pc;
        const char *symbol;
        const char *cpu_state;
        if (cpu->cc->get_pc) {
            pc = cpu->cc->get_pc(cpu);
            symbol = lookup_symbol(pc);
        } else {
            pc = -1;
            symbol = "?";
        }
        if (cpu->halted && cpu->held_in_reset) {
            cpu_state = " [HR]";
        } else if (cpu->halted) {
            cpu_state = " [H]";
        } else if (cpu->held_in_reset) {
            cpu_state = " [R]";
        } else {
            cpu_state = "";
        }
        monitor_printf(mon, "* CPU #%d%s: 0x%" PRIx64 " in '%s'\n",
                       cpu->cpu_index, cpu_state, (uint64_t)pc, symbol);
    }
}

static void ibex_register_types(void)
{
    monitor_register_hmp("ibex", true, &hmp_info_ibex);
}

type_init(ibex_register_types);
