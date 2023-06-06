/*
 * QEMU RISC-V Board Compatible with OpenTitan EarlGrey FPGA platform
 *
 * Copyright (c) 2022-2023 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * This implementation is based on OpenTitan RTL version:
 *  <lowRISC/opentitan@caa3bd0a14ddebbf60760490f7c917901482c8fd>
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
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_aon_timer.h"
#include "hw/opentitan/ot_ast.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_csrng.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_entropy_src.h"
#include "hw/opentitan/ot_flash.h"
#include "hw/opentitan/ot_hmac.h"
#include "hw/opentitan/ot_ibex_wrapper.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/opentitan/ot_lifecycle.h"
#include "hw/opentitan/ot_otbn.h"
#include "hw/opentitan/ot_otp_earlgrey.h"
#include "hw/opentitan/ot_pinmux.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/opentitan/ot_sensor.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/opentitan/ot_sram_ctrl.h"
#include "hw/opentitan/ot_timer.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ot_earlgrey.h"
#include "hw/ssi/ssi.h"
#include "sysemu/blockdev.h"
#include "sysemu/sysemu.h"

/* ------------------------------------------------------------------------ */
/* Forward Declarations */
/* ------------------------------------------------------------------------ */

static void ot_earlgrey_soc_flash_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_earlgrey_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_earlgrey_soc_otp_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_earlgrey_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);

/* ------------------------------------------------------------------------ */
/* Constants */
/* ------------------------------------------------------------------------ */

/* EarlGrey/CW310 Peripheral clock is 2.5 MHz */
#define OT_EARLGREY_PERIPHERAL_CLK_HZ 2500000u

/* EarlGrey/CW310 AON clock is 250 kHz */
#define OT_EARLGREY_AON_CLK_HZ 250000u

enum OtEarlgreySocDevice {
    OT_EARLGREY_SOC_DEV_ADC_CTRL,
    OT_EARLGREY_SOC_DEV_AES,
    OT_EARLGREY_SOC_DEV_ALERT_HANDLER,
    OT_EARLGREY_SOC_DEV_AON_TIMER,
    OT_EARLGREY_SOC_DEV_AST,
    OT_EARLGREY_SOC_DEV_CLKMGR,
    OT_EARLGREY_SOC_DEV_CSRNG,
    OT_EARLGREY_SOC_DEV_EDN0,
    OT_EARLGREY_SOC_DEV_EDN1,
    OT_EARLGREY_SOC_DEV_ENTROPY_SRC,
    OT_EARLGREY_SOC_DEV_FLASH_CTRL,
    OT_EARLGREY_SOC_DEV_GPIO,
    OT_EARLGREY_SOC_DEV_HART,
    OT_EARLGREY_SOC_DEV_HMAC,
    OT_EARLGREY_SOC_DEV_I2C0,
    OT_EARLGREY_SOC_DEV_I2C1,
    OT_EARLGREY_SOC_DEV_I2C2,
    OT_EARLGREY_SOC_DEV_IBEX_WRAPPER,
    OT_EARLGREY_SOC_DEV_KEYMGR,
    OT_EARLGREY_SOC_DEV_KMAC,
    OT_EARLGREY_SOC_DEV_LC_CTRL,
    OT_EARLGREY_SOC_DEV_OTBN,
    OT_EARLGREY_SOC_DEV_OTP_CTRL,
    OT_EARLGREY_SOC_DEV_PATTGEN,
    OT_EARLGREY_SOC_DEV_PINMUX,
    OT_EARLGREY_SOC_DEV_PLIC,
    OT_EARLGREY_SOC_DEV_PWM,
    OT_EARLGREY_SOC_DEV_PWRMGR,
    OT_EARLGREY_SOC_DEV_SRAM_RET_CTRL,
    OT_EARLGREY_SOC_DEV_ROM_CTRL,
    OT_EARLGREY_SOC_DEV_RSTMGR,
    OT_EARLGREY_SOC_DEV_RV_DM,
    OT_EARLGREY_SOC_DEV_RV_DM_MEM,
    OT_EARLGREY_SOC_DEV_SENSOR_CTRL,
    OT_EARLGREY_SOC_DEV_SPI_DEVICE,
    OT_EARLGREY_SOC_DEV_SPI_HOST0,
    OT_EARLGREY_SOC_DEV_SPI_HOST1,
    OT_EARLGREY_SOC_DEV_SRAM_MAIN_CTRL,
    OT_EARLGREY_SOC_DEV_SYSRST_CTRL,
    OT_EARLGREY_SOC_DEV_TIMER,
    OT_EARLGREY_SOC_DEV_UART0,
    OT_EARLGREY_SOC_DEV_UART1,
    OT_EARLGREY_SOC_DEV_UART2,
    OT_EARLGREY_SOC_DEV_UART3,
    OT_EARLGREY_SOC_DEV_USBDEV,
};

#define OT_EARLGREY_SOC_GPIO(_irq_, _target_, _num_) \
    IBEX_GPIO(_irq_, OT_EARLGREY_SOC_DEV_##_target_, _num_)

#define OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(_irq_, _target_, _num_) \
    IBEX_GPIO_SYSBUS_IRQ(_irq_, OT_EARLGREY_SOC_DEV_##_target_, _num_)

#define OT_EARLGREY_SOC_DEVLINK(_pname_, _target_) \
    IBEX_DEVLINK(_pname_, OT_EARLGREY_SOC_DEV_##_target_)

#define OT_EARLGREY_SOC_SIGNAL(_sname_, _snum_, _tgt_, _tname_, _tnum_) \
    { \
        .out = { \
            .name = (_sname_), \
            .num = (_snum_), \
        }, \
        .in = { \
            .name = (_tname_), \
            .index = (OT_EARLGREY_SOC_DEV_ ## _tgt_), \
            .num = (_tnum_), \
        } \
    }

#define OT_EARLGREY_SOC_CLKMGR_HINT(_num_) \
    OT_EARLGREY_SOC_SIGNAL(OPENTITAN_CLOCK_ACTIVE, 0, CLKMGR, \
                           OPENTITAN_CLKMGR_HINT, _num_)

/*
 * MMIO/interrupt mapping as per:
 * lowRISC/opentitan: hw/top_earlgrey/sw/autogen/top_earlgrey_memory.h
 * and
 * lowRISC/opentitan: hw/top_earlgrey/sw/autogen/top_earlgrey.h
 */
static const IbexDeviceDef ot_earlgrey_soc_devices[] = {
    /* clang-format off */
    [OT_EARLGREY_SOC_DEV_HART] = {
        .type = TYPE_RISCV_CPU_LOWRISC_IBEX,
        .cfg = &ot_earlgrey_soc_hart_configure,
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
            IBEX_DEV_BOOL_PROP("start-powered-off", true)
        ),
    },
    [OT_EARLGREY_SOC_DEV_RV_DM_MEM] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-rv_dm_mem",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x00010000u, 0x1000u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_UART0] = {
        .type = TYPE_OT_UART,
        .cfg = &ot_earlgrey_soc_uart_configure,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x40000000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 1),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 2),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 3),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 4),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 5),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 6),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 7),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 8)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_EARLGREY_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_EARLGREY_SOC_DEV_UART1] = {
        .type = TYPE_OT_UART,
        .cfg = &ot_earlgrey_soc_uart_configure,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x40010000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 9),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 10),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 11),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 12),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 13),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 14),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 15),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 16)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_EARLGREY_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_EARLGREY_SOC_DEV_UART2] = {
        .type = TYPE_OT_UART,
        .cfg = &ot_earlgrey_soc_uart_configure,
        .instance = 2,
        .memmap = MEMMAPENTRIES(
            { 0x40020000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 17),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 18),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 19),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 20),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 21),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 22),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 23),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 24)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_EARLGREY_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_EARLGREY_SOC_DEV_UART3] = {
        .type = TYPE_OT_UART,
        .cfg = &ot_earlgrey_soc_uart_configure,
        .instance = 3,
        .memmap = MEMMAPENTRIES(
            { 0x40030000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 25),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 26),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 27),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 28),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 29),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 30),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 31),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 32)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_EARLGREY_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_EARLGREY_SOC_DEV_GPIO] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-gpio",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40040000u, 0x40u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_SPI_DEVICE] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-spi_device",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40050000u, 0x2000u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_I2C0] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-i2c",
        .cfg = &ibex_unimp_configure,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x40080000u, 0x80u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_I2C1] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-i2c",
        .cfg = &ibex_unimp_configure,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x40090000u, 0x80u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_I2C2] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-i2c",
        .cfg = &ibex_unimp_configure,
        .instance = 2,
        .memmap = MEMMAPENTRIES(
            { 0x400a0000u, 0x80u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_PATTGEN] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-pattgen",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x400e0000u, 0x40u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_TIMER] = {
        .type = TYPE_OT_TIMER,
        .memmap = MEMMAPENTRIES(
            { 0x40100000u, 0x200u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO(0, HART, IRQ_M_TIMER),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 124)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_EARLGREY_PERIPHERAL_CLK_HZ)
        ),
    },
    [OT_EARLGREY_SOC_DEV_OTP_CTRL] = {
        .type = TYPE_OT_OTP_EARLGREY,
        .cfg = &ot_earlgrey_soc_otp_ctrl_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40130000u, 0x2000u },
            { 0x40132000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 125),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 126)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 1u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_LC_CTRL] = {
        .type = TYPE_OT_LIFECYCLE,
        .memmap = MEMMAPENTRIES(
            { 0x40140000u, 0x100u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        )
    },
    [OT_EARLGREY_SOC_DEV_ALERT_HANDLER] = {
        .type = TYPE_OT_ALERT,
        .memmap = MEMMAPENTRIES(
            { 0x40150000u, 0x800u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 127),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 128),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 129),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 130)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 4u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_SPI_HOST0] = {
        .type = TYPE_OT_SPI_HOST,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x40300000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 131),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 132)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("bus-num", 0)
        ),
    },
    [OT_EARLGREY_SOC_DEV_SPI_HOST1] = {
        .type = TYPE_OT_SPI_HOST,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x40310000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 133),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 134)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("bus-num", 1)
        ),
    },
    [OT_EARLGREY_SOC_DEV_USBDEV] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-usbdev",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40320000u, 0x1000u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_PWRMGR] = {
        .type = TYPE_OT_PWRMGR,
        .memmap = MEMMAPENTRIES(
            { 0x40400000u, 0x80u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 152)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("num-rom", 1u)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("rstmgr", RSTMGR)
        ),
    },
    [OT_EARLGREY_SOC_DEV_RSTMGR] = {
        .type = TYPE_OT_RSTMGR,
        .memmap = MEMMAPENTRIES(
            { 0x40410000u, 0x80u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_SIGNAL(OPENTITAN_RSTMGR_SW_RST, 0, PWRMGR, \
                                   OPENTITAN_PWRMGR_SW_RST_REQ, 0)
        ),
    },
    [OT_EARLGREY_SOC_DEV_CLKMGR] = {
        .type = TYPE_OT_CLKMGR,
        .memmap = MEMMAPENTRIES(
            { 0x40420000u, 0x80u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_SYSRST_CTRL] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-sysrst_ctrl",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40430000u, 0x100u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_ADC_CTRL] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-adc_ctrl",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40440000u, 0x80u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_PWM] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-pwm",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x40450000u, 0x80u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_PINMUX] = {
        .type = TYPE_OT_PINMUX,
        .memmap = MEMMAPENTRIES(
            { 0x40460000u, 0x1000u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_AON_TIMER] = {
        .type = TYPE_OT_AON_TIMER,
        .memmap = MEMMAPENTRIES(
            { 0x40470000u, 0x40u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 155),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 156),
            OT_EARLGREY_SOC_SIGNAL(OPENTITAN_AON_TIMER_WKUP, 0, PWRMGR, \
                                   OPENTITAN_PWRMGR_WKUP_REQ, \
                                   OT_PWRMGR_WAKEUP_AON_TIMER),
            OT_EARLGREY_SOC_SIGNAL(OPENTITAN_AON_TIMER_BITE, 0, PWRMGR, \
                                   OPENTITAN_PWRMGR_RST_REQ,
                                   OT_PWRMGR_RST_REQ_AON_TIMER)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("pclk", OT_EARLGREY_AON_CLK_HZ)
        ),
    },
    [OT_EARLGREY_SOC_DEV_AST] = {
        .type = TYPE_OT_AST,
        .memmap = MEMMAPENTRIES(
            { 0x40480000u, 0x400u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_SENSOR_CTRL] = {
        .type = TYPE_OT_SENSOR,
        .memmap = MEMMAPENTRIES(
            { 0x40490000u, 0x40u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_SRAM_RET_CTRL] = {
        .type = TYPE_OT_SRAM_CTRL,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x40500000u, 0x20u },
            { 0x40600000u, 0x1000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("size", 0x1000u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_FLASH_CTRL] = {
        .type = TYPE_OT_FLASH,
        .cfg = &ot_earlgrey_soc_flash_ctrl_configure,
        .memmap = MEMMAPENTRIES(
            { 0x41000000u, 0x1000u },
            { 0x41008000u, 0x1000u },
            { 0x20000000u, 0x100000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 159),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 160),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 161),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 162),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 163),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 164)
        ),
    },
    [OT_EARLGREY_SOC_DEV_AES] = {
        .type = TYPE_OT_AES,
        .memmap = MEMMAPENTRIES(
            { 0x41100000u, 0x100u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_CLKMGR_HINT(OT_CLKMGR_HINT_AES)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 5u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_HMAC] = {
        .type = TYPE_OT_HMAC,
        .memmap = MEMMAPENTRIES(
            { 0x41110000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 165),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 166),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 167),
            OT_EARLGREY_SOC_CLKMGR_HINT(OT_CLKMGR_HINT_HMAC)
        ),
    },
    [OT_EARLGREY_SOC_DEV_KMAC] = {
        .type = TYPE_OT_KMAC,
        .memmap = MEMMAPENTRIES(
            { 0x41120000u, 0x1000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 168),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 169),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 170)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 3u),
            IBEX_DEV_INT_PROP("num-app", 3u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_OTBN] = {
        .type = TYPE_OT_OTBN,
        .memmap = MEMMAPENTRIES(
            { 0x41130000u, 0x10000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 171),
            OT_EARLGREY_SOC_CLKMGR_HINT(OT_CLKMGR_HINT_OTBN)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("edn-u", EDN0),
            OT_EARLGREY_SOC_DEVLINK("edn-r", EDN1)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-u-ep", 6u),
            IBEX_DEV_INT_PROP("edn-r-ep", 0u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_KEYMGR] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-keymgr",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x41140000u, 0x100u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_CSRNG] = {
        .type = TYPE_OT_CSRNG,
        .memmap = MEMMAPENTRIES(
            { 0x41150000u, 0x80u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 173),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 174),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 175),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 176)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("entropy_src", ENTROPY_SRC),
            OT_EARLGREY_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
    },
    [OT_EARLGREY_SOC_DEV_ENTROPY_SRC] = {
        .type = TYPE_OT_ENTROPY_SRC,
        .memmap = MEMMAPENTRIES(
            { 0x41160000u, 0x100u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 177),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 178),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 179),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 180)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("ast", AST),
            OT_EARLGREY_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
    },
    [OT_EARLGREY_SOC_DEV_EDN0] = {
        .type = TYPE_OT_EDN,
        .instance = 0,
        .memmap = MEMMAPENTRIES(
            { 0x41170000u, 0x80u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 181),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 182)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("csrng", CSRNG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("csrng-app", 0u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_EDN1] = {
        .type = TYPE_OT_EDN,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x41180000u, 0x80u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 183),
            OT_EARLGREY_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 184)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("csrng", CSRNG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("csrng-app", 1u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_SRAM_MAIN_CTRL] = {
        .type = TYPE_OT_SRAM_CTRL,
        .instance = 1,
        .memmap = MEMMAPENTRIES(
            { 0x411c0000u, 0x20u },
            { 0x10000000u, 0x20000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("size", 0x20000u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_ROM_CTRL] = {
        .type = TYPE_OT_ROM_CTRL,
        .name = "ot-rom_ctrl",
        .memmap = MEMMAPENTRIES(
            { 0x411e0000u, 0x80u },
            { 0x00008000u, 0x8000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_SIGNAL(OPENTITAN_ROM_CTRL_GOOD, 0, PWRMGR, \
                                   OPENTITAN_PWRMGR_ROM_GOOD, 0),
            OT_EARLGREY_SOC_SIGNAL(OPENTITAN_ROM_CTRL_DONE, 0, PWRMGR, \
                                   OPENTITAN_PWRMGR_ROM_DONE, 0)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("kmac", KMAC)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("rom_id", "rom"),
            IBEX_DEV_INT_PROP("size", 0x8000u),
            IBEX_DEV_INT_PROP("kmac-app", 2u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_IBEX_WRAPPER] = {
        .type = TYPE_OT_IBEX_WRAPPER,
        .memmap = MEMMAPENTRIES(
            { 0x411f0000u, 0x100u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_EARLGREY_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_INT_PROP("edn-ep", 7u)
        ),
    },
    [OT_EARLGREY_SOC_DEV_RV_DM] = {
        .type = TYPE_UNIMPLEMENTED_DEVICE,
        .name = "ot-rv_dm",
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { 0x41200000u, 0x4u }
        ),
    },
    [OT_EARLGREY_SOC_DEV_PLIC] = {
        .type = TYPE_SIFIVE_PLIC,
        .memmap = MEMMAPENTRIES(
            { 0x48000000u, 0x8000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_EARLGREY_SOC_GPIO(1, HART, IRQ_M_EXT)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("hart-config", "M"),
            IBEX_DEV_UINT_PROP("hartid-base", 0u),
            /* note: should always be max_irq + 1 */
            IBEX_DEV_UINT_PROP("num-sources", 185u),
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
    /* clang-format on */
};

#define PMP_CFG(_l_, _a_, _x_, _w_, _r_) \
    ((uint8_t)(((_l_) << 7u) | ((_a_) << 3u) | ((_x_) << 2u) | ((_w_) << 1u) | \
               ((_r_))))
#define PMP_ADDR(_a_) ((_a_) >> 2u)

#define MSECCFG(_rlb_, _mmwp_, _mml_) \
    (((_rlb_) << 2u) | ((_mmwp_) << 1u) | ((_mml_)))

enum { PMP_MODE_OFF, PMP_MODE_TOR, PMP_MODE_NA4, PMP_MODE_NAPOT };

static const uint8_t ot_earlgrey_pmp_cfgs[] = {
    /* clang-format off */
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(1, PMP_MODE_NAPOT, 1, 0, 1), /* rgn 2  [ROM: LRX]      */
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(1, PMP_MODE_TOR, 0, 1, 1), /* rgn 11 [MMIO: LRW] */
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(1, PMP_MODE_NAPOT, 1, 1, 1), /* rgn 13 [DV_ROM: LRWX]  */
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0),
    PMP_CFG(0, PMP_MODE_OFF, 0, 0, 0)
    /* clang-format on */
};

static const uint32_t ot_earlgrey_pmp_addrs[] = {
    /* clang-format off */
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x000083fc), /* rgn 2 [ROM: base=0x0000_8000 size (2KiB)]      */
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x40000000), /* rgn 10 [MMIO: lo=0x4000_0000]                  */
    PMP_ADDR(0x42010000), /* rgn 11 [MMIO: hi=0x4201_0000]                  */
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x000107fc), /* rgn 13 [DV_ROM: base=0x0001_0000 size (4KiB)]  */
    PMP_ADDR(0x00000000),
    PMP_ADDR(0x00000000)
    /* clang-format on */
};

#define OT_EARLGREY_MSECCFG MSECCFG(1, 1, 0)

enum OtEarlgreyBoardDevice {
    OT_EARLGREY_BOARD_DEV_SOC,
    OT_EARLGREY_BOARD_DEV_FLASH,
    _OT_EARLGREY_BOARD_DEV_COUNT,
};

/* ------------------------------------------------------------------------ */
/* Type definitions */
/* ------------------------------------------------------------------------ */

struct OtEarlGreySoCClass {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

struct OtEarlGreySoCState {
    SysBusDevice parent_obj;

    DeviceState **devices;
};

struct OtEarlGreyBoardState {
    DeviceState parent_obj;

    DeviceState **devices;
};

struct OtEarlGreyMachineState {
    MachineState parent_obj;

    bool no_epmp_cfg;
};

/* ------------------------------------------------------------------------ */
/* Device Configuration */
/* ------------------------------------------------------------------------ */

static void ot_earlgrey_soc_flash_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    DriveInfo *dinfo = drive_get(IF_MTD, 1, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
}

static void ot_earlgrey_soc_hart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    OtEarlGreyMachineState *ms = RISCV_OT_EARLGREY_MACHINE(qdev_get_machine());
    if (ms->no_epmp_cfg) {
        /* skip default PMP config */
        return;
    }

    qdev_prop_set_uint32(dev, PROP_ARRAY_LEN_PREFIX "pmp_cfg",
                         ARRAY_SIZE(ot_earlgrey_pmp_cfgs));
    for (unsigned ix = 0; ix < ARRAY_SIZE(ot_earlgrey_pmp_cfgs); ix++) {
        char *propname = g_strdup_printf("pmp_cfg[%u]", ix);
        qdev_prop_set_uint8(dev, propname, ot_earlgrey_pmp_cfgs[ix]);
        g_free(propname);
    }

    qdev_prop_set_uint32(dev, PROP_ARRAY_LEN_PREFIX "pmp_addr",
                         ARRAY_SIZE(ot_earlgrey_pmp_addrs));
    for (unsigned ix = 0; ix < ARRAY_SIZE(ot_earlgrey_pmp_addrs); ix++) {
        char *propname = g_strdup_printf("pmp_addr[%u]", ix);
        qdev_prop_set_uint64(dev, propname,
                             (uint64_t)ot_earlgrey_pmp_addrs[ix]);
        g_free(propname);
    }
    qdev_prop_set_uint64(dev, "mseccfg", (uint64_t)OT_EARLGREY_MSECCFG);
}

static void ot_earlgrey_soc_otp_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
}

static void ot_earlgrey_soc_uart_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    qdev_prop_set_chr(dev, "chardev", serial_hd(def->instance));
}

/* ------------------------------------------------------------------------ */
/* SoC */
/* ------------------------------------------------------------------------ */

static void ot_earlgrey_soc_reset_hold(Object *obj)
{
    OtEarlGreySoCClass *c = RISCV_OT_EARLGREY_SOC_GET_CLASS(obj);
    OtEarlGreySoCState *s = RISCV_OT_EARLGREY_SOC(obj);

    if (c->parent_phases.hold) {
        c->parent_phases.hold(obj);
    }

    /* keep ROM_CTRL in reset, we'll release it last */
    resettable_assert_reset(OBJECT(s->devices[OT_EARLGREY_SOC_DEV_ROM_CTRL]),
                            RESET_TYPE_COLD);

    cpu_reset(CPU(s->devices[OT_EARLGREY_SOC_DEV_HART]));
}

static void ot_earlgrey_soc_reset_exit(Object *obj)
{
    OtEarlGreySoCClass *c = RISCV_OT_EARLGREY_SOC_GET_CLASS(obj);
    OtEarlGreySoCState *s = RISCV_OT_EARLGREY_SOC(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj);
    }

    /* let ROM_CTRL get out of reset now */
    resettable_release_reset(OBJECT(s->devices[OT_EARLGREY_SOC_DEV_ROM_CTRL]),
                             RESET_TYPE_COLD);
}

static void ot_earlgrey_soc_realize(DeviceState *dev, Error **errp)
{
    OtEarlGreySoCState *s = RISCV_OT_EARLGREY_SOC(dev);

    /* Link, define properties and realize devices, then connect GPIOs */
    ibex_link_devices(s->devices, ot_earlgrey_soc_devices,
                      ARRAY_SIZE(ot_earlgrey_soc_devices));
    ibex_define_device_props(s->devices, ot_earlgrey_soc_devices,
                             ARRAY_SIZE(ot_earlgrey_soc_devices));
    ibex_realize_system_devices(s->devices, ot_earlgrey_soc_devices,
                                ARRAY_SIZE(ot_earlgrey_soc_devices));
    ibex_connect_devices(s->devices, ot_earlgrey_soc_devices,
                         ARRAY_SIZE(ot_earlgrey_soc_devices));

    /* load kernel if provided */
    ibex_load_kernel(NULL);
}

static void ot_earlgrey_soc_init(Object *obj)
{
    OtEarlGreySoCState *s = RISCV_OT_EARLGREY_SOC(obj);

    s->devices =
        ibex_create_devices(ot_earlgrey_soc_devices,
                            ARRAY_SIZE(ot_earlgrey_soc_devices), DEVICE(s));
}

static void ot_earlgrey_soc_class_init(ObjectClass *oc, void *data)
{
    OtEarlGreySoCClass *sc = RISCV_OT_EARLGREY_SOC_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(dc);

    resettable_class_set_parent_phases(rc, NULL, &ot_earlgrey_soc_reset_hold,
                                       &ot_earlgrey_soc_reset_exit,
                                       &sc->parent_phases);
    dc->realize = &ot_earlgrey_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo ot_earlgrey_soc_type_info = {
    .name = TYPE_RISCV_OT_EARLGREY_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtEarlGreySoCState),
    .instance_init = &ot_earlgrey_soc_init,
    .class_init = &ot_earlgrey_soc_class_init,
    .class_size = sizeof(OtEarlGreySoCClass),
};

static void ot_earlgrey_soc_register_types(void)
{
    type_register_static(&ot_earlgrey_soc_type_info);
}

type_init(ot_earlgrey_soc_register_types);

/* ------------------------------------------------------------------------ */
/* Board */
/* ------------------------------------------------------------------------ */

static void ot_earlgrey_board_realize(DeviceState *dev, Error **errp)
{
    OtEarlGreyBoardState *board = RISCV_OT_EARLGREY_BOARD(dev);

    DeviceState *soc = board->devices[OT_EARLGREY_BOARD_DEV_SOC];
    object_property_add_child(OBJECT(board), "soc", OBJECT(soc));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);

    DeviceState *spihost =
        RISCV_OT_EARLGREY_SOC(soc)->devices[OT_EARLGREY_SOC_DEV_SPI_HOST0];
    DeviceState *flash = board->devices[OT_EARLGREY_BOARD_DEV_FLASH];
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

static void ot_earlgrey_board_init(Object *obj)
{
    OtEarlGreyBoardState *s = RISCV_OT_EARLGREY_BOARD(obj);

    s->devices = g_new0(DeviceState *, _OT_EARLGREY_BOARD_DEV_COUNT);
    s->devices[OT_EARLGREY_BOARD_DEV_SOC] =
        qdev_new(TYPE_RISCV_OT_EARLGREY_SOC);
    s->devices[OT_EARLGREY_BOARD_DEV_FLASH] = qdev_new("is25wp128");
}

static void ot_earlgrey_board_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = &ot_earlgrey_board_realize;
}

static const TypeInfo ot_earlgrey_board_type_info = {
    .name = TYPE_RISCV_OT_EARLGREY_BOARD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtEarlGreyBoardState),
    .instance_init = &ot_earlgrey_board_init,
    .class_init = &ot_earlgrey_board_class_init,
};

static void ot_earlgrey_board_register_types(void)
{
    type_register_static(&ot_earlgrey_board_type_info);
}

type_init(ot_earlgrey_board_register_types);

/* ------------------------------------------------------------------------ */
/* Machine */
/* ------------------------------------------------------------------------ */

static bool ot_earlgrey_machine_get_no_epmp_cfg(Object *obj, Error **errp)
{
    OtEarlGreyMachineState *s = RISCV_OT_EARLGREY_MACHINE(obj);

    return s->no_epmp_cfg;
}

static void
ot_earlgrey_machine_set_no_epmp_cfg(Object *obj, bool value, Error **errp)
{
    OtEarlGreyMachineState *s = RISCV_OT_EARLGREY_MACHINE(obj);

    s->no_epmp_cfg = value;
}

static void ot_earlgrey_machine_instance_init(Object *obj)
{
    OtEarlGreyMachineState *s = RISCV_OT_EARLGREY_MACHINE(obj);

    s->no_epmp_cfg = false;
    object_property_add_bool(obj, "no-epmp-cfg",
                             &ot_earlgrey_machine_get_no_epmp_cfg,
                             &ot_earlgrey_machine_set_no_epmp_cfg);
    object_property_set_description(obj, "no-epmp-cfg",
                                    "Skip default ePMP configuration");
}

static void ot_earlgrey_machine_init(MachineState *state)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_OT_EARLGREY_BOARD);

    object_property_add_child(OBJECT(state), "board", OBJECT(dev));
    qdev_realize(dev, NULL, &error_fatal);
}

static void ot_earlgrey_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Board compatible with OpenTitan EarlGrey FPGA platform";
    mc->init = ot_earlgrey_machine_init;
    mc->max_cpus = 1u;
    mc->default_cpu_type =
        ot_earlgrey_soc_devices[OT_EARLGREY_SOC_DEV_HART].type;
    const IbexDeviceDef *sram =
        &ot_earlgrey_soc_devices[OT_EARLGREY_SOC_DEV_SRAM_MAIN_CTRL];
    mc->default_ram_id = sram->type;
    mc->default_ram_size = sram->memmap[1].size;
}

static const TypeInfo ot_earlgrey_machine_type_info = {
    .name = TYPE_RISCV_OT_EARLGREY_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OtEarlGreyMachineState),
    .instance_init = &ot_earlgrey_machine_instance_init,
    .class_init = &ot_earlgrey_machine_class_init,
};

static void ot_earlgrey_machine_register_types(void)
{
    type_register_static(&ot_earlgrey_machine_type_info);
}

type_init(ot_earlgrey_machine_register_types);
