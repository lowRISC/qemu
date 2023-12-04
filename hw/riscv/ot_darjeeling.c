/*
 * QEMU RISC-V Board Compatible with OpenTitan "integrated" Darjeeling platform
 *
 * Copyright (c) 2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * This implementation is based on:
 *  https://docs.google.com/document/d/
 *         1jGeVNqmEUEJcmOfQ0mEZ_E8pG-RYovtVMelVTQZECcA
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
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/intc/sifive_plic.h"
#include "hw/misc/unimp.h"
#include "hw/opentitan/ot_aes.h"
#include "hw/opentitan/ot_alert_darjeeling.h"
#include "hw/opentitan/ot_aon_timer.h"
#include "hw/opentitan/ot_ast_darjeeling.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_csrng.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_entropy_src.h"
#include "hw/opentitan/ot_gpio.h"
#include "hw/opentitan/ot_hmac.h"
#include "hw/opentitan/ot_ibex_wrapper_darjeeling.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/opentitan/ot_lifecycle.h"
#include "hw/opentitan/ot_otbn.h"
#include "hw/opentitan/ot_otp_darjeeling.h"
#include "hw/opentitan/ot_pinmux.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/opentitan/ot_sensor.h"
#include "hw/opentitan/ot_spi_device.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/opentitan/ot_sram_ctrl.h"
#include "hw/opentitan/ot_timer.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ot_darjeeling.h"
#include "hw/ssi/ssi.h"
#include "sysemu/blockdev.h"
#include "sysemu/sysemu.h"

/* ------------------------------------------------------------------------ */
/* Forward Declarations */
/* ------------------------------------------------------------------------ */

static void ot_darjeeling_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_darjeeling_soc_otp_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_darjeeling_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);

/* ------------------------------------------------------------------------ */
/* Constants */
/* ------------------------------------------------------------------------ */

/* Darjeeling Peripheral clock is 62.5 MHz */
#define OT_DARJEELING_PERIPHERAL_CLK_HZ 62500000u

/* Darjeeling SPI host clock is 250 MHz */
#define OT_DARJEELING__SPIHOST_CLK_HZ 250000000u

/* Darjeeling AON clock is 62.5 MHz */
#define OT_DARJEELING_AON_CLK_HZ 62500000u

/* CTN RAM (1MB) */
#define OT_DARJEELING_CTN_RAM_ADDR 0x41000000u
#define OT_DARJEELING_CTN_RAM_SIZE (1u << 20u)

enum OtDarjeelingSocDevice {
    OT_DARJEELING_SOC_DEV_AES,
    OT_DARJEELING_SOC_DEV_ALERT_HANDLER,
    OT_DARJEELING_SOC_DEV_AON_TIMER,
    OT_DARJEELING_SOC_DEV_AST,
    OT_DARJEELING_SOC_DEV_CLKMGR,
    OT_DARJEELING_SOC_DEV_CSRNG,
    OT_DARJEELING_SOC_DEV_DMA,
    OT_DARJEELING_SOC_DEV_EDN0,
    OT_DARJEELING_SOC_DEV_EDN1,
    OT_DARJEELING_SOC_DEV_GPIO,
    OT_DARJEELING_SOC_DEV_HART,
    OT_DARJEELING_SOC_DEV_HMAC,
    OT_DARJEELING_SOC_DEV_I2C0,
    OT_DARJEELING_SOC_DEV_IBEX_WRAPPER,
    OT_DARJEELING_SOC_DEV_KEYMGR_DPE,
    OT_DARJEELING_SOC_DEV_KMAC,
    OT_DARJEELING_SOC_DEV_LC_CTRL,
    OT_DARJEELING_SOC_DEV_MBX0,
    OT_DARJEELING_SOC_DEV_MBX1,
    OT_DARJEELING_SOC_DEV_MBX2,
    OT_DARJEELING_SOC_DEV_MBX3,
    OT_DARJEELING_SOC_DEV_MBX4,
    OT_DARJEELING_SOC_DEV_MBX5,
    OT_DARJEELING_SOC_DEV_MBX6,
    OT_DARJEELING_SOC_DEV_MBX_JTAG,
    OT_DARJEELING_SOC_DEV_MBX_PCIE0,
    OT_DARJEELING_SOC_DEV_MBX_PCIE1,
    OT_DARJEELING_SOC_DEV_OTBN,
    OT_DARJEELING_SOC_DEV_OTP_CTRL,
    OT_DARJEELING_SOC_DEV_PINMUX,
    OT_DARJEELING_SOC_DEV_PLIC,
    OT_DARJEELING_SOC_DEV_PWRMGR,
    OT_DARJEELING_SOC_DEV_ROM0,
    OT_DARJEELING_SOC_DEV_ROM1,
    OT_DARJEELING_SOC_DEV_RSTMGR,
    OT_DARJEELING_SOC_DEV_RV_DM,
    OT_DARJEELING_SOC_DEV_RV_DM_MEM,
    OT_DARJEELING_SOC_DEV_SENSOR_CTRL,
    OT_DARJEELING_SOC_DEV_SOC_PROXY,
    OT_DARJEELING_SOC_DEV_SPI_DEVICE,
    OT_DARJEELING_SOC_DEV_SPI_HOST0,
    OT_DARJEELING_SOC_DEV_SRAM_MAIN,
    OT_DARJEELING_SOC_DEV_SRAM_MBX,
    OT_DARJEELING_SOC_DEV_SRAM_RET,
    OT_DARJEELING_SOC_DEV_TIMER,
    OT_DARJEELING_SOC_DEV_UART0,
};

enum OtDarjeelingMemoryRegion {
    OT_DARJEELING_DEFAULT_MEMORY_REGION,
    OT_DARJEELING_XPORT_MEMORY_REGION,
};

#define OT_DARJEELING_SOC_GPIO(_irq_, _target_, _num_) \
    IBEX_GPIO(_irq_, OT_DARJEELING_SOC_DEV_##_target_, _num_)

#define OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(_irq_, _target_, _num_) \
    IBEX_GPIO_SYSBUS_IRQ(_irq_, OT_DARJEELING_SOC_DEV_##_target_, _num_)

#define OT_DARJEELING_SOC_DEVLINK(_pname_, _target_) \
    IBEX_DEVLINK(_pname_, OT_DARJEELING_SOC_DEV_##_target_)

#define OT_DARJEELING_SOC_SIGNAL(_sname_, _snum_, _tgt_, _tname_, _tnum_) \
    { \
        .out = { \
            .name = (_sname_), \
            .num = (_snum_), \
        }, \
        .in = { \
            .name = (_tname_), \
            .index = (OT_DARJEELING_SOC_DEV_ ## _tgt_), \
            .num = (_tnum_), \
        } \
    }

#define OT_DARJEELING_SOC_CLKMGR_HINT(_num_) \
    OT_DARJEELING_SOC_SIGNAL(OPENTITAN_CLOCK_ACTIVE, 0, CLKMGR, \
                             OPENTITAN_CLKMGR_HINT, _num_)

#define OT_DARJEELING_XPORT_MEMORY(_addr_) \
    IBEX_MEMMAP_MAKE_REG((_addr_), OT_DARJEELING_XPORT_MEMORY_REGION)

/*
 * MMIO/interrupt mapping as per:
 * lowRISC/opentitan: hw/top_darjeeling/sw/autogen/top_darjeeling_memory.h
 * and
 * lowRISC/opentitan: hw/top_darjeeling/sw/autogen/top_darjeeling.h
 */
static const IbexDeviceDef ot_darjeeling_soc_devices[] = {
    /* clang-format off */
    [OT_DARJEELING_SOC_DEV_HART] = {
        .type = TYPE_RISCV_CPU_LOWRISC_IBEX,
        .cfg = &ot_darjeeling_soc_hart_configure,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_BOOL_PROP("m", true),
            IBEX_DEV_BOOL_PROP("pmp", true),
            IBEX_DEV_BOOL_PROP("zba", true),
            IBEX_DEV_BOOL_PROP("zbb", true),
            IBEX_DEV_BOOL_PROP("zbc", true),
            IBEX_DEV_BOOL_PROP("zbs", true),
            IBEX_DEV_BOOL_PROP("x-epmp", true),
            IBEX_DEV_BOOL_PROP("x-zbr", true),
            IBEX_DEV_UINT_PROP("resetvec", 0x8080u),
            IBEX_DEV_UINT_PROP("mtvec", 0x8001u),
            IBEX_DEV_BOOL_PROP("start-powered-off", true)
        ),
    },
    [OT_DARJEELING_SOC_DEV_RV_DM_MEM] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-rv_dm_mem",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x00040000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_AES] = {
        .type = TYPE_OT_AES,
        .memmap = MEMMAPENTRIES(
            { 0x21100000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_CLKMGR_HINT(OT_CLKMGR_HINT_AES)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 5u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_HMAC] = {
        .type = TYPE_OT_HMAC,
        .memmap = MEMMAPENTRIES(
            { 0x21110000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 115),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 116),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 117),
            OT_DARJEELING_SOC_CLKMGR_HINT(OT_CLKMGR_HINT_HMAC)
        ),
    },
    [OT_DARJEELING_SOC_DEV_KMAC] = {
        .type = TYPE_OT_KMAC,
        .memmap = MEMMAPENTRIES(
            { 0x21120000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 118),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 119),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 120)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 3u),
            IBEX_DEV_INT_PROP("num-app", 4u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_OTBN] = {
        .type = TYPE_OT_OTBN,
        .memmap = MEMMAPENTRIES(
            { 0x21130000u, 0x10000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 121),
            OT_DARJEELING_SOC_CLKMGR_HINT(OT_CLKMGR_HINT_OTBN)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("edn-u", EDN0),
            OT_DARJEELING_SOC_DEVLINK("edn-r", EDN1)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-u-ep", 6u),
            IBEX_DEV_INT_PROP("edn-r-ep", 0u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_KEYMGR_DPE] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-keymgr_dpe",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x21140000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_CSRNG] = {
        .type = TYPE_OT_CSRNG,
        .memmap = MEMMAPENTRIES(
            { 0x21150000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 123),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 124),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 125),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 126)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("random_src", AST),
            OT_DARJEELING_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
    },
    [OT_DARJEELING_SOC_DEV_EDN0] = {
        .type = TYPE_OT_EDN,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x21170000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 127),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 128)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("csrng", CSRNG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("csrng-app", 0u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_EDN1] = {
        .type = TYPE_OT_EDN,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x21180000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 129),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 130)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("csrng", CSRNG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("csrng-app", 1u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_SRAM_MAIN] = {
        .type = TYPE_OT_SRAM_CTRL,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x211c0000u, 01000u },
            { 0x10000000u, 0x40000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("size", 0x40000u),
            IBEX_DEV_STRING_PROP("id", "ram")
        ),
    },
    [OT_DARJEELING_SOC_DEV_SRAM_MBX] = {
        .type = TYPE_OT_SRAM_CTRL,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x211d0000u, 01000u },
            { 0x11000000u, 0x1000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("size", 0x1000u),
            IBEX_DEV_STRING_PROP("id", "mbx")
        ),
    },
    [OT_DARJEELING_SOC_DEV_ROM0] = {
        .type = TYPE_OT_ROM_CTRL,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x211e0000u, 0x1000u },
            { 0x00008000u, 0x8000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_ROM_CTRL_GOOD, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_ROM_GOOD, 0),
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_ROM_CTRL_DONE, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_ROM_DONE, 0)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("kmac", KMAC)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("rom_id", "rom0"),
            IBEX_DEV_INT_PROP("size", 0x8000u),
            IBEX_DEV_INT_PROP("kmac-app", 2u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_ROM1] = {
        .type = TYPE_OT_ROM_CTRL,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x211e1000u, 0x1000u },
            { 0x00020000u, 0x10000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_ROM_CTRL_GOOD, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_ROM_GOOD, 1),
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_ROM_CTRL_DONE, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_ROM_DONE, 1)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("kmac", KMAC)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("rom_id", "rom1"),
            IBEX_DEV_INT_PROP("size", 0x10000u),
            IBEX_DEV_INT_PROP("kmac-app", 3u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_IBEX_WRAPPER] = {
        .type = TYPE_OT_IBEX_WRAPPER_DARJEELING,
        .memmap = MEMMAPENTRIES(
            { 0x211f0000u, 0x1000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 7u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_RV_DM] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-rv_dm",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x21200000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX0] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 0,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000000u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX1] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 1,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000100u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX2] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 2,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000200u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX3] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 3,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000300u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX4] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 4,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000400u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX5] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 5,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000500u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX6] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 6,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000600u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX_JTAG] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 7,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22000800u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_DMA] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-dma",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22010000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_SOC_PROXY] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-soc_proxy",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22030000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX_PCIE0] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 8,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22040000u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_MBX_PCIE1] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-mbx",
        .instance = 9,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x22040100u, 0x40u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_PLIC] = {
        .type = TYPE_SIFIVE_PLIC,
        .memmap = MEMMAPENTRIES(
            { 0x28000000u, 0x8000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO(1, HART, IRQ_M_EXT)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("hart-config", "M"),
            IBEX_DEV_UINT_PROP("hartid-base", 0u),
            /* note: should always be max_irq + 1 */
            IBEX_DEV_UINT_PROP("num-sources", 164u),
            IBEX_DEV_UINT_PROP("num-priorities", 3u),
            IBEX_DEV_UINT_PROP("priority-base", 0x0u),
            IBEX_DEV_UINT_PROP("pending-base", 0x1000u),
            IBEX_DEV_UINT_PROP("enable-base", 0x2000u),
            IBEX_DEV_UINT_PROP("enable-stride", 32u),
            IBEX_DEV_UINT_PROP("context-base", 0x200000u),
            IBEX_DEV_UINT_PROP("context-stride", 8u),
            IBEX_DEV_UINT_PROP("aperture-size", 0x8000000u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_GPIO] = {
        .type = TYPE_OT_GPIO,
        .name = "ot-gpio",
        .memmap = MEMMAPENTRIES(
            { 0x30000000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 9),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 10),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 11),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 12),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 13),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 14),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 25),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 16),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(8, PLIC, 17),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(9, PLIC, 18),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(10, PLIC, 19),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(11, PLIC, 20),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(12, PLIC, 21),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(13, PLIC, 22),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(14, PLIC, 23),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(15, PLIC, 24),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(16, PLIC, 35),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(17, PLIC, 26),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(18, PLIC, 27),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(19, PLIC, 28),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(20, PLIC, 29),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(21, PLIC, 30),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(22, PLIC, 31),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(23, PLIC, 32),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(24, PLIC, 33),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(25, PLIC, 34),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(26, PLIC, 45),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(27, PLIC, 36),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(28, PLIC, 37),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(29, PLIC, 38),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(30, PLIC, 39),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(31, PLIC, 40)
        )
    },
    [OT_DARJEELING_SOC_DEV_UART0] = {
        .type = TYPE_OT_UART,
        .cfg = &ot_darjeeling_soc_uart_configure,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x30010000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 1),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 2),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 3),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 4),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 5),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 6),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 7),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 8)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_DARJEELING_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_DARJEELING_SOC_DEV_SENSOR_CTRL] = {
        .type = TYPE_OT_SENSOR,
        .memmap = MEMMAPENTRIES(
            { 0x30020000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 80),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 81)
        )
    },
    [OT_DARJEELING_SOC_DEV_I2C0] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-i2c",
        .cfg = &ibex_unimp_configure,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x30080000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_TIMER] = {
        .type = TYPE_OT_TIMER,
        .memmap = MEMMAPENTRIES(
            { 0x30100000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO(0, HART, IRQ_M_TIMER),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 68)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_DARJEELING_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_DARJEELING_SOC_DEV_OTP_CTRL] = {
        .type = TYPE_OT_OTP_DARJEELING,
        .cfg = &ot_darjeeling_soc_otp_ctrl_configure,
        .memmap = MEMMAPENTRIES(
            { 0x30130000u, 0x8000u },
            { 0x30138000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 69),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 70)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 1u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_LC_CTRL] = {
        .type = TYPE_OT_LIFECYCLE,
        .memmap = MEMMAPENTRIES(
            { 0x30140000u, 0x1000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        )
    },
    [OT_DARJEELING_SOC_DEV_ALERT_HANDLER] = {
        .type = TYPE_OT_ALERT_DARJEELING,
        .memmap = MEMMAPENTRIES(
            { 0x30150000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 71),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 72),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 73),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 74)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 4u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_SPI_HOST0] = {
        .type = TYPE_OT_SPI_HOST,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x30300000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 76),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 77)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("bus-num", 0)
        ),
    },
    [OT_DARJEELING_SOC_DEV_SPI_DEVICE] = {
        .type = TYPE_OT_SPI_DEVICE,
        .memmap = MEMMAPENTRIES(
            { 0x30310000u, 0x2000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 41),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 42),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 43),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 44),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 45),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 46),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 47),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 48),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(8, PLIC, 49),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(9, PLIC, 50),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(10, PLIC, 51),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(11, PLIC, 52)
        ),
    },
    [OT_DARJEELING_SOC_DEV_PWRMGR] = {
        .type = TYPE_OT_PWRMGR,
        .memmap = MEMMAPENTRIES(
            { 0x30400000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 78)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("rstmgr", RSTMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("num-rom", 2u)
        ),
    },
    [OT_DARJEELING_SOC_DEV_RSTMGR] = {
        .type = TYPE_OT_RSTMGR,
        .memmap = MEMMAPENTRIES(
            { 0x30410000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_RSTMGR_SW_RST, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_SW_RST_REQ, 0)
        ),
    },
    [OT_DARJEELING_SOC_DEV_CLKMGR] = {
        .type = TYPE_OT_CLKMGR,
        .memmap = MEMMAPENTRIES(
            { 0x30420000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_PINMUX] = {
        .type = TYPE_OT_PINMUX,
        .memmap = MEMMAPENTRIES(
            { 0x30460000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_AON_TIMER] = {
        .type = TYPE_OT_AON_TIMER,
        .memmap = MEMMAPENTRIES(
            { 0x30470000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 79),
            OT_DARJEELING_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 80),
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_AON_TIMER_WKUP, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_WKUP_REQ, \
                                     OT_PWRMGR_WAKEUP_AON_TIMER),
            OT_DARJEELING_SOC_SIGNAL(OPENTITAN_AON_TIMER_BITE, 0, PWRMGR, \
                                     OPENTITAN_PWRMGR_RST_REQ,
                                     OT_PWRMGR_RST_REQ_AON_TIMER)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_DARJEELING_AON_CLK_HZ)
        ),
    },
    [OT_DARJEELING_SOC_DEV_AST] = {
        .type = TYPE_OT_AST_DARJEELING,
        .memmap = MEMMAPENTRIES(
            { 0x30480000u, 0x1000u }
        ),
    },
    [OT_DARJEELING_SOC_DEV_SRAM_RET] = {
        .type = TYPE_OT_SRAM_CTRL,
        .instance = 2,
        .memmap = MEMMAPENTRIES(
            { 0x30500000u, 0x1000u },
            { 0x30600000u, 0x1000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DARJEELING_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("size", 0x1000u),
            IBEX_DEV_STRING_PROP("id", "ret")
        ),
    },
    /* clang-format on */
};

static const uint8_t ot_darjeeling_pmp_cfgs[] = {
    /* clang-format off */
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(1, IBEX_PMP_MODE_NAPOT, 1, 0, 1), /* rgn 2  [ROM: LRX]      */
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(1, IBEX_PMP_MODE_TOR, 0, 1, 1), /* rgn 11 [MMIO: LRW] */
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(1, IBEX_PMP_MODE_NAPOT, 1, 1, 1), /* rgn 13 [DV_ROM: LRWX]  */
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0)
    /* clang-format on */
};

static const uint32_t ot_darjeeling_pmp_addrs[] = {
    /* clang-format off */
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x000083fc), /* rgn 2 [ROM: base=0x0000_8000 sz (2KiB)] */
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x21100000), /* rgn 10 [MMIO: lo=0x2110_0000] */
    IBEX_PMP_ADDR(0x30601000), /* rgn 11 [MMIO: hi=0x3060_1000] */
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x000407fc), /* rgn 13 [DV_ROM: base=0x0004_0000 sz (4KiB)] */
    IBEX_PMP_ADDR(0x00000000),
    IBEX_PMP_ADDR(0x00000000)
    /* clang-format on */
};

#define OT_DARJEELING_MSECCFG IBEX_MSECCFG(1, 1, 0)

enum OtDarjeelingBoardDevice {
    OT_DARJEELING_BOARD_DEV_SOC,
    OT_DARJEELING_BOARD_DEV_FLASH,
    _OT_DARJEELING_BOARD_DEV_COUNT,
};

/* ------------------------------------------------------------------------ */
/* Type definitions */
/* ------------------------------------------------------------------------ */

struct OtDarjeelingSoCClass {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

struct OtDarjeelingSoCState {
    SysBusDevice parent_obj;

    DeviceState **devices;
    MemoryRegion *sys; /* local memory region */
    MemoryRegion *xport; /* external port */
};

struct OtDarjeelingBoardState {
    DeviceState parent_obj;

    DeviceState **devices;
};

struct OtDarjeelingMachineState {
    MachineState parent_obj;

    bool no_epmp_cfg;
};

/* ------------------------------------------------------------------------ */
/* Device Configuration */
/* ------------------------------------------------------------------------ */

static void ot_darjeeling_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    OtDarjeelingMachineState *ms =
        RISCV_OT_DARJEELING_MACHINE(qdev_get_machine());
    (void)def;
    (void)parent;

    if (ms->no_epmp_cfg) {
        /* skip default PMP config */
        return;
    }

    qdev_prop_set_uint32(dev, PROP_ARRAY_LEN_PREFIX "pmp_cfg",
                         ARRAY_SIZE(ot_darjeeling_pmp_cfgs));
    for (unsigned ix = 0; ix < ARRAY_SIZE(ot_darjeeling_pmp_cfgs); ix++) {
        char *propname = g_strdup_printf("pmp_cfg[%u]", ix);
        qdev_prop_set_uint8(dev, propname, ot_darjeeling_pmp_cfgs[ix]);
        g_free(propname);
    }

    qdev_prop_set_uint32(dev, PROP_ARRAY_LEN_PREFIX "pmp_addr",
                         ARRAY_SIZE(ot_darjeeling_pmp_addrs));
    for (unsigned ix = 0; ix < ARRAY_SIZE(ot_darjeeling_pmp_addrs); ix++) {
        char *propname = g_strdup_printf("pmp_addr[%u]", ix);
        qdev_prop_set_uint64(dev, propname,
                             (uint64_t)ot_darjeeling_pmp_addrs[ix]);
        g_free(propname);
    }
    qdev_prop_set_uint64(dev, "mseccfg", (uint64_t)OT_DARJEELING_MSECCFG);
}

static void ot_darjeeling_soc_otp_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    (void)def;
    (void)parent;
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
}

static void ot_darjeeling_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)def;
    (void)parent;
    qdev_prop_set_chr(dev, "chardev", serial_hd(def->instance));
}

/* ------------------------------------------------------------------------ */
/* SoC */
/* ------------------------------------------------------------------------ */

static void ot_darjeeling_soc_reset_hold(Object *obj)
{
    OtDarjeelingSoCClass *c = RISCV_OT_DARJEELING_SOC_GET_CLASS(obj);
    OtDarjeelingSoCState *s = RISCV_OT_DARJEELING_SOC(obj);

    if (c->parent_phases.hold) {
        c->parent_phases.hold(obj);
    }

    /* keep ROM_CTRLs in reset, we'll release them last */
    resettable_assert_reset(OBJECT(s->devices[OT_DARJEELING_SOC_DEV_ROM0]),
                            RESET_TYPE_COLD);
    resettable_assert_reset(OBJECT(s->devices[OT_DARJEELING_SOC_DEV_ROM1]),
                            RESET_TYPE_COLD);

    cpu_reset(CPU(s->devices[OT_DARJEELING_SOC_DEV_HART]));
}

static void ot_darjeeling_soc_reset_exit(Object *obj)
{
    OtDarjeelingSoCClass *c = RISCV_OT_DARJEELING_SOC_GET_CLASS(obj);
    OtDarjeelingSoCState *s = RISCV_OT_DARJEELING_SOC(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj);
    }

    /* let ROM_CTRLs get out of reset now */
    resettable_release_reset(OBJECT(s->devices[OT_DARJEELING_SOC_DEV_ROM0]),
                             RESET_TYPE_COLD);
    resettable_release_reset(OBJECT(s->devices[OT_DARJEELING_SOC_DEV_ROM1]),
                             RESET_TYPE_COLD);
}

static void ot_darjeeling_soc_realize(DeviceState *dev, Error **errp)
{
    OtDarjeelingSoCState *s = RISCV_OT_DARJEELING_SOC(dev);
    (void)errp;

    CPUState *cpu = CPU(s->devices[OT_DARJEELING_SOC_DEV_HART]);
    cpu->memory = s->sys;
    cpu->cpu_index = 0;

    /* Link, define properties and realize devices, then connect GPIOs */
    ibex_configure_devices(s->devices, dev->parent_bus,
                           ot_darjeeling_soc_devices,
                           ARRAY_SIZE(ot_darjeeling_soc_devices));

    MemoryRegion *mrs[IBEX_MEMMAP_REGIDX_COUNT] = {
        [OT_DARJEELING_DEFAULT_MEMORY_REGION] = s->sys,
        [OT_DARJEELING_XPORT_MEMORY_REGION] = s->xport,
    };
    ibex_map_devices(s->devices, mrs, ot_darjeeling_soc_devices,
                     ARRAY_SIZE(ot_darjeeling_soc_devices));

    /* load kernel if provided */
    ibex_load_kernel(cpu->as);
}

static void ot_darjeeling_soc_init(Object *obj)
{
    OtDarjeelingSoCState *s = RISCV_OT_DARJEELING_SOC(obj);

    s->devices =
        ibex_create_devices(ot_darjeeling_soc_devices,
                            ARRAY_SIZE(ot_darjeeling_soc_devices), DEVICE(s));
}

static void ot_darjeeling_soc_class_init(ObjectClass *oc, void *data)
{
    OtDarjeelingSoCClass *sc = RISCV_OT_DARJEELING_SOC_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(dc);
    (void)data;

    resettable_class_set_parent_phases(rc, NULL, &ot_darjeeling_soc_reset_hold,
                                       &ot_darjeeling_soc_reset_exit,
                                       &sc->parent_phases);
    dc->realize = &ot_darjeeling_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo ot_darjeeling_soc_type_info = {
    .name = TYPE_RISCV_OT_DARJEELING_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtDarjeelingSoCState),
    .instance_init = &ot_darjeeling_soc_init,
    .class_init = &ot_darjeeling_soc_class_init,
    .class_size = sizeof(OtDarjeelingSoCClass),
};

static void ot_darjeeling_soc_register_types(void)
{
    type_register_static(&ot_darjeeling_soc_type_info);
}

type_init(ot_darjeeling_soc_register_types);

/* ------------------------------------------------------------------------ */
/* Board */
/* ------------------------------------------------------------------------ */

static void ot_darjeeling_board_realize(DeviceState *dev, Error **errp)
{
    OtDarjeelingBoardState *board = RISCV_OT_DARJEELING_BOARD(dev);

    DeviceState *soc = board->devices[OT_DARJEELING_BOARD_DEV_SOC];
    object_property_add_child(OBJECT(board), "soc", OBJECT(soc));

    OtDarjeelingSoCState *s = RISCV_OT_DARJEELING_SOC(soc);

    s->sys = get_system_memory();
    s->xport = NULL; /* to be filled */
    BusState *bus = sysbus_get_default();
    qdev_realize_and_unref(DEVICE(soc), bus, &error_fatal);

    /* CTN RAM */
    MachineState *ms = MACHINE(qdev_get_machine());
    memory_region_add_subregion(s->sys, OT_DARJEELING_CTN_RAM_ADDR, ms->ram);

    DeviceState *spihost = s->devices[OT_DARJEELING_SOC_DEV_SPI_HOST0];
    DeviceState *flash = board->devices[OT_DARJEELING_BOARD_DEV_FLASH];
    BusState *spibus = qdev_get_child_bus(spihost, "spi0");
    g_assert(spibus);

    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(DEVICE(flash), "drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    object_property_add_child(OBJECT(board), "dataflash", OBJECT(flash));
    ssi_realize_and_unref(flash, SSI_BUS(spibus), errp);

    qemu_irq cs = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
    qdev_connect_gpio_out_named(spihost, SSI_GPIO_CS, 0, cs);
}

static void ot_darjeeling_board_init(Object *obj)
{
    OtDarjeelingBoardState *s = RISCV_OT_DARJEELING_BOARD(obj);

    s->devices = g_new0(DeviceState *, _OT_DARJEELING_BOARD_DEV_COUNT);
    s->devices[OT_DARJEELING_BOARD_DEV_SOC] =
        qdev_new(TYPE_RISCV_OT_DARJEELING_SOC);
    s->devices[OT_DARJEELING_BOARD_DEV_FLASH] = qdev_new("is25wp128");
}

static void ot_darjeeling_board_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    (void)data;

    dc->realize = &ot_darjeeling_board_realize;
}

static const TypeInfo ot_darjeeling_board_type_info = {
    .name = TYPE_RISCV_OT_DARJEELING_BOARD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtDarjeelingBoardState),
    .instance_init = &ot_darjeeling_board_init,
    .class_init = &ot_darjeeling_board_class_init,
};

static void ot_darjeeling_board_register_types(void)
{
    type_register_static(&ot_darjeeling_board_type_info);
}

type_init(ot_darjeeling_board_register_types);

/* ------------------------------------------------------------------------ */
/* Machine */
/* ------------------------------------------------------------------------ */

static bool ot_darjeeling_machine_get_no_epmp_cfg(Object *obj, Error **errp)
{
    OtDarjeelingMachineState *s = RISCV_OT_DARJEELING_MACHINE(obj);
    (void)errp;

    return s->no_epmp_cfg;
}

static void
ot_darjeeling_machine_set_no_epmp_cfg(Object *obj, bool value, Error **errp)
{
    OtDarjeelingMachineState *s = RISCV_OT_DARJEELING_MACHINE(obj);
    (void)errp;

    s->no_epmp_cfg = value;
}

static void ot_darjeeling_machine_instance_init(Object *obj)
{
    OtDarjeelingMachineState *s = RISCV_OT_DARJEELING_MACHINE(obj);

    s->no_epmp_cfg = false;
    object_property_add_bool(obj, "no-epmp-cfg",
                             &ot_darjeeling_machine_get_no_epmp_cfg,
                             &ot_darjeeling_machine_set_no_epmp_cfg);
    object_property_set_description(obj, "no-epmp-cfg",
                                    "Skip default ePMP configuration");
}

static void ot_darjeeling_machine_init(MachineState *state)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_OT_DARJEELING_BOARD);

    object_property_add_child(OBJECT(state), "board", OBJECT(dev));
    qdev_realize(dev, NULL, &error_fatal);
}

static void ot_darjeeling_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    (void)data;

    mc->desc = "RISC-V Board compatible with OpenTitan Darjeeling platform";
    mc->init = ot_darjeeling_machine_init;
    mc->max_cpus = 1u;
    mc->default_cpus = 1u;
    mc->default_ram_id = "ctn-ram";
    mc->default_ram_size = OT_DARJEELING_CTN_RAM_SIZE;
}

static const TypeInfo ot_darjeeling_machine_type_info = {
    .name = TYPE_RISCV_OT_DARJEELING_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OtDarjeelingMachineState),
    .instance_init = &ot_darjeeling_machine_instance_init,
    .class_init = &ot_darjeeling_machine_class_init,
};

static void ot_darjeeling_machine_register_types(void)
{
    type_register_static(&ot_darjeeling_machine_type_info);
}

type_init(ot_darjeeling_machine_register_types);
