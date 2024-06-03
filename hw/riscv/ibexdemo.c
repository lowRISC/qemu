/*
 * QEMU RISC-V Board Compatible with Ibex Demo System FPGA platform
 *
 * Copyright (c) 2022-2024 Rivos, Inc.
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
 * Notes: GPIO output, SIMCTRL, SPI, TIMER, UART and ST7735 devices are
 *        supported. PWM is only a dummy device, GPIO inputs are not supported.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qlist.h"
#include "chardev/chardev-internal.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/display/st7735.h"
#include "hw/ibexdemo/ibexdemo_gpio.h"
#include "hw/ibexdemo/ibexdemo_simctrl.h"
#include "hw/ibexdemo/ibexdemo_spi.h"
#include "hw/ibexdemo/ibexdemo_timer.h"
#include "hw/ibexdemo/ibexdemo_uart.h"
#include "hw/jtag/tap_ctrl.h"
#include "hw/jtag/tap_ctrl_rbb.h"
#include "hw/misc/pulp_rv_dm.h"
#include "hw/misc/unimp.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/dm.h"
#include "hw/riscv/dtm.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibexdemo.h"
#include "hw/ssi/ssi.h"
#include "sysemu/sysemu.h"

/* ------------------------------------------------------------------------ */
/* Forward Declarations */
/* ------------------------------------------------------------------------ */

static void ibexdemo_soc_dm_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ibexdemo_soc_gpio_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ibexdemo_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ibexdemo_soc_tap_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ibexdemo_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);

/* ------------------------------------------------------------------------ */
/* Constants */
/* ------------------------------------------------------------------------ */

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
    IBEXDEMO_SOC_DEV_DM,
    IBEXDEMO_SOC_DEV_DTM,
    IBEXDEMO_SOC_DEV_GPIO,
    IBEXDEMO_SOC_DEV_HART,
    IBEXDEMO_SOC_DEV_PWM,
    IBEXDEMO_SOC_DEV_RV_DM,
    IBEXDEMO_SOC_DEV_SIM_CTRL,
    IBEXDEMO_SOC_DEV_SPI,
    IBEXDEMO_SOC_DEV_TAP_CTRL,
    IBEXDEMO_SOC_DEV_TIMER,
    IBEXDEMO_SOC_DEV_UART,
};

enum IbexDemoBoardDevice {
    IBEXDEMO_BOARD_DEV_SOC,
    IBEXDEMO_BOARD_DEV_DISPLAY,
    _IBEXDEMO_BOARD_DEV_COUNT
};

#define IBEXDEMO_SOC_DEVLINK(_pname_, _target_) \
    IBEX_DEVLINK(_pname_, IBEXDEMO_SOC_DEV_##_target_)

/*
 * Ibex Demo System RV DM
 * see https://github.com/lowRISC/part-number-registry/blob/main/jtag_partno.md
 */
#define IBEXDEMO_TAP_IDCODE IBEX_JTAG_IDCODE(256, 1, 0)

#define PULP_DM_BASE   0x00010000u
#define SRAM_MAIN_BASE 0x100000u
#define SRAM_MAIN_SIZE 0x10000u

#define IBEXDEMO_DM_CONNECTION(_dst_dev_, _num_) \
    { \
        .out = { \
            .name = PULP_RV_DM_ACK_OUT_LINES, \
            .num = (_num_), \
        }, \
        .in = { \
            .name = RISCV_DM_ACK_LINES, \
            .index = (_dst_dev_), \
            .num = (_num_), \
        } \
    }

static const IbexDeviceDef ibexdemo_soc_devices[] = {
    /* clang-format off */
    [IBEXDEMO_SOC_DEV_HART] = {
        .type = TYPE_RISCV_CPU_LOWRISC_IBEXDEMO,
        .cfg = &ibexdemo_soc_hart_configure,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("mtvec", 0x00100001u),
            IBEX_DEV_UINT_PROP("dmhaltvec", PULP_DM_BASE +
                PULP_RV_DM_ROM_BASE + PULP_RV_DM_HALT_OFFSET),
            IBEX_DEV_UINT_PROP("dmexcpvec", PULP_DM_BASE +
                PULP_RV_DM_ROM_BASE + PULP_RV_DM_EXCEPTION_OFFSET)
        ),
    },
    [IBEXDEMO_SOC_DEV_TAP_CTRL] = {
        .type = TYPE_TAP_CTRL_RBB,
        .cfg = &ibexdemo_soc_tap_ctrl_configure,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("ir_length", IBEX_TAP_IR_LENGTH),
            IBEX_DEV_UINT_PROP("idcode", IBEXDEMO_TAP_IDCODE)
        ),
    },
    [IBEXDEMO_SOC_DEV_DTM] = {
        .type = TYPE_RISCV_DTM,
        .link = IBEXDEVICELINKDEFS(
            IBEXDEMO_SOC_DEVLINK("tap_ctrl", TAP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("abits", 7u)
        ),
    },
    [IBEXDEMO_SOC_DEV_DM] = {
        .type = TYPE_RISCV_DM,
        .cfg = &ibexdemo_soc_dm_configure,
        .link = IBEXDEVICELINKDEFS(
            IBEXDEMO_SOC_DEVLINK("dtm", DTM)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("nscratch", PULP_RV_DM_NSCRATCH_COUNT),
            IBEX_DEV_UINT_PROP("progbuf_count",
                PULP_RV_DM_PROGRAM_BUFFER_COUNT),
            IBEX_DEV_UINT_PROP("data_count", PULP_RV_DM_DATA_COUNT),
            IBEX_DEV_UINT_PROP("abstractcmd_count",
                PULP_RV_DM_ABSTRACTCMD_COUNT),
            IBEX_DEV_UINT_PROP("dm_phyaddr", PULP_DM_BASE),
            IBEX_DEV_UINT_PROP("rom_phyaddr",
                PULP_DM_BASE + PULP_RV_DM_ROM_BASE),
            IBEX_DEV_UINT_PROP("whereto_phyaddr",
                PULP_DM_BASE + PULP_RV_DM_WHERETO_OFFSET),
            IBEX_DEV_UINT_PROP("data_phyaddr",
                PULP_DM_BASE + PULP_RV_DM_DATAADDR_OFFSET),
            IBEX_DEV_UINT_PROP("progbuf_phyaddr",
                PULP_DM_BASE + PULP_RV_DM_PROGRAM_BUFFER_OFFSET),
            IBEX_DEV_UINT_PROP("resume_offset", PULP_RV_DM_RESUME_OFFSET),
            IBEX_DEV_BOOL_PROP("sysbus_access", true),
            IBEX_DEV_BOOL_PROP("abstractauto", false)
        ),
    },
    [IBEXDEMO_SOC_DEV_RV_DM] = {
        .type = TYPE_PULP_RV_DM,
        .memmap = MEMMAPENTRIES(
            { .base = 0x00000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            IBEXDEMO_DM_CONNECTION(IBEXDEMO_SOC_DEV_DM, 0),
            IBEXDEMO_DM_CONNECTION(IBEXDEMO_SOC_DEV_DM, 1),
            IBEXDEMO_DM_CONNECTION(IBEXDEMO_SOC_DEV_DM, 2),
            IBEXDEMO_DM_CONNECTION(IBEXDEMO_SOC_DEV_DM, 3)
        ),
    },
    [IBEXDEMO_SOC_DEV_SIM_CTRL] = {
        .type = TYPE_IBEXDEMO_SIMCTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x00020000u }
        ),
    },
    [IBEXDEMO_SOC_DEV_GPIO] = {
        .type = TYPE_IBEXDEMO_GPIO,
        .cfg = &ibexdemo_soc_gpio_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80000000u }
        ),
    },
    [IBEXDEMO_SOC_DEV_UART] = {
        .type = TYPE_IBEXDEMO_UART,
        .cfg = &ibexdemo_soc_uart_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80001000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            IBEX_GPIO_SYSBUS_IRQ(0, IBEXDEMO_SOC_DEV_HART, 16)
        ),
    },
    [IBEXDEMO_SOC_DEV_TIMER] = {
        .type = TYPE_IBEXDEMO_TIMER,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80002000u }
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
            { .base = 0x80003000u }
        ),
    },
    [IBEXDEMO_SOC_DEV_SPI] = {
        .type = TYPE_IBEXDEMO_SPI,
        .memmap = MEMMAPENTRIES(
            { .base = 0x80004000u }
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

static void ibexdemo_soc_dm_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)def;
    (void)parent;
    QList *hart = qlist_new();
    qlist_append_int(hart, 0);
    qdev_prop_set_array(dev, "hart", hart);
}


static void ibexdemo_soc_gpio_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)def;
    (void)parent;
    qdev_prop_set_uint32(dev, "in_count", IBEXDEMO_GPIO_IN_MAX);
    qdev_prop_set_uint32(dev, "out_count", IBEXDEMO_GPIO_OUT_MAX);
}

static void ibexdemo_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)def;
    (void)parent;
    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(parent);

    qdev_prop_set_uint64(dev, "resetvec", s->resetvec);
}

static void ibexdemo_soc_tap_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)parent;
    (void)def;

    Chardev *chr;

    chr = ibex_get_chardev_by_id("taprbb");
    if (chr) {
        qdev_prop_set_chr(dev, "chardev", chr);
    }
}

static void ibexdemo_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)parent;
    qdev_prop_set_chr(dev, "chardev", serial_hd(def->instance));
}

/* ------------------------------------------------------------------------ */
/* SoC */
/* ------------------------------------------------------------------------ */

static void ibexdemo_soc_load_boot(void)
{
    /* do not use rom_add_blob_fixed_as as absolute address is not yet known */
    MachineState *ms = MACHINE(qdev_get_machine());
    void *ram = memory_region_get_ram_ptr(ms->ram);
    if (!ram) {
        error_setg(&error_fatal, "no main RAM");
        /* linter cannot detect error_fatal prevents from returning */
        abort();
    }
    memcpy(ram, IBEXDEMO_BOOT, sizeof(IBEXDEMO_BOOT));
}

static void ibexdemo_soc_reset(DeviceState *dev)
{
    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(dev);

    device_cold_reset(s->devices[IBEXDEMO_SOC_DEV_DTM]);
    device_cold_reset(s->devices[IBEXDEMO_SOC_DEV_DM]);

    cpu_reset(CPU(s->devices[IBEXDEMO_SOC_DEV_HART]));
}

static void ibexdemo_soc_realize(DeviceState *dev, Error **errp)
{
    (void)errp;

    IbexDemoSoCState *s = RISCV_IBEXDEMO_SOC(dev);

    MachineState *ms = MACHINE(qdev_get_machine());
    MemoryRegion *sys_mem = get_system_memory();
    memory_region_add_subregion(sys_mem, SRAM_MAIN_BASE, ms->ram);

    ibex_link_devices(s->devices, ibexdemo_soc_devices,
                      ARRAY_SIZE(ibexdemo_soc_devices));
    ibex_define_device_props(s->devices, ibexdemo_soc_devices,
                             ARRAY_SIZE(ibexdemo_soc_devices));
    ibex_realize_system_devices(s->devices, ibexdemo_soc_devices,
                                ARRAY_SIZE(ibexdemo_soc_devices));
    ibex_connect_devices(s->devices, ibexdemo_soc_devices,
                         ARRAY_SIZE(ibexdemo_soc_devices));

    ibexdemo_soc_load_boot();

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
    (void)data;

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
    (void)errp;

    IbexDemoSoCState *soc =
        RISCV_IBEXDEMO_SOC(board->devices[IBEXDEMO_BOARD_DEV_SOC]);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);

    BusState *spibus =
        qdev_get_child_bus(DEVICE(soc->devices[IBEXDEMO_SOC_DEV_SPI]), "spi0");
    g_assert(spibus);

    board->devices[IBEXDEMO_BOARD_DEV_DISPLAY] =
        DEVICE(ST7735(ssi_create_peripheral(SSI_BUS(spibus), TYPE_ST7735)));

    qemu_irq cs, dc, rst;

    dev = board->devices[IBEXDEMO_BOARD_DEV_DISPLAY];
    cs = qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0);
    dc = qdev_get_gpio_in_named(dev, ST7735_IO_LINES, ST7735_IO_D_C);
    rst = qdev_get_gpio_in_named(dev, ST7735_IO_LINES, ST7735_IO_RESET);

    dev = soc->devices[IBEXDEMO_SOC_DEV_GPIO];
    qdev_connect_gpio_out_named(dev, IBEXDEMO_GPIO_OUT_LINES, 0, cs);
    qdev_connect_gpio_out_named(dev, IBEXDEMO_GPIO_OUT_LINES, 1, rst);
    qdev_connect_gpio_out_named(dev, IBEXDEMO_GPIO_OUT_LINES, 2, dc);
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
    (void)data;

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
    (void)data;

    mc->desc = "RISC-V Board compatible with IbexDemo";
    mc->init = ibexdemo_machine_init;
    mc->max_cpus = 1u;
    mc->default_cpu_type = ibexdemo_soc_devices[IBEXDEMO_SOC_DEV_HART].type;
    mc->default_ram_id = "ibexdemo.ram";
    mc->default_ram_size = SRAM_MAIN_SIZE;
}

static const TypeInfo ibexdemo_machine_type_info = {
    .name = TYPE_RISCV_IBEXDEMO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IbexDemoMachineState),
    .class_init = &ibexdemo_machine_class_init,
};

static void ibexdemo_machine_register_types(void)
{
    type_register_static(&ibexdemo_machine_type_info);
}

type_init(ibexdemo_machine_register_types);
