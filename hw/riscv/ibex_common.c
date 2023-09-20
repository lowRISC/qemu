/*
 * QEMU RISC-V Helpers for LowRISC Ibex Demo System & OpenTitan EarlGrey
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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

static void rust_demangle_fn(const char *st_name, int st_info,
                             uint64_t st_value, uint64_t st_size);

static void ibex_mmio_map_device(SysBusDevice *dev, MemoryRegion *mr,
                                 unsigned nr, hwaddr addr)
{
    g_assert(nr < dev->num_mmio);
    g_assert(dev->mmio[nr].addr == (hwaddr)-1);
    dev->mmio[nr].addr = addr;
    memory_region_add_subregion(mr, addr, dev->mmio[nr].memory);
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
    if (!remotes) {
        remotes = &devices;
    }
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
                DeviceState **tdevices = remotes[rix];
                g_assert(tdevices);
                DeviceState *target = tdevices[dix];
                g_assert(target);
                object_property_set_link(OBJECT(dev), link->propname,
                                         OBJECT(target), &error_fatal);
                link++;
            }
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
        const IbexDevicePropDef *prop = defs[idx].prop;
        if (prop) {
            while (prop->propname) {
                switch (prop->type) {
                case IBEX_PROP_TYPE_BOOL:
                    object_property_set_bool(OBJECT(dev), prop->propname,
                                             prop->b, &error_fatal);
                    break;
                case IBEX_PROP_TYPE_INT:
                    object_property_set_int(OBJECT(dev), prop->propname,
                                            prop->i, &error_fatal);
                    break;
                case IBEX_PROP_TYPE_UINT:
                    object_property_set_int(OBJECT(dev), prop->propname,
                                            prop->u, &error_fatal);
                    break;
                case IBEX_PROP_TYPE_STR:
                    object_property_set_str(OBJECT(dev), prop->propname,
                                            prop->s, &error_fatal);
                    break;
                default:
                    g_assert_not_reached();
                    break;
                }
                prop++;
            }
        }
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

void ibex_map_devices_mask(DeviceState **devices, MemoryRegion **mrs,
                           const IbexDeviceDef *defs, unsigned count,
                           uint32_t region_mask)
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
                const MemMapEntry *memmap = def->memmap;
                unsigned mem = 0;
                while (memmap->size) {
                    unsigned region = IBEX_MEMMAP_GET_REGIDX(memmap->base);
                    if (region_mask & (1u << region)) {
                        MemoryRegion *mr = mrs[region];
                        if (mr) {
                            ibex_mmio_map_device(busdev, mr, mem,
                                                 IBEX_MEMMAP_GET_ADDRESS(
                                                     memmap->base));
                        }
                    }
                    mem++;
                    memmap++;
                }
            }
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
                assert(ngl);
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

void ibex_configure_devices(DeviceState **devices, BusState *bus,
                            const IbexDeviceDef *defs, unsigned count)
{
    ibex_link_devices(devices, defs, count);
    ibex_define_device_props(devices, defs, count);
    ibex_realize_devices(devices, bus, defs, count);
    ibex_connect_devices(devices, defs, count);
}

void ibex_unimp_configure(DeviceState *dev, const IbexDeviceDef *def,
                          DeviceState *parent)
{
    if (def->name) {
        qdev_prop_set_string(dev, "name", def->name);
    }
    g_assert(def->memmap != NULL);
    g_assert(def->memmap->size != 0);
    qdev_prop_set_uint64(dev, "size", def->memmap->size);
}

void ibex_load_kernel(AddressSpace *as)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    /* load kernel if provided */
    if (ms->kernel_filename) {
        uint64_t kernel_entry;
        if (load_elf_ram_sym(ms->kernel_filename, NULL, NULL, NULL,
                             &kernel_entry, NULL, NULL, NULL, 0, EM_RISCV, 1, 0,
                             as, true, &rust_demangle_fn) <= 0) {
            error_report("Cannot load ELF kernel %s", ms->kernel_filename);
            exit(EXIT_FAILURE);
        }

        CPUState *cpu;
        CPU_FOREACH(cpu) {
            if (!as || cpu->as == as) {
                CPURISCVState *env = &RISCV_CPU(cpu)->env;
                env->resetvec = (target_ulong)kernel_entry;
            }
        }
    }
}

uint64_t ibex_get_current_pc(void)
{
    CPUState *cs = current_cpu;

    return cs && cs->cc->get_pc ? cs->cc->get_pc(cs) : 0u;
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

    CPU_FOREACH(cpu) {
        vaddr pc;
        const char *symbol;
        if (cpu->cc->get_pc) {
            pc = cpu->cc->get_pc(cpu);
            symbol = lookup_symbol(pc);
        } else {
            pc = -1;
            symbol = "?";
        }
        monitor_printf(mon, "* CPU #%d: 0x%" PRIx64 " in '%s'\n",
                       cpu->cpu_index, (uint64_t)pc, symbol);
    }
}

static void ibex_register_types(void)
{
    monitor_register_hmp("ibex", true, &hmp_info_ibex);
}

type_init(ibex_register_types)
