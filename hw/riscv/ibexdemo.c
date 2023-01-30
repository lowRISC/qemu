/*
 * QEMU RISC-V Board Compatible with Ibex Demo System FPGA platform
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
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
 *
 * Notes: GPIO output, SIMCTRL, SPI, TIMER and UART devices are supported. PWM
 *        is only a dummy device, GPIO inputs are not supported.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "elf.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/ibexdemo/ibexdemo_gpio.h"
#include "hw/ibexdemo/ibexdemo_simctrl.h"
#include "hw/ibexdemo/ibexdemo_spi.h"
#include "hw/ibexdemo/ibexdemo_timer.h"
#include "hw/ibexdemo/ibexdemo_uart.h"
#include "hw/loader.h"
#include "hw/misc/unimp.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibexdemo.h"
#include "hw/ssi/ssi.h"
#include "sysemu/sysemu.h"

/* ------------------------------------------------------------------------ */
/* Forward Declarations */
/* ------------------------------------------------------------------------ */

static void ibexdemo_soc_gpio_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ibexdemo_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ibexdemo_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);

/* ------------------------------------------------------------------------ */
/* Constants */
/* ------------------------------------------------------------------------ */

static const MemMapEntry ibexdemo_ram = { .base = 0x00100000u,
                                          .size = 0x10000u };

static const uint32_t IBEXDEMO_BOOT[] = {
    /* Exception vectors */
    0x0840006fu, 0x0800006fu, 0x07c0006fu, 0x0780006fu, 0x0740006fu,
    0x0700006fu, 0x06c0006fu, 0x0680006fu, 0x0640006fu, 0x0600006fu,
    0x05c0006fu, 0x0580006fu, 0x0540006fu, 0x0500006fu, 0x04c0006fu,
    0x0480006fu, 0x0440006fu, 0x0400006fu, 0x03c0006fu, 0x0380006fu,
    0x0340006fu, 0x0300006fu, 0x02c0006fu, 0x0280006fu, 0x0240006fu,
    0x0200006fu, 0x01c0006fu, 0x0180006fu, 0x0140006fu, 0x0100006fu,
    0x00c0006fu, 0x0080006fu,
    /* reset vector */
    0x0040006fu,
    /* blank_loop */
    0x10500073u, /* wfi */
    0x0000bff5u, /* j blank_loop */
};

enum IbexDemoSocDevice {
    IBEXDEMO_SOC_DEV_GPIO,
    IBEXDEMO_SOC_DEV_HART,
    IBEXDEMO_SOC_DEV_PWM,
    IBEXDEMO_SOC_DEV_SIMCTRL,
    IBEXDEMO_SOC_DEV_SPI,
    IBEXDEMO_SOC_DEV_TIMER,
    IBEXDEMO_SOC_DEV_UART,
};

enum IbexDemoBoardDevice {
    /* clang-format off */
    IBEXDEMO_BOARD_DEV_SOC,
    _IBEXDEMO_BOARD_DEV_COUNT
    /* clang-format on */
};

static const IbexDeviceDef ibexdemo_soc_devices[] = {
    /* clang-format off */
    [IBEXDEMO_SOC_DEV_HART] = {
        .type = TYPE_RISCV_CPU_LOWRISC_IBEX,
        .cfg = &ibexdemo_soc_hart_configure,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_BOOL_PROP("m", true),
            IBEX_DEV_UINT_PROP("mtvec", 0x00100001u)
        ),
    },
    [IBEXDEMO_SOC_DEV_SIMCTRL] = {
        .type = TYPE_IBEXDEMO_SIMCTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x00020000u, .size = 0x0400u }
        ),
    },
    [IBEXDEMO_SOC_DEV_GPIO] = {
        .type = TYPE_IBEXDEMO_GPIO,
        .cfg = &ibexdemo_soc_gpio_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80000000u, .size = 0x1000u }
        ),
    },
    [IBEXDEMO_SOC_DEV_UART] = {
        .type = TYPE_IBEXDEMO_UART,
        .cfg = &ibexdemo_soc_uart_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80001000u, .size = 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            IBEX_GPIO_SYSBUS_IRQ(0, IBEXDEMO_SOC_DEV_HART, 16)
        ),
    },
    [IBEXDEMO_SOC_DEV_TIMER] = {
        .type = TYPE_IBEXDEMO_TIMER,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80002000u, .size = 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            IBEX_GPIO_SYSBUS_IRQ(0, IBEXDEMO_SOC_DEV_HART, IRQ_M_TIMER)
        ),
    },
    [IBEXDEMO_SOC_DEV_PWM] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ibexdemo-pwm",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80003000u, .size = 0x1000u }
        ),
    },
    [IBEXDEMO_SOC_DEV_SPI] = {
        .type = TYPE_IBEXDEMO_SPI,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80004000u, .size = 0x0400u }
        ),
    },
    /* clang-format on */
};

/* ------------------------------------------------------------------------ */
/* Type definitions */
/* ------------------------------------------------------------------------ */

struct IbexDemoSoCState {
    SysBusDevice parent_obj;

    DeviceState **devices;

    /* properties */
    uint32_t resetvec;
};

struct IbexDemoBoardState {
    DeviceState parent_obj;

    DeviceState **devices;
};

struct IbexDemoMachineState {
    MachineState parent_obj;

    char *rv_exts;
};

/* ------------------------------------------------------------------------ */
/* Device Configuration */
/* ------------------------------------------------------------------------ */

static void ibexdemo_soc_gpio_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    qdev_prop_set_uint32(dev, "in_count", IBEXDEMO_GPIO_IN_MAX);
    qdev_prop_set_uint32(dev, "out_count", IBEXDEMO_GPIO_OUT_MAX);
}

static void ibexdemo_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(parent);

    qdev_prop_set_uint64(dev, "resetvec", s->resetvec);
}

static void ibexdemo_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    qdev_prop_set_chr(dev, "chardev", serial_hd(def->instance));
}

/* ------------------------------------------------------------------------ */
/* SoC */
/* ------------------------------------------------------------------------ */

static void ibexdemo_soc_load_boot(IbexDemoSoCState *s)
{
    /* do not use rom_add_blob_fixed_as as absolute address is not yet known */
    MachineState *ms = MACHINE(qdev_get_machine());
    void *ram = memory_region_get_ram_ptr(ms->ram);
    if (!ram) {
        error_setg(&error_fatal, "no main RAM");
    }
    memcpy(ram, IBEXDEMO_BOOT, sizeof(IBEXDEMO_BOOT));
}

static void ibexdemo_soc_reset(DeviceState *dev)
{
    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(dev);

    cpu_reset(CPU(s->devices[IBEXDEMO_SOC_DEV_HART]));
}

static void ibexdemo_soc_realize(DeviceState *dev, Error **errp)
{
    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(dev);

    MachineState *ms = MACHINE(qdev_get_machine());
    MemoryRegion *sys_mem = get_system_memory();
    memory_region_add_subregion(sys_mem, ibexdemo_ram.base, ms->ram);

    ibex_link_devices(s->devices, ibexdemo_soc_devices,
                      ARRAY_SIZE(ibexdemo_soc_devices));
    ibex_define_device_props(s->devices, ibexdemo_soc_devices,
                             ARRAY_SIZE(ibexdemo_soc_devices));
    ibex_realize_system_devices(s->devices, ibexdemo_soc_devices,
                                ARRAY_SIZE(ibexdemo_soc_devices));
    ibex_connect_devices(s->devices, ibexdemo_soc_devices,
                         ARRAY_SIZE(ibexdemo_soc_devices));

    ibexdemo_soc_load_boot(s);

    /* load application if provided */
    ibex_load_kernel(NULL);
}

static void ibexdemo_soc_init(Object *obj)
{
    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(obj);

    s->devices =
        ibex_create_devices(ibexdemo_soc_devices,
                            ARRAY_SIZE(ibexdemo_soc_devices), DEVICE(s));
}

static Property ibexdemo_soc_props[] = {
    DEFINE_PROP_UINT32("resetvec", IbexDemoSoCState, resetvec, 0x00100080u),
    DEFINE_PROP_END_OF_LIST()
};

static void ibexdemo_soc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, ibexdemo_soc_props);
    dc->reset = &ibexdemo_soc_reset;
    dc->realize = &ibexdemo_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo ibexdemo_soc_type_info = {
    .name = TYPE_RISCV_IBEXDEMO_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoSoCState),
    .instance_init = &ibexdemo_soc_init,
    .class_init = &ibexdemo_soc_class_init,
};

static void ibexdemo_soc_register_types(void)
{
    type_register_static(&ibexdemo_soc_type_info);
}

type_init(ibexdemo_soc_register_types);

/* ------------------------------------------------------------------------ */
/* Board */
/* ------------------------------------------------------------------------ */

static void ibexdemo_board_realize(DeviceState *dev, Error **errp)
{
    IbexDemoBoardState *board = RISCV_IBEXDEMO_BOARD(dev);

    IbexDemoSoCState *soc =
        RISCV_IBEXDEMO_SOC(board->devices[IBEXDEMO_BOARD_DEV_SOC]);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
}

static void ibexdemo_board_instance_init(Object *obj)
{
    IbexDemoBoardState *s = RISCV_IBEXDEMO_BOARD(obj);

    s->devices = g_new0(DeviceState *, _IBEXDEMO_BOARD_DEV_COUNT);
    s->devices[IBEXDEMO_BOARD_DEV_SOC] = qdev_new(TYPE_RISCV_IBEXDEMO_SOC);

    object_property_add_child(obj, "soc",
                              OBJECT(s->devices[IBEXDEMO_BOARD_DEV_SOC]));
}

static void ibexdemo_board_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = &ibexdemo_board_realize;
}

static const TypeInfo ibexdemo_board_type_info = {
    .name = TYPE_RISCV_IBEXDEMO_BOARD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IbexDemoBoardState),
    .instance_init = &ibexdemo_board_instance_init,
    .class_init = &ibexdemo_board_class_init,
};

static void ibexdemo_board_register_types(void)
{
    type_register_static(&ibexdemo_board_type_info);
}

type_init(ibexdemo_board_register_types);

/* ------------------------------------------------------------------------ */
/* Machine */
/* ------------------------------------------------------------------------ */

static void ibexdemo_machine_init(MachineState *state)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_IBEXDEMO_BOARD);

    object_property_add_child(OBJECT(state), "board", OBJECT(dev));

    qdev_realize(dev, NULL, &error_fatal);
}

static void ibexdemo_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Board compatible with IbexDemo";
    mc->init = ibexdemo_machine_init;
    mc->max_cpus = 1u;
    mc->default_cpu_type = ibexdemo_soc_devices[IBEXDEMO_SOC_DEV_HART].type;
    mc->default_ram_id = "ibexdemo.ram";
    mc->default_ram_size = ibexdemo_ram.size;
}

static const TypeInfo ot_earlgrey_machine_type_info = {
    .name = TYPE_RISCV_IBEXDEMO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IbexDemoMachineState),
    .class_init = &ibexdemo_machine_class_init,
};

static void ot_earlgrey_machine_register_types(void)
{
    type_register_static(&ot_earlgrey_machine_type_info);
}

type_init(ot_earlgrey_machine_register_types);
