/*
 * QEMU RISC-V Board Compatible with OpenTitan Darjeeling platform
 *
 * Copyright (c) 2023-2025 Rivos, Inc.
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
#include "qapi/error.h"
#include "qapi/qmp/qlist.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/core/split-irq.h"
#include "hw/intc/sifive_plic.h"
#include "hw/jtag/tap_ctrl.h"
#include "hw/jtag/tap_ctrl_rbb.h"
#include "hw/misc/pulp_rv_dm.h"
#include "hw/misc/unimp.h"
#include "hw/opentitan/ot_address_space.h"
#include "hw/opentitan/ot_aes.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_aon_timer.h"
#include "hw/opentitan/ot_ast_dj.h"
#include "hw/opentitan/ot_clkmgr.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_csrng.h"
#include "hw/opentitan/ot_dev_proxy.h"
#include "hw/opentitan/ot_dm_tl.h"
#include "hw/opentitan/ot_dma.h"
#include "hw/opentitan/ot_edn.h"
#include "hw/opentitan/ot_entropy_src_dj.h"
#include "hw/opentitan/ot_gpio_dj.h"
#include "hw/opentitan/ot_hmac.h"
#include "hw/opentitan/ot_i2c.h"
#include "hw/opentitan/ot_ibex_wrapper.h"
#include "hw/opentitan/ot_keymgr_dpe.h"
#include "hw/opentitan/ot_kmac.h"
#include "hw/opentitan/ot_lc_ctrl.h"
#include "hw/opentitan/ot_mbx.h"
#include "hw/opentitan/ot_otbn.h"
#include "hw/opentitan/ot_otp_dj.h"
#include "hw/opentitan/ot_otp_ot_be.h"
#include "hw/opentitan/ot_pinmux_dj.h"
#include "hw/opentitan/ot_plic_ext.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_rom_ctrl.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/opentitan/ot_socdbg_ctrl.h"
#include "hw/opentitan/ot_spi_device.h"
#include "hw/opentitan/ot_spi_host.h"
#include "hw/opentitan/ot_sram_ctrl.h"
#include "hw/opentitan/ot_timer.h"
#include "hw/opentitan/ot_uart.h"
#include "hw/opentitan/ot_unimp.h"
#include "hw/opentitan/ot_vmapper.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/dm.h"
#include "hw/riscv/dtm.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ot_darjeeling.h"
#include "hw/ssi/ssi.h"
#include "sysemu/blockdev.h"
#include "sysemu/hw_accel.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"

/* ------------------------------------------------------------------------ */
/* Forward Declarations */
/* ------------------------------------------------------------------------ */

static void ot_dj_soc_dm_configure(DeviceState *dev, const IbexDeviceDef *def,
                                   DeviceState *parent);
static void ot_dj_soc_hart_configure(DeviceState *dev, const IbexDeviceDef *def,
                                     DeviceState *parent);
static void ot_dj_soc_otp_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_dj_soc_tap_ctrl_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_dj_soc_spi_device_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent);
static void ot_dj_soc_uart_configure(DeviceState *dev, const IbexDeviceDef *def,
                                     DeviceState *parent);

/* ------------------------------------------------------------------------ */
/* Constants */
/* ------------------------------------------------------------------------ */

enum OtDjMemoryRegion {
    OT_DJ_DEFAULT_MEMORY_REGION,
    OT_DJ_CTN_MEMORY_REGION,
    OT_DJ_DEBUG_MEMORY_REGION,
};

enum OtDjSocDevice {
    OT_DJ_SOC_DEV_AC_RANGE_CHECK,
    OT_DJ_SOC_DEV_AES,
    OT_DJ_SOC_DEV_ALERT_HANDLER,
    OT_DJ_SOC_DEV_AON_TIMER,
    OT_DJ_SOC_DEV_AST,
    OT_DJ_SOC_DEV_CLKMGR,
    OT_DJ_SOC_DEV_CSRNG,
    OT_DJ_SOC_DEV_DM_MBX,
    OT_DJ_SOC_DEV_DM_LC_CTRL,
    OT_DJ_SOC_DEV_DM,
    OT_DJ_SOC_DEV_DMA,
    OT_DJ_SOC_DEV_DTM,
    OT_DJ_SOC_DEV_EDN0,
    OT_DJ_SOC_DEV_EDN1,
    OT_DJ_SOC_DEV_ENTROPY_SRC,
    OT_DJ_SOC_DEV_GPIO,
    OT_DJ_SOC_DEV_HART,
    OT_DJ_SOC_DEV_HMAC,
    OT_DJ_SOC_DEV_I2C0,
    OT_DJ_SOC_DEV_IBEX_WRAPPER,
    OT_DJ_SOC_DEV_KEYMGR_DPE,
    OT_DJ_SOC_DEV_KMAC,
    OT_DJ_SOC_DEV_LC_CTRL,
    OT_DJ_SOC_DEV_MBX0,
    OT_DJ_SOC_DEV_MBX1,
    OT_DJ_SOC_DEV_MBX2,
    OT_DJ_SOC_DEV_MBX3,
    OT_DJ_SOC_DEV_MBX4,
    OT_DJ_SOC_DEV_MBX5,
    OT_DJ_SOC_DEV_MBX6,
    OT_DJ_SOC_DEV_MBX_JTAG,
    OT_DJ_SOC_DEV_MBX_PCIE0,
    OT_DJ_SOC_DEV_MBX_PCIE1,
    OT_DJ_SOC_DEV_OTBN,
    OT_DJ_SOC_DEV_OTP_BACKEND,
    OT_DJ_SOC_DEV_OTP_CTRL,
    OT_DJ_SOC_DEV_PINMUX,
    OT_DJ_SOC_DEV_PLIC,
    OT_DJ_SOC_DEV_PLIC_EXT,
    OT_DJ_SOC_DEV_PWRMGR,
    OT_DJ_SOC_DEV_RACL_CTRL,
    OT_DJ_SOC_DEV_ROM_CTRL0,
    OT_DJ_SOC_DEV_ROM_CTRL1,
    OT_DJ_SOC_DEV_RSTMGR,
    OT_DJ_SOC_DEV_RV_DM,
    OT_DJ_SOC_DEV_SOC_DBG_CTRL,
    OT_DJ_SOC_DEV_SOC_PROXY,
    OT_DJ_SOC_DEV_SPI_DEVICE,
    OT_DJ_SOC_DEV_SPI_HOST0,
    OT_DJ_SOC_DEV_SRAM_CTRL_MAIN,
    OT_DJ_SOC_DEV_SRAM_CTRL_MBOX,
    OT_DJ_SOC_DEV_SRAM_CTRL_RET,
    OT_DJ_SOC_DEV_TAP_CTRL,
    OT_DJ_SOC_DEV_TIMER,
    OT_DJ_SOC_DEV_UART0,
    OT_DJ_SOC_DEV_VMAPPER,
    /* IRQ splitters, i.e. 1-to-N signal dispatchers */
    OT_DJ_SOC_SPLITTER_LC_HW_DEBUG,
    OT_DJ_SOC_SPLITTER_LC_ESCALATE,
};

enum OtDjResetRequest {
    OT_DJ_RESET_AON_TIMER,
    OT_DJ_RESET_SOC_PROXY,
    OT_DJ_RESET_COUNT,
};

enum OtDjResetWakeup {
    OT_DJ_WAKEUP_PINMUX_AON_PIN,
    OT_DJ_WAKEUP_PINMUX_AON_USB,
    OT_DJ_WAKEUP_AON_TIMER_AON,
    OT_DJ_WAKEUP_SENSOR_CTRL,
    OT_DJ_WAKEUP_SOC_PROXY_INTERNAL,
    OT_DJ_WAKEUP_SOC_PROXY_EXTERNAL,
    OT_DJ_WAKEUP_COUNT,
};

enum OtDjPinmuxDio {
    DIO_SPI_HOST0_SD0, /* 0 */
    DIO_SPI_HOST0_SD1, /* 1 */
    DIO_SPI_HOST0_SD2, /* 2 */
    DIO_SPI_HOST0_SD3, /* 3 */
    DIO_SPI_DEVICE_SD0, /* 4 */
    DIO_SPI_DEVICE_SD1, /* 5 */
    DIO_SPI_DEVICE_SD2, /* 6 */
    DIO_SPI_DEVICE_SD3, /* 7 */
    DIO_I2C0_SCL, /* 8 */
    DIO_I2C0_SDA, /* 9 */
    DIO_GPIO_GPIO0, /* 10 */
    DIO_GPIO_GPIO1, /* 11 */
    DIO_GPIO_GPIO2, /* 12 */
    DIO_GPIO_GPIO3, /* 13 */
    DIO_GPIO_GPIO4, /* 14 */
    DIO_GPIO_GPIO5, /* 15 */
    DIO_GPIO_GPIO6, /* 16 */
    DIO_GPIO_GPIO7, /* 17 */
    DIO_GPIO_GPIO8, /* 18 */
    DIO_GPIO_GPIO9, /* 19 */
    DIO_GPIO_GPIO10, /* 20 */
    DIO_GPIO_GPIO11, /* 21 */
    DIO_GPIO_GPIO12, /* 22 */
    DIO_GPIO_GPIO13, /* 23 */
    DIO_GPIO_GPIO14, /* 24 */
    DIO_GPIO_GPIO15, /* 25 */
    DIO_GPIO_GPIO16, /* 26 */
    DIO_GPIO_GPIO17, /* 27 */
    DIO_GPIO_GPIO18, /* 28 */
    DIO_GPIO_GPIO19, /* 29 */
    DIO_GPIO_GPIO20, /* 30 */
    DIO_GPIO_GPIO21, /* 31 */
    DIO_GPIO_GPIO22, /* 32 */
    DIO_GPIO_GPIO23, /* 33 */
    DIO_GPIO_GPIO24, /* 34 */
    DIO_GPIO_GPIO25, /* 35 */
    DIO_GPIO_GPIO26, /* 36 */
    DIO_GPIO_GPIO27, /* 37 */
    DIO_GPIO_GPIO28, /* 38 */
    DIO_GPIO_GPIO29, /* 39 */
    DIO_GPIO_GPIO30, /* 40 */
    DIO_GPIO_GPIO31, /* 41 */
    DIO_SPI_DEVICE_SCK, /* 42 */
    DIO_SPI_DEVICE_CSB, /* 43 */
    DIO_SPI_DEVICE_TPM_CSB, /* 44 */
    DIO_UART0_RX, /* 45 */
    DIO_SOC_PROXY_SOC_GPI0, /* 46 */
    DIO_SOC_PROXY_SOC_GPI1, /* 47 */
    DIO_SOC_PROXY_SOC_GPI2, /* 48 */
    DIO_SOC_PROXY_SOC_GPI3, /* 49 */
    DIO_SOC_PROXY_SOC_GPI4, /* 50 */
    DIO_SOC_PROXY_SOC_GPI5, /* 51 */
    DIO_SOC_PROXY_SOC_GPI6, /* 52 */
    DIO_SOC_PROXY_SOC_GPI7, /* 53 */
    DIO_SOC_PROXY_SOC_GPI8, /* 54 */
    DIO_SOC_PROXY_SOC_GPI9, /* 55 */
    DIO_SOC_PROXY_SOC_GPI10, /* 56 */
    DIO_SOC_PROXY_SOC_GPI11, /* 57 */
    DIO_SPI_HOST0_SCK, /* 58 */
    DIO_SPI_HOST0_CSB, /* 59 */
    DIO_UART0_TX, /* 60 */
    DIO_SOC_PROXY_SOC_GPO0, /* 61 */
    DIO_SOC_PROXY_SOC_GPO1, /* 62 */
    DIO_SOC_PROXY_SOC_GPO2, /* 63 */
    DIO_SOC_PROXY_SOC_GPO3, /* 64 */
    DIO_SOC_PROXY_SOC_GPO4, /* 65 */
    DIO_SOC_PROXY_SOC_GPO5, /* 66 */
    DIO_SOC_PROXY_SOC_GPO6, /* 67 */
    DIO_SOC_PROXY_SOC_GPO7, /* 68 */
    DIO_SOC_PROXY_SOC_GPO8, /* 69 */
    DIO_SOC_PROXY_SOC_GPO9, /* 70 */
    DIO_SOC_PROXY_SOC_GPO10, /* 71 */
    DIO_SOC_PROXY_SOC_GPO11, /* 72 */
    DIO_COUNT, /* 73 */
};

enum OtDjPinmuxMioIn {
    MIO_IN_SOC_PROXY_SOC_GPI12, /* 0 */
    MIO_IN_SOC_PROXY_SOC_GPI13, /* 1 */
    MIO_IN_SOC_PROXY_SOC_GPI14, /* 2 */
    MIO_IN_SOC_PROXY_SOC_GPI15, /* 3 */
    MIO_IN_COUNT, /* 4 */
};

enum OtDjPinmuxMioOut {
    MIO_OUT_SOC_PROXY_SOC_GPO12, /* 0 */
    MIO_OUT_SOC_PROXY_SOC_GPO13, /* 1 */
    MIO_OUT_SOC_PROXY_SOC_GPO14, /* 2 */
    MIO_OUT_SOC_PROXY_SOC_GPO15, /* 3 */
    MIO_OUT_OTP_MACRO_TEST0, /* 4 */
    MIO_OUT_COUNT, /* 5 */
};

enum OtDjBoardDevice {
    OT_DJ_BOARD_DEV_SOC,
    OT_DJ_BOARD_DEV_FLASH,
    OT_DJ_BOARD_DEV_DEV_PROXY,
    OT_DJ_BOARD_DEV_COUNT,
};

#define OT_DJ_PRIVATE_REGION_OFFSET 0x00000000u
#define OT_DJ_PRIVATE_REGION_SIZE   (1u << 30u)

#define OT_DJ_IBEX_WRAPPER_NUM_REGIONS 32u

/* CTN address space */
#define OT_DJ_CTN_REGION_OFFSET 0x40000000u
#define OT_DJ_CTN_REGION_SIZE   (1u << 30u)

/* CTN RAM (1MB) */
#define OT_DJ_CTN_RAM_ADDR 0x01000000u
#define OT_DJ_CTN_RAM_SIZE (2u << 20u)

/* DEBUG address space */
#define OT_DJ_DEBUG_RV_DM_ADDR    0x0u
#define OT_DJ_DEBUG_MBX_JTAG_ADDR 0x2200u
#define OT_DJ_DEBUG_LC_CTRL_ADDR  0x3000u
#define OT_DJ_DEBUG_LC_CTRL_SIZE  0x400u
#define OT_DJ_DBG_XBAR_SIZE       0x4000u

static const uint8_t ot_dj_pmp_cfgs[] = {
    /* clang-format off */
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(1, IBEX_PMP_MODE_NAPOT, 1, 0, 1), /* rgn 2  [ROM: LRX] */
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
    IBEX_PMP_CFG(1, IBEX_PMP_MODE_NAPOT, 1, 1, 1), /* rgn 13 [DV_ROM: LRWX] */
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0),
    IBEX_PMP_CFG(0, IBEX_PMP_MODE_OFF, 0, 0, 0)
    /* clang-format on */
};

static const uint32_t ot_dj_pmp_addrs[] = {
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

#define OT_DJ_MSECCFG IBEX_MSECCFG(1, 1, 0)

#define OT_DJ_SOC_RST_REQ TYPE_RISCV_OT_DJ_SOC "-reset"

#define OT_DJ_SOC_GPIO(_irq_, _target_, _num_) \
    IBEX_GPIO(_irq_, OT_DJ_SOC_DEV_##_target_, _num_)

#define OT_DJ_SOC_GPIO_SYSBUS_IRQ(_irq_, _target_, _num_) \
    IBEX_GPIO_SYSBUS_IRQ(_irq_, OT_DJ_SOC_DEV_##_target_, _num_)

#define OT_DJ_SOC_GPIO_ALERT(_snum_, _tnum_) \
    OT_DJ_SOC_SIGNAL(OT_DEVICE_ALERT, _snum_, ALERT_HANDLER, OT_DEVICE_ALERT, \
                     _tnum_)

#define OT_DJ_SOC_GPIO_ESCALATE(_snum_, _tgt_, _tnum_) \
    OT_DJ_SOC_SIGNAL(OT_ALERT_ESCALATE, _snum_, _tgt_, OT_ALERT_ESCALATE, \
                     _tnum_)

#define OT_DJ_SOC_DEVLINK(_pname_, _target_) \
    IBEX_DEVLINK(_pname_, OT_DJ_SOC_DEV_##_target_)

/* Device named signal to device named signal */
#define OT_DJ_SOC_SIGNAL(_sname_, _snum_, _tgt_, _tname_, _tnum_) \
    { \
        .out = { \
            .name = (_sname_), \
            .num = (_snum_), \
        }, \
        .in = { \
            .name = (_tname_), \
            .index = (OT_DJ_SOC_DEV_ ## _tgt_), \
            .num = (_tnum_), \
        } \
    }

/* Device named signal to splitter input */
#define OT_DJ_SOC_D2S(_sname_, _snum_, _tgt_) \
    { \
        .out = { \
            .name = (_sname_), \
            .num = (_snum_), \
        }, \
        .in = { \
            .index = (OT_DJ_SOC_SPLITTER_ ## _tgt_), \
        } \
    }

/* Splitter output to device named signal */
#define OT_DJ_SOC_S2D(_snum_, _tgt_, _tname_, _tnum_) \
    { \
        .out = { \
            .num = (_snum_), \
        }, \
        .in = { \
            .name = (_tname_), \
            .index = (OT_DJ_SOC_DEV_ ## _tgt_), \
            .num = (_tnum_), \
        } \
    }

/* Request link */
#define OT_DJ_SOC_REQ(_req_, _tgt_) \
    OT_DJ_SOC_SIGNAL(_req_##_REQ, 0, _tgt_, _req_##_REQ, 0)

/* Response link */
#define OT_DJ_SOC_RSP(_rsp_, _tgt_) \
    OT_DJ_SOC_SIGNAL(_rsp_##_RSP, 0, _tgt_, _rsp_##_RSP, 0)

#define OT_DJ_SOC_DEV_MBX(_ix_, _addr_, _asname_, _irq_, _alert_) \
    .type = TYPE_OT_MBX, .instance = IBEX_MAKE_INSTANCE_NUM(_ix_), \
    .memmap = MEMMAPENTRIES({ .base = (_addr_) }), \
    .gpio = IBEXGPIOCONNDEFS(OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, (_irq_)), \
                             OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, (_irq_) + 1u), \
                             OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, (_irq_) + 2u), \
                             OT_DJ_SOC_GPIO_ALERT(0, (_alert_)), \
                             OT_DJ_SOC_GPIO_ALERT(1, (_alert_) + 1)), \
    .prop = IBEXDEVICEPROPDEFS(IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, \
                                                    stringify(_ix_)), \
                               IBEX_DEV_STRING_PROP("ram_as_name", _asname_))

#define OT_DJ_SOC_DEV_MBX_DUAL(_ix_, _addr_, _asname_, _irq_, _alert_, \
                               _xaddr_) \
    .type = TYPE_OT_MBX, .instance = IBEX_MAKE_INSTANCE_NUM(_ix_), \
    .memmap = MEMMAPENTRIES({ .base = (_addr_) }, { .base = (_xaddr_) }), \
    .gpio = IBEXGPIOCONNDEFS(OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, (_irq_)), \
                             OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, (_irq_) + 1u), \
                             OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, (_irq_) + 2u), \
                             OT_DJ_SOC_GPIO_ALERT(0, (_alert_)), \
                             OT_DJ_SOC_GPIO_ALERT(1, (_alert_) + 1)), \
    .prop = IBEXDEVICEPROPDEFS(IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, \
                                                    stringify(_ix_)), \
                               IBEX_DEV_STRING_PROP("ram_as_name", _asname_))

#define OT_DJ_PINMUX_LINK(_type_, _name_, _tgt_, _num_) \
    OT_DJ_SOC_SIGNAL(OT_PINMUX_##_type_, _type_##_##_name_, _tgt_, \
                     OT_PINMUX_PAD, (_num_))

#define OT_DJ_SOC_CLOCK_CONN(_src_, _src_type_, _src_name_, _in_name_, \
                             _in_num_) \
    IBEX_CLOCK_CONN((OT_DJ_SOC_DEV_##_src_), _src_type_, _src_name_, \
                    _in_name_, _in_num_)

#define OT_DJ_SOC_CLOCK(_src_, _src_type_, _src_name_) \
    OT_DJ_SOC_CLOCK_CONN(_src_, _src_type_, _src_name_, "clock", 0)

#define OT_DJ_XPORT_MEMORY(_addr_) \
    IBEX_MEMMAP_MAKE_REG((_addr_), OT_DJ_CTN_MEMORY_REGION)

#define DEBUG_MEMORY(_addr_) \
    IBEX_MEMMAP_MAKE_REG((_addr_), OT_DJ_DEBUG_MEMORY_REGION)

#define OT_DJ_DEBUG_TL_TO_DMI(_val_) ((_val_) / sizeof(uint32_t))

#define OT_DJ_DEBUG_LC_CTRL_DMI_ADDR \
    OT_DJ_DEBUG_TL_TO_DMI(OT_DJ_DEBUG_LC_CTRL_ADDR)
#define OT_DJ_DEBUG_LC_CTRL_DMI_SIZE \
    OT_DJ_DEBUG_TL_TO_DMI(OT_DJ_DEBUG_LC_CTRL_SIZE)
#define OT_DJ_DEBUG_MBX_JTAG_DMI_ADDR \
    OT_DJ_DEBUG_TL_TO_DMI(OT_DJ_DEBUG_MBX_JTAG_ADDR)
#define OT_DJ_DEBUG_MBX_JTAG_DMI_SIZE (OT_MBX_SYS_APERTURE / sizeof(uint32_t))

#define OT_DJ_SOC_DM_CONNECTION(_dst_dev_, _num_) \
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

/*
 * Darjeeling RV DM
 * see https://github.com/lowRISC/part-number-registry/blob/main/jtag_partno.md
 */
#define DARJEELING_TAP_IDCODE IBEX_JTAG_IDCODE(1, 1, 0)

#define PULP_DM_BASE 0x00040000u

/*
 * MMIO/interrupt mapping as per:
 * lowRISC/opentitan: hw/top_darjeeling/sw/autogen/top_darjeeling_memory.h
 * and
 * lowRISC/opentitan: hw/top_darjeeling/sw/autogen/top_darjeeling.h
 */
static const IbexDeviceDef ot_dj_soc_devices[] = {
    /* clang-format off */
    [OT_DJ_SOC_DEV_HART] = {
        .type = TYPE_RISCV_CPU_LOWRISC_OPENTITAN,
        .cfg = &ot_dj_soc_hart_configure,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("resetvec", 0x8080u),
            IBEX_DEV_UINT_PROP("mtvec", 0x8001u),
            IBEX_DEV_UINT_PROP("dmhaltvec", PULP_DM_BASE +
                PULP_RV_DM_ROM_BASE + PULP_RV_DM_HALT_OFFSET),
            IBEX_DEV_UINT_PROP("dmexcpvec", PULP_DM_BASE +
                PULP_RV_DM_ROM_BASE + PULP_RV_DM_EXCEPTION_OFFSET),
            IBEX_DEV_BOOL_PROP("start-powered-off", true)
        ),
    },
    [OT_DJ_SOC_DEV_TAP_CTRL] = {
        .type = TYPE_TAP_CTRL_RBB,
        .cfg = &ot_dj_soc_tap_ctrl_configure,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("ir_length", IBEX_TAP_IR_LENGTH),
            IBEX_DEV_UINT_PROP("idcode", DARJEELING_TAP_IDCODE)
        ),
    },
    [OT_DJ_SOC_DEV_DTM] = {
        .type = TYPE_RISCV_DTM,
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("tap_ctrl", TAP_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("abits", 12u)
        ),
    },
    [OT_DJ_SOC_DEV_DM] = {
        .type = TYPE_RISCV_DM,
        .cfg = &ot_dj_soc_dm_configure,
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("dtm", DTM)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("dmi_addr", OT_DJ_DEBUG_RV_DM_ADDR),
            IBEX_DEV_UINT_PROP("dmi_next", 0u), /* last in chain */
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
            IBEX_DEV_BOOL_PROP("abstractauto", true)
        ),
    },
    [OT_DJ_SOC_DEV_DM_MBX] = {
        .type = TYPE_OT_DM_TL,
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("dtm", DTM),
            OT_DJ_SOC_DEVLINK("tl_dev", MBX_JTAG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("dmi_addr", OT_DJ_DEBUG_MBX_JTAG_DMI_ADDR),
            IBEX_DEV_UINT_PROP("dmi_size", OT_DJ_DEBUG_MBX_JTAG_DMI_SIZE),
            IBEX_DEV_UINT_PROP("tl_addr", OT_DJ_DEBUG_MBX_JTAG_ADDR),
            IBEX_DEV_STRING_PROP("tl_as_name", "ot-dbg")
        )
    },
    [OT_DJ_SOC_DEV_DM_LC_CTRL] = {
        .type = TYPE_OT_DM_TL,
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("dtm", DTM),
            OT_DJ_SOC_DEVLINK("tl_dev", LC_CTRL)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("dmi_addr", OT_DJ_DEBUG_LC_CTRL_DMI_ADDR),
            IBEX_DEV_UINT_PROP("dmi_size", OT_DJ_DEBUG_LC_CTRL_DMI_SIZE),
            IBEX_DEV_UINT_PROP("tl_addr", OT_DJ_DEBUG_LC_CTRL_ADDR),
            IBEX_DEV_STRING_PROP("tl_as_name", "ot-dbg")
        )
    },
    [OT_DJ_SOC_DEV_RACL_CTRL] = {
        .type = TYPE_OT_UNIMP,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x1461f00u }
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "racl"),
            IBEX_DEV_UINT_PROP("size", 0x100u),
            IBEX_DEV_UINT_PROP("irq-count", 1u),
            IBEX_DEV_UINT_PROP("alert-count", 2u),
            IBEX_DEV_BOOL_PROP("warn-once", true)
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 130),
            OT_DJ_SOC_GPIO_ALERT(0, 68),
            OT_DJ_SOC_GPIO_ALERT(1, 69)
        ),
    },
    [OT_DJ_SOC_DEV_AC_RANGE_CHECK] = {
        .type = TYPE_OT_UNIMP,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x1464000u }
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "acr"),
            IBEX_DEV_UINT_PROP("size", 0x400u),
            IBEX_DEV_UINT_PROP("irq-count", 1u),
            IBEX_DEV_UINT_PROP("alert-count", 2u),
            IBEX_DEV_BOOL_PROP("warn-once", true)
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 131),
            OT_DJ_SOC_GPIO_ALERT(0, 70),
            OT_DJ_SOC_GPIO_ALERT(1, 71)
        ),
    },
    [OT_DJ_SOC_DEV_AES] = {
        .type = TYPE_OT_AES,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21100000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 24),
            OT_DJ_SOC_GPIO_ALERT(1, 25)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR),
            OT_DJ_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "trans.aes"),
            IBEX_DEV_UINT_PROP("edn-ep", 5u)
        ),
    },
    [OT_DJ_SOC_DEV_HMAC] = {
        .type = TYPE_OT_HMAC,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21110000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 77),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 78),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 79),
            OT_DJ_SOC_GPIO_ALERT(0, 26)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "trans.hmac")
        ),
    },
    [OT_DJ_SOC_DEV_KMAC] = {
        .type = TYPE_OT_KMAC,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21120000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 80),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 81),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 82),
            OT_DJ_SOC_GPIO_ALERT(0, 27),
            OT_DJ_SOC_GPIO_ALERT(1, 28)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR),
            OT_DJ_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "trans.kmac"),
            IBEX_DEV_UINT_PROP("edn-ep", 3u),
            IBEX_DEV_UINT_PROP("num-app", 4u)
        ),
    },
    [OT_DJ_SOC_DEV_OTBN] = {
        .type = TYPE_OT_OTBN,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21130000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 83),
            OT_DJ_SOC_GPIO_ALERT(0, 29),
            OT_DJ_SOC_GPIO_ALERT(1, 30)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR),
            OT_DJ_SOC_DEVLINK("edn-u", EDN0),
            OT_DJ_SOC_DEVLINK("edn-r", EDN1)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "trans.otbn"),
            IBEX_DEV_UINT_PROP("edn-u-ep", 6u),
            IBEX_DEV_UINT_PROP("edn-r-ep", 0u)
        ),
    },
    [OT_DJ_SOC_DEV_KEYMGR_DPE] = {
        .type = TYPE_OT_KEYMGR_DPE,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21140000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 84),
            OT_DJ_SOC_GPIO_ALERT(0, 31),
            OT_DJ_SOC_GPIO_ALERT(1, 32)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("aes", AES),
            OT_DJ_SOC_DEVLINK("edn", EDN0),
            OT_DJ_SOC_DEVLINK("kmac", KMAC),
            OT_DJ_SOC_DEVLINK("otbn", OTBN),
            OT_DJ_SOC_DEVLINK("lc_ctrl", LC_CTRL),
            OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL),
            OT_DJ_SOC_DEVLINK("rom0", ROM_CTRL0),
            OT_DJ_SOC_DEVLINK("rom1", ROM_CTRL1)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("edn-ep", 0u),
            IBEX_DEV_UINT_PROP("kmac-app", 0u)
        ),
    },
    [OT_DJ_SOC_DEV_CSRNG] = {
        .type = TYPE_OT_CSRNG,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21150000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 85),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 86),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 87),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 88),
            OT_DJ_SOC_GPIO_ALERT(0, 33),
            OT_DJ_SOC_GPIO_ALERT(1, 34)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("random_src", AST),
            OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
        ),
    },
    [OT_DJ_SOC_DEV_ENTROPY_SRC] = {
        .type = TYPE_OT_UNIMP,
        .cfg = &ibex_unimp_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21160000u }
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "entropy_src"),
            IBEX_DEV_UINT_PROP("size", 0x100u),
            IBEX_DEV_UINT_PROP("alert-count", 2u),
            IBEX_DEV_BOOL_PROP("warn-once", true)
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 35),
            OT_DJ_SOC_GPIO_ALERT(1, 36)
        ),
    },
    /*
     * @todo enable DJ entropy source once implemented, replacing the above
     *       unimplemented fake device
     *
     * [OT_DJ_SOC_DEV_ENTROPY_SRC] = {
     *     .type = TYPE_OT_ENTROPY_SRC,
     *     .memmap = MEMMAPENTRIES(
     *      { .base = 0x21160000u }
     *     ),
     *     .gpio = IBEXGPIOCONNDEFS(
     *      OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 89),
     *      OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 90),
     *      OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 91),
     *      OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 92),
     *      OT_DJ_SOC_GPIO_ALERT(0, 35),
     *      OT_DJ_SOC_GPIO_ALERT(1, 36)
     *     ),
     *     .link = IBEXDEVICELINKDEFS(
     *      OT_DJ_SOC_DEVLINK("ast", AST),
     *      OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL)
     *     ),
     * },
     */
    [OT_DJ_SOC_DEV_EDN0] = {
        .type = TYPE_OT_EDN,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21170000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 93),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 94),
            OT_DJ_SOC_GPIO_ALERT(0, 37),
            OT_DJ_SOC_GPIO_ALERT(1, 38)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("csrng", CSRNG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("csrng-app", 0)
        ),
    },
    [OT_DJ_SOC_DEV_EDN1] = {
        .type = TYPE_OT_EDN,
        .memmap = MEMMAPENTRIES(
            { .base = 0x21180000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 95),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 96),
            OT_DJ_SOC_GPIO_ALERT(0, 39),
            OT_DJ_SOC_GPIO_ALERT(1, 40)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("csrng", CSRNG)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("csrng-app", 1u)
        ),
    },
    [OT_DJ_SOC_DEV_SRAM_CTRL_MAIN] = {
        .type = TYPE_OT_SRAM_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x211c0000u },
            { .base = 0x10000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 41)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL),
            OT_DJ_SOC_DEVLINK("vmapper", VMAPPER)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("size", 0x10000u),
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "ram"),
            IBEX_DEV_BOOL_PROP("ifetch", true)
        ),
    },
    [OT_DJ_SOC_DEV_SRAM_CTRL_MBOX] = {
        .type = TYPE_OT_SRAM_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x211d0000u },
            { .base = 0x11000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 42)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL),
            OT_DJ_SOC_DEVLINK("vmapper", VMAPPER)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("size", 0x1000u),
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "mbx"),
            IBEX_DEV_BOOL_PROP("ifetch", false)
        ),
    },
    [OT_DJ_SOC_DEV_ROM_CTRL0] = {
        .type = TYPE_OT_ROM_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x211e0000u },
            { .base = 0x00008000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 43),
            OT_DJ_SOC_SIGNAL(OT_ROM_CTRL_GOOD, 0, PWRMGR,
                             OT_PWRMGR_ROM_GOOD, 0),
            OT_DJ_SOC_SIGNAL(OT_ROM_CTRL_DONE, 0, PWRMGR,
                             OT_PWRMGR_ROM_DONE, 0)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("kmac", KMAC)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "rom0"),
            IBEX_DEV_UINT_PROP("size", 0x8000u),
            IBEX_DEV_UINT_PROP("kmac-app", 2u),
            IBEX_DEV_STRING_PROP("nonce", "a7bdb05fe921615b"),
            IBEX_DEV_STRING_PROP("key", "f2ff984540f7d43ece76b4eb13363774")
        ),
    },
    [OT_DJ_SOC_DEV_ROM_CTRL1] = {
        .type = TYPE_OT_ROM_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x211e1000u },
            { .base = 0x00020000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 44),
            OT_DJ_SOC_SIGNAL(OT_ROM_CTRL_GOOD, 0, PWRMGR,
                             OT_PWRMGR_ROM_GOOD, 1),
            OT_DJ_SOC_SIGNAL(OT_ROM_CTRL_DONE, 0, PWRMGR,
                             OT_PWRMGR_ROM_DONE, 1)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("kmac", KMAC)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "rom1"),
            IBEX_DEV_UINT_PROP("size", 0x10000u),
            IBEX_DEV_UINT_PROP("kmac-app", 3u),
            IBEX_DEV_STRING_PROP("nonce", "ed2ed45545e927f6"),
            IBEX_DEV_STRING_PROP("key", "d9e4abac398d42c745eef646c1464dca")
        ),
    },
    [OT_DJ_SOC_DEV_IBEX_WRAPPER] = {
        .type = TYPE_OT_IBEX_WRAPPER,
        .memmap = MEMMAPENTRIES(
            { .base = 0x211f0000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 72),
            OT_DJ_SOC_GPIO_ALERT(1, 73),
            OT_DJ_SOC_GPIO_ALERT(2, 74),
            OT_DJ_SOC_GPIO_ALERT(3, 75)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("edn", EDN0),
            OT_DJ_SOC_DEVLINK("vmapper", VMAPPER)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("edn-ep", 7u),
            IBEX_DEV_UINT_PROP("num-regions", OT_DJ_IBEX_WRAPPER_NUM_REGIONS)
        ),
    },
    [OT_DJ_SOC_DEV_RV_DM] = {
        .type = TYPE_PULP_RV_DM,
        .memmap = MEMMAPENTRIES(
            { .base = PULP_DM_BASE },
            { .base = 0x21200000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_DM_CONNECTION(OT_DJ_SOC_DEV_DM, 0),
            OT_DJ_SOC_DM_CONNECTION(OT_DJ_SOC_DEV_DM, 1),
            OT_DJ_SOC_DM_CONNECTION(OT_DJ_SOC_DEV_DM, 2),
            OT_DJ_SOC_DM_CONNECTION(OT_DJ_SOC_DEV_DM, 3),
            OT_DJ_SOC_GPIO_ALERT(0, 53)
        ),
    },
    [OT_DJ_SOC_DEV_MBX0] = {
        OT_DJ_SOC_DEV_MBX(0, 0x22000000u, "ot-mbx.sram", 100, 46),
    },
    [OT_DJ_SOC_DEV_MBX1] = {
        OT_DJ_SOC_DEV_MBX(1, 0x22000100u, "ot-mbx.sram", 103, 48),
    },
    [OT_DJ_SOC_DEV_MBX2] = {
        OT_DJ_SOC_DEV_MBX(2, 0x22000200u, "ot-mbx.sram", 106, 50),
    },
    [OT_DJ_SOC_DEV_MBX3] = {
        OT_DJ_SOC_DEV_MBX(3, 0x22000300u, "ot-mbx.sram", 109, 52),
    },
    [OT_DJ_SOC_DEV_MBX4] = {
        OT_DJ_SOC_DEV_MBX(4, 0x22000400u, "ot-mbx.sram", 112, 54),
    },
    [OT_DJ_SOC_DEV_MBX5] = {
        OT_DJ_SOC_DEV_MBX(5, 0x22000500u, "ot-mbx.sram", 115, 56),
    },
    [OT_DJ_SOC_DEV_MBX6] = {
        OT_DJ_SOC_DEV_MBX(6, 0x22000600u, "ot-mbx.sram", 118, 58),
    },
    [OT_DJ_SOC_DEV_MBX_JTAG] = {
        OT_DJ_SOC_DEV_MBX_DUAL(7, 0x22000800u, "ot-mbx.sram", 121, 60,
                               DEBUG_MEMORY(OT_DJ_DEBUG_MBX_JTAG_ADDR)),
    },
    [OT_DJ_SOC_DEV_DMA] = {
        .type = TYPE_OT_DMA,
        .memmap = MEMMAPENTRIES(
            { .base = 0x22010000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 97),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 98),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 99),
            OT_DJ_SOC_GPIO_ALERT(0, 45)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("ot_as_name", "ot-dma"),
            IBEX_DEV_STRING_PROP("ctn_as_name", "ctn-dma")
        )
    },
    // todo: obsolete SOC_PROXY
    [OT_DJ_SOC_DEV_MBX_PCIE0] = {
        OT_DJ_SOC_DEV_MBX(8, 0x22040000u, "ot-mbx.sram", 124, 62),
    },
    [OT_DJ_SOC_DEV_MBX_PCIE1] = {
        OT_DJ_SOC_DEV_MBX(9, 0x22040100u, "ot-mbx.sram", 127, 64),
    },
    [OT_DJ_SOC_DEV_PLIC] = {
        .type = TYPE_SIFIVE_PLIC,
        .memmap = MEMMAPENTRIES(
            { .base = 0x28000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO(1, HART, IRQ_M_EXT)
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
            IBEX_DEV_UINT_PROP("aperture-size", 0x4000000u)
        ),
    },
    [OT_DJ_SOC_DEV_PLIC_EXT] = {
        .type = TYPE_OT_PLIC_EXT,
        .memmap = MEMMAPENTRIES(
            { .base = 0x2c000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO(0, HART, IRQ_M_SOFT),
            OT_DJ_SOC_GPIO_ALERT(0, 23)
        ),
    },
    [OT_DJ_SOC_DEV_GPIO] = {
        .type = TYPE_OT_GPIO_DJ,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30000000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 10),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 11),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 12),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 13),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 14),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 15),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 16),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 17),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(8, PLIC, 18),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(9, PLIC, 19),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(10, PLIC, 20),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(11, PLIC, 21),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(12, PLIC, 22),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(13, PLIC, 23),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(14, PLIC, 24),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(15, PLIC, 25),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(16, PLIC, 26),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(17, PLIC, 27),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(18, PLIC, 28),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(19, PLIC, 29),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(20, PLIC, 30),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(21, PLIC, 31),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(22, PLIC, 32),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(23, PLIC, 33),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(24, PLIC, 34),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(25, PLIC, 35),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(26, PLIC, 36),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(27, PLIC, 37),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(28, PLIC, 38),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(29, PLIC, 39),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(30, PLIC, 40),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(31, PLIC, 41),
            OT_DJ_SOC_GPIO_ALERT(0, 1)
        )
    },
    [OT_DJ_SOC_DEV_UART0] = {
        .type = TYPE_OT_UART,
        .cfg = &ot_dj_soc_uart_configure,
        .instance = IBEX_MAKE_INSTANCE_NUM(0),
        .memmap = MEMMAPENTRIES(
            { .base = 0x30010000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 1),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 2),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 3),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 4),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 5),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 6),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 7),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 8),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(8, PLIC, 9),
            OT_DJ_SOC_GPIO_ALERT(0, 0)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "peri.io")
        ),
    },
    [OT_DJ_SOC_DEV_I2C0] = {
        .type = TYPE_OT_I2C,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30080000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 50),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 51),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 52),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 53),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 54),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 55),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 56),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 57),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(8, PLIC, 58),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(9, PLIC, 59),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(10, PLIC, 60),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(11, PLIC, 61),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(12, PLIC, 62),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(13, PLIC, 63),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(14, PLIC, 64),
            OT_DJ_SOC_GPIO_ALERT(0, 3)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "peri.io")
        ),
    },
    [OT_DJ_SOC_DEV_TIMER] = {
        .type = TYPE_OT_TIMER,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30100000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO(0, HART, IRQ_M_TIMER),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 65),
            OT_DJ_SOC_GPIO_ALERT(0, 4)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "timers.io")
        ),
    },
    [OT_DJ_SOC_DEV_OTP_CTRL] = {
        .type = TYPE_OT_OTP_DJ,
        .cfg = &ot_dj_soc_otp_ctrl_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30130000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 66),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 67),
            OT_DJ_SOC_GPIO_ALERT(0, 5),
            OT_DJ_SOC_GPIO_ALERT(1, 6),
            OT_DJ_SOC_GPIO_ALERT(2, 7),
            OT_DJ_SOC_GPIO_ALERT(3, 8),
            OT_DJ_SOC_GPIO_ALERT(4, 9),
            OT_DJ_SOC_RSP(OT_PWRMGR_OTP, PWRMGR)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("edn", EDN0),
            OT_DJ_SOC_DEVLINK("backend", OTP_BACKEND)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("edn-ep", 1u)
        ),
    },
    [OT_DJ_SOC_DEV_OTP_BACKEND] = {
        .type = TYPE_OT_OTP_OT_BE,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30140000u }
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("parent", OTP_CTRL)
        ),
    },
    [OT_DJ_SOC_DEV_LC_CTRL] = {
        .type = TYPE_OT_LC_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30150000u },
            { .base = DEBUG_MEMORY(OT_DJ_DEBUG_LC_CTRL_ADDR) }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 10),
            OT_DJ_SOC_GPIO_ALERT(1, 11),
            OT_DJ_SOC_GPIO_ALERT(2, 12),
            OT_DJ_SOC_D2S(OT_LC_BROADCAST, OT_LC_HW_DEBUG_EN, LC_HW_DEBUG),
            OT_DJ_SOC_D2S(OT_LC_BROADCAST, OT_LC_ESCALATE_EN, LC_ESCALATE),
            OT_DJ_SOC_SIGNAL(OT_LC_BROADCAST, OT_LC_CPU_EN, IBEX_WRAPPER,
                             OT_IBEX_WRAPPER_CPU_EN, OT_IBEX_LC_CTRL_CPU_EN),
            OT_DJ_SOC_SIGNAL(OT_LC_BROADCAST, OT_LC_CHECK_BYP_EN, OTP_CTRL,
                             OT_LC_BROADCAST, OT_OTP_LC_CHECK_BYP_EN),
            OT_DJ_SOC_SIGNAL(OT_LC_BROADCAST, OT_LC_KEYMGR_EN, KEYMGR_DPE,
                             OT_KEYMGR_DPE_ENABLE, 0),
            OT_DJ_SOC_SIGNAL(OT_LC_BROADCAST, OT_LC_CREATOR_SEED_SW_RW_EN,
                             OTP_CTRL, OT_LC_BROADCAST,
                             OT_OTP_LC_CREATOR_SEED_SW_RW_EN),
            OT_DJ_SOC_SIGNAL(OT_LC_BROADCAST, OT_LC_OWNER_SEED_SW_RW_EN,
                             OTP_CTRL, OT_LC_BROADCAST,
                             OT_OTP_LC_OWNER_SEED_SW_RW_EN),
            OT_DJ_SOC_SIGNAL(OT_LC_BROADCAST, OT_LC_SEED_HW_RD_EN,
                             OTP_CTRL, OT_LC_BROADCAST,
                             OT_OTP_LC_SEED_HW_RD_EN),
            OT_DJ_SOC_RSP(OT_PWRMGR_LC, PWRMGR)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL),
            OT_DJ_SOC_DEVLINK("kmac", KMAC)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("silicon_creator_id", 0x4002u),
            IBEX_DEV_UINT_PROP("product_id", 0x4000u),
            IBEX_DEV_UINT_PROP("revision_id", 0x1u),
            IBEX_DEV_BOOL_PROP("volatile_raw_unlock", true),
            IBEX_DEV_UINT_PROP("kmac-app", 1u),
            IBEX_DEV_STRING_PROP("raw_unlock_token",
                                 "ea2b3f32cbe77554e43c8ea7ebf197c2"),
            IBEX_DEV_STRING_PROP("lc_state_first",
                "ee75b407d2314d2ef84185ac8c990f536071632c"
                "086d4c924070be92d2948d6228b2711e9b2d8c4d"),
            IBEX_DEV_STRING_PROP("lc_state_last",
                "ee75fe0ffe7b6f3ffc5f9ffd9ff96fdb7f736f6c"
                "9e6fdcd35277fef2d3bdcd6ffbb2f59fdf3fbedd"),
            IBEX_DEV_STRING_PROP("lc_trscnt_first",
                "dfb6c45a241f85ce9f42229e8627462fdb02c6701242f14b"
                "41891180045c09c26c52744267c04aa055921b9461bb07da"),
            IBEX_DEV_STRING_PROP("lc_trscnt_last",
                "dfb6f4fabf1fefcebf5ba2ffc677c6afdbabcefeb672f36b"
                "4fbdb3988dfe1be67e7e77ca77c76af7ddde3b9e7fbfe7de")
        )
    },
    [OT_DJ_SOC_DEV_ALERT_HANDLER] = {
        .type = TYPE_OT_ALERT,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30160000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 68),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 69),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 70),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 71),
            OT_DJ_SOC_GPIO_ESCALATE(0, IBEX_WRAPPER, 0),
            OT_DJ_SOC_GPIO_ESCALATE(1, LC_CTRL, 0),
            OT_DJ_SOC_GPIO_ESCALATE(2, LC_CTRL, 1),
            OT_DJ_SOC_GPIO_ESCALATE(3, PWRMGR, 0)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR),
            OT_DJ_SOC_DEVLINK("edn", EDN0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("n_alerts", 99u),
            IBEX_DEV_UINT_PROP("n_classes", 4u),
            IBEX_DEV_UINT_PROP("n_lpg", 18u),
            IBEX_DEV_UINT_PROP("edn-ep", 4u),
            IBEX_DEV_STRING_PROP("clock-name", "secure.io"),
            IBEX_DEV_STRING_PROP("clock-name-edn", "secure.main")
        ),
    },
    [OT_DJ_SOC_DEV_SOC_DBG_CTRL] = {
        /* @todo: should be renamed TYPE_OT_SOC_DBG_CTRL */
        .type = TYPE_OT_SOCDBG_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30170000u }
        ),
        /*
         * @todo: add alert signals once OT_SOC_DBG_CTRL supports them
         *
         * .gpio = IBEXGPIOCONNDEFS(
         *     OT_DJ_SOC_GPIO_ALERT(0, 66),
         *     OT_DJ_SOC_GPIO_ALERT(1, 67)
         * ),
         */
    },
    [OT_DJ_SOC_DEV_SPI_HOST0] = {
        .type = TYPE_OT_SPI_HOST,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30300000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 72),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 73),
            OT_DJ_SOC_GPIO_ALERT(0, 13)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "spi0"),
            IBEX_DEV_UINT_PROP("bus-num", 0),
            IBEX_DEV_STRING_PROP("clock-name", "peri.io"),
            IBEX_DEV_UINT_PROP("version", OT_SPI_HOST_VERSION_DJ_PRE)
        ),
    },
    [OT_DJ_SOC_DEV_SPI_DEVICE] = {
        .type = TYPE_OT_SPI_DEVICE,
        .cfg = &ot_dj_soc_spi_device_configure,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30310000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 42),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 43),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(2, PLIC, 44),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(3, PLIC, 45),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(4, PLIC, 46),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(5, PLIC, 47),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(6, PLIC, 48),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(7, PLIC, 49),
            OT_DJ_SOC_GPIO_ALERT(0, 2)
        ),
    },
    [OT_DJ_SOC_DEV_PWRMGR] = {
        .type = TYPE_OT_PWRMGR,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30400000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 74),
            OT_DJ_SOC_GPIO_ALERT(0, 14),
            OT_DJ_SOC_REQ(OT_PWRMGR_OTP, OTP_CTRL),
            OT_DJ_SOC_REQ(OT_PWRMGR_LC, LC_CTRL),
            OT_DJ_SOC_SIGNAL(OT_PWRMGR_CPU_EN, 0, IBEX_WRAPPER,
                             OT_IBEX_WRAPPER_CPU_EN, OT_IBEX_PWRMGR_CPU_EN),
            OT_DJ_SOC_SIGNAL(OT_PWRMGR_STRAP, 0, GPIO,
                             OT_GPIO_STRAP_EN, 0),
            OT_DJ_SOC_SIGNAL(OT_PWRMGR_RST_REQ, 0, RSTMGR,
                             OT_RSTMGR_RST_REQ, 0)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock_ctrl", AST)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clocks", "main,io,usb"),
            IBEX_DEV_UINT_PROP("num-rom", 2u),
            IBEX_DEV_UINT_PROP("version", OT_PWRMGR_VERSION_DJ_PRE)
        ),
    },
    [OT_DJ_SOC_DEV_RSTMGR] = {
        .type = TYPE_OT_RSTMGR,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30410000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 15),
            OT_DJ_SOC_GPIO_ALERT(1, 16),
            OT_DJ_SOC_SIGNAL(OT_RSTMGR_SW_RST, 0, PWRMGR,
                             OT_PWRMGR_SW_RST, 0)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("version", OT_RSTMGR_VERSION_DJ_PRE)
        ),
    },
    [OT_DJ_SOC_DEV_CLKMGR] = {
        .type = TYPE_OT_CLKMGR,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30420000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 17),
            OT_DJ_SOC_GPIO_ALERT(1, 18)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", AST)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("topclocks", "main:16,io:16,usb:16,aon:1"),
            IBEX_DEV_STRING_PROP("refclock", "aon"),
            IBEX_DEV_STRING_PROP("subclocks",
                "io_div2:io:2,io_div4:io:4,"
                "aes:main:1,hmac:main:1,kmac:main:1,otbn:main:1"),
            IBEX_DEV_STRING_PROP("groups",
                "powerup:io_div4+aon+main+io+usb+io_div2,"
                "trans:aes+hmac+kmac+otbn,"
                "infra:io_div4+main+aon+usb+io,"
                "secure:io_div4+main+aon,"
                "peri:io_div4+io_div2+io+aon+usb,"
                "timers:io_div4+aon"),
            IBEX_DEV_STRING_PROP("swcg", "peri"),
            IBEX_DEV_STRING_PROP("hint", "trans"),
            IBEX_DEV_UINT_PROP("version", OT_CLKMGR_VERSION_DJ_PRE)
        ),
    },
    [OT_DJ_SOC_DEV_PINMUX] = {
        .type = TYPE_OT_PINMUX_DJ,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30460000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO0, GPIO, 0),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO1, GPIO, 1),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO2, GPIO, 2),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO3, GPIO, 3),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO4, GPIO, 4),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO5, GPIO, 5),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO6, GPIO, 6),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO7, GPIO, 7),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO8, GPIO, 8),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO9, GPIO, 9),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO10, GPIO, 10),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO11, GPIO, 11),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO12, GPIO, 12),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO13, GPIO, 13),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO14, GPIO, 14),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO15, GPIO, 15),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO16, GPIO, 16),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO17, GPIO, 17),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO18, GPIO, 18),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO19, GPIO, 19),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO20, GPIO, 20),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO21, GPIO, 21),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO22, GPIO, 22),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO23, GPIO, 23),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO24, GPIO, 24),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO25, GPIO, 25),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO26, GPIO, 26),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO27, GPIO, 27),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO28, GPIO, 28),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO29, GPIO, 29),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO30, GPIO, 30),
            OT_DJ_PINMUX_LINK(DIO, GPIO_GPIO31, GPIO, 31),
            OT_DJ_SOC_GPIO_ALERT(0, 19)
        ),
    },
    [OT_DJ_SOC_DEV_AON_TIMER] = {
        .type = TYPE_OT_AON_TIMER,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30470000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(0, PLIC, 75),
            OT_DJ_SOC_GPIO_SYSBUS_IRQ(1, PLIC, 76),
            OT_DJ_SOC_GPIO_ALERT(0, 20),
            OT_DJ_SOC_SIGNAL(OT_AON_TIMER_WKUP, 0, PWRMGR,
                             OT_PWRMGR_WKUP, OT_PWRMGR_WAKEUP_AON_TIMER),
            OT_DJ_SOC_SIGNAL(OT_AON_TIMER_BITE, 0, PWRMGR,
                             OT_PWRMGR_RST, OT_DJ_RESET_AON_TIMER)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("clock-src", CLKMGR)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("clock-name", "timers.io"),
            IBEX_DEV_STRING_PROP("clock-name-aon", "timers.aon")
        ),
    },
    [OT_DJ_SOC_DEV_AST] = {
        .type = TYPE_OT_AST_DJ,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30480000u }
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP("topclocks",
                "main:1000000000,io:1000000000,usb:1000000000,aon:62500000"),
            IBEX_DEV_STRING_PROP("aonclocks", "aon")
        ),
    },
    [OT_DJ_SOC_DEV_SRAM_CTRL_RET] = {
        .type = TYPE_OT_SRAM_CTRL,
        .memmap = MEMMAPENTRIES(
            { .base = 0x30500000u },
            { .base = 0x30600000u }
        ),
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_GPIO_ALERT(0, 21)
        ),
        .link = IBEXDEVICELINKDEFS(
            OT_DJ_SOC_DEVLINK("otp_ctrl", OTP_CTRL),
            OT_DJ_SOC_DEVLINK("vmapper", VMAPPER)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("size", 0x1000u),
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "ret"),
            IBEX_DEV_BOOL_PROP("ifetch", false)
        ),
    },
    [OT_DJ_SOC_DEV_VMAPPER] = {
        .type = TYPE_OT_VMAPPER,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_STRING_PROP(OT_COMMON_DEV_ID, "soc"),
            IBEX_DEV_UINT_PROP("trans_count", OT_DJ_IBEX_WRAPPER_NUM_REGIONS)
        ),
    },
    /* IRQ splitters */
    [OT_DJ_SOC_SPLITTER_LC_HW_DEBUG] = {
        .type = TYPE_SPLIT_IRQ,
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("num-lines", 1u) // to be changed
        )
    },
    [OT_DJ_SOC_SPLITTER_LC_ESCALATE] = {
        .type = TYPE_SPLIT_IRQ,
        .gpio = IBEXGPIOCONNDEFS(
            OT_DJ_SOC_S2D(0, OTP_CTRL, OT_LC_BROADCAST,
                                  OT_OTP_LC_ESCALATE_EN)
        ),
        .prop = IBEXDEVICEPROPDEFS(
            IBEX_DEV_UINT_PROP("num-lines", 1u) // to be changed
        )
    }
    /* clang-format on */
};

/* ------------------------------------------------------------------------ */
/* Type definitions */
/* ------------------------------------------------------------------------ */

/* Temporary storage for reset iteration */
typedef struct {
    OtDjSoCState *soc;
    ResettableChildCallback cb; /* the callback to call for each child */
    void *opaque; /* opaque data for the callback */
    ResetType type; /* type of reset */
} OtDjSocChildReset;

struct OtDjSoCClass {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

struct OtDjSoCState {
    SysBusDevice parent_obj;

    BusState *ot_bus; /* private OpenTitan bus */

    DeviceState **devices;
};

struct OtDjBoardState {
    DeviceState parent_obj;

    DeviceState **devices;
};

struct OtDjMachineState {
    MachineState parent_obj;

    ResettableState reset;

    bool no_epmp_cfg;
    bool ignore_elf_entry;
};

/* ------------------------------------------------------------------------ */
/* Device Configuration */
/* ------------------------------------------------------------------------ */

static void ot_dj_soc_dm_configure(DeviceState *dev, const IbexDeviceDef *def,
                                   DeviceState *parent)
{
    (void)def;
    (void)parent;

    QList *hart = qlist_new();
    qlist_append_int(hart, 0);
    qdev_prop_set_array(dev, "hart", hart);

    RISCVDMMemAttrs pulp_attrs = {
        .attrs = {
            .requester_id = PULP_RV_DM_REQUESTER_ID,
        },
    };
    qdev_prop_set_uint64(dev, "mta_dm", pulp_attrs.value);
}

static void ot_dj_soc_hart_configure(DeviceState *dev, const IbexDeviceDef *def,
                                     DeviceState *parent)
{
    OtDjMachineState *ms = RISCV_OT_DJ_MACHINE(qdev_get_machine());
    QList *pmp_cfg, *pmp_addr;
    (void)def;
    (void)parent;

    if (ms->no_epmp_cfg) {
        /* skip default PMP config */
        return;
    }

    pmp_cfg = qlist_new();
    for (unsigned ix = 0; ix < ARRAY_SIZE(ot_dj_pmp_cfgs); ix++) {
        qlist_append_int(pmp_cfg, ot_dj_pmp_cfgs[ix]);
    }
    qdev_prop_set_array(dev, "pmp_cfg", pmp_cfg);

    pmp_addr = qlist_new();
    for (unsigned ix = 0; ix < ARRAY_SIZE(ot_dj_pmp_addrs); ix++) {
        qlist_append_int(pmp_addr, ot_dj_pmp_addrs[ix]);
    }
    qdev_prop_set_array(dev, "pmp_addr", pmp_addr);

    qdev_prop_set_uint64(dev, "mseccfg", (uint64_t)OT_DJ_MSECCFG);
}

static void ot_dj_soc_otp_ctrl_configure(
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

static void ot_dj_soc_tap_ctrl_configure(
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

static void ot_dj_soc_spi_device_configure(
    DeviceState *dev, const IbexDeviceDef *def, DeviceState *parent)
{
    (void)parent;
    (void)def;

    Chardev *chr;

    chr = ibex_get_chardev_by_id("spidev");
    if (chr) {
        qdev_prop_set_chr(dev, "chardev", chr);
    }
}

static void ot_dj_soc_uart_configure(DeviceState *dev, const IbexDeviceDef *def,
                                     DeviceState *parent)
{
    (void)def;
    (void)parent;
    qdev_prop_set_chr(dev, "chardev", serial_hd(IBEX_GET_INSTANCE_NUM(def)));
}

/* ------------------------------------------------------------------------ */
/* SoC */
/* ------------------------------------------------------------------------ */

static int ot_dj_soc_reset_child(Object *child, void *opaque)
{
    OtDjSocChildReset *cr = opaque;

    if (object_dynamic_cast(child, TYPE_SYS_BUS_DEVICE)) {
        /*
         * sysbus devices being connected to the OT bus, and the bus performing
         * its own children traversal to perform reset, skip this kind of
         * devices to avoid resetting each of them twice
         */
        return 0;
    }

    if (object_dynamic_cast(child, TYPE_RESETTABLE_INTERFACE)) {
        cr->cb(child, cr->opaque, cr->type);
    }

    /* resume with next child */
    return 0;
}

static void ot_dj_soc_reset_child_foreach(
    Object *obj, ResettableChildCallback cb, void *opaque, ResetType type)
{
    OtDjSoCState *s = RISCV_OT_DJ_SOC(obj);

    OtDjSocChildReset r = {
        .soc = s,
        .cb = cb,
        .opaque = opaque,
        .type = type,
    };

    /* execute reset stage for each child */
    object_child_foreach(obj, &ot_dj_soc_reset_child, &r);
}

static void ot_dj_soc_hw_reset(void *opaque, int irq, int level)
{
    OtDjSoCState *s = opaque;

    g_assert(irq == 0);

    if (level) {
        resettable_reset(OBJECT(s), RESET_TYPE_COLD);
    }
}

static void ot_dj_soc_reset_hold(Object *obj, ResetType type)
{
    OtDjSoCClass *c = RISCV_OT_DJ_SOC_GET_CLASS(obj);
    OtDjSoCState *s = RISCV_OT_DJ_SOC(obj);

    if (c->parent_phases.hold) {
        c->parent_phases.hold(obj, type);
    }

    /*
     * This function is called after all children have been reset_hold,
     * before any child has been reset_exit.
     *
     * Power-On-Reset: leave hart disabled on reset
     * PowerManager takes care of managing Ibex reset when ready
     */
    CPUState *cs = CPU(s->devices[OT_DJ_SOC_DEV_HART]);
    cs->disabled = true;
}

static void ot_dj_soc_reset_exit(Object *obj, ResetType type)
{
    OtDjSoCClass *c = RISCV_OT_DJ_SOC_GET_CLASS(obj);
    OtDjSoCState *s = RISCV_OT_DJ_SOC(obj);

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    /* Kick off ROM checks and boot */
    object_property_set_bool(OBJECT(s->devices[OT_DJ_SOC_DEV_ROM_CTRL0]),
                             "load", true, &error_fatal);
    object_property_set_bool(OBJECT(s->devices[OT_DJ_SOC_DEV_ROM_CTRL1]),
                             "load", true, &error_fatal);
}

static void ot_dj_soc_realize(DeviceState *dev, Error **errp)
{
    OtDjSoCState *s = RISCV_OT_DJ_SOC(dev);
    (void)errp;

    CPUState *cpu = CPU(s->devices[OT_DJ_SOC_DEV_HART]);
    cpu->memory = get_system_memory();
    cpu->cpu_index = 0;

    /* Create the private bus on which all OpenTitan IPs are connected */
    s->ot_bus = BUS(object_new(TYPE_SYSTEM_BUS));
    qbus_init(s->ot_bus, sizeof(*s->ot_bus), TYPE_SYSTEM_BUS, DEVICE(s),
              "ot.bus");

    /* Link, define properties and realize devices, then connect GPIOs */
    ot_common_configure_devices_with_id(s->devices, s->ot_bus, "soc", false,
                                        ot_dj_soc_devices,
                                        ARRAY_SIZE(ot_dj_soc_devices));

    Object *oas;

    oas = object_property_get_link(OBJECT(s)->parent, "ctn.as", errp);
    g_assert(oas);
    AddressSpace *ctn_as = ot_address_space_get(OT_ADDRESS_SPACE(oas));

    MemoryRegion *dbg_mr = g_new0(MemoryRegion, 1u);
    memory_region_init(dbg_mr, OBJECT(dev), "dbg.xbar", OT_DJ_DBG_XBAR_SIZE);

    MemoryRegion *mrs[IBEX_MEMMAP_REGIDX_COUNT] = {
        [OT_DJ_DEFAULT_MEMORY_REGION] = cpu->memory,
        [OT_DJ_CTN_MEMORY_REGION] = ctn_as->root,
        [OT_DJ_DEBUG_MEMORY_REGION] = dbg_mr,
    };
    ibex_map_devices_mask(s->devices, mrs, ot_dj_soc_devices,
                          ARRAY_SIZE(ot_dj_soc_devices),
                          IBEX_MEMMAP_MAKE_REG_MASK(
                              OT_DJ_DEFAULT_MEMORY_REGION) |
                              IBEX_MEMMAP_MAKE_REG_MASK(
                                  OT_DJ_DEBUG_MEMORY_REGION));

    MemoryRegion *mbx_sram_root_mr = g_new0(MemoryRegion, 1u);
    memory_region_init(mbx_sram_root_mr, OBJECT(s), "mbx.sram",
                       OT_DJ_PRIVATE_REGION_SIZE);
    AddressSpace *mbx_sram_as = g_new0(AddressSpace, 1u);
    address_space_init(mbx_sram_as, mbx_sram_root_mr, "mbx.sram.as");
    Object *mbx_sram_oas = object_new(TYPE_OT_ADDRESS_SPACE);
    object_property_add_child(OBJECT(dev), "ot-mbx.sram", mbx_sram_oas);
    ot_address_space_set(OT_ADDRESS_SPACE(mbx_sram_oas), mbx_sram_as);
    SysBusDevice *sram_dev =
        SYS_BUS_DEVICE(s->devices[OT_DJ_SOC_DEV_SRAM_CTRL_MBOX]);
    /* first MMIO maps to register, second MMIO maps to SRAM */
    MemoryRegion *mbx_sram_mr = sysbus_mmio_get_region(sram_dev, 1);
    MemoryRegion *mbx_sram_alias_mr = g_new0(MemoryRegion, 1u);
    memory_region_init_alias(mbx_sram_alias_mr, OBJECT(s), "mbx.sram",
                             mbx_sram_mr, 0u, int128_getlo(mbx_sram_mr->size));
    memory_region_add_subregion(mbx_sram_root_mr, mbx_sram_mr->addr,
                                mbx_sram_alias_mr);

    AddressSpace *dbg_as = g_new0(AddressSpace, 1u);
    address_space_init(dbg_as, dbg_mr, "dbg.as");

    oas = object_new(TYPE_OT_ADDRESS_SPACE);
    object_property_add_child(OBJECT(dev), "ot-dbg", oas);
    ot_address_space_set(OT_ADDRESS_SPACE(oas), dbg_as);


    oas = object_new(TYPE_OT_ADDRESS_SPACE);
    object_property_add_child(OBJECT(dev), "ot-dma", oas);
    ot_address_space_set(OT_ADDRESS_SPACE(oas), cpu->as);

    /*
     * create a new root region to map the CTN for the DMA, viewed as an
     * elevated region, which means the address range below the elevated CTN
     * range is kept empty
     */
    MemoryRegion *ctn_dma_mr = g_new0(MemoryRegion, 1u);
    memory_region_init(ctn_dma_mr, OBJECT(dev), "ctn.dma",
                       OT_DJ_CTN_REGION_OFFSET + OT_DJ_CTN_REGION_SIZE);

    /* create an AS view for this new root region */
    AddressSpace *ctn_dma_as = g_new0(AddressSpace, 1u);
    address_space_init(ctn_dma_as, ctn_dma_mr, "ctn.dma.as");

    /* create and map an alias to the CTN MR into the elevated region */
    MemoryRegion *ctn_amr = g_new0(MemoryRegion, 1u);
    memory_region_init_alias(ctn_amr, OBJECT(dev), "ctn.dma.alias",
                             ctn_as->root, 0u, (uint64_t)OT_DJ_CTN_REGION_SIZE);
    memory_region_add_subregion(ctn_dma_mr, (hwaddr)OT_DJ_CTN_REGION_OFFSET,
                                ctn_amr);

    oas = object_new(TYPE_OT_ADDRESS_SPACE);
    object_property_add_child(OBJECT(dev), "ctn-dma", oas);
    ot_address_space_set(OT_ADDRESS_SPACE(oas), ctn_dma_as);

    qdev_connect_gpio_out_named(DEVICE(s->devices[OT_DJ_SOC_DEV_RSTMGR]),
                                OT_RSTMGR_SOC_RST, 0,
                                qdev_get_gpio_in_named(DEVICE(s),
                                                       OT_DJ_SOC_RST_REQ, 0));

    ot_common_check_rom_configuration();

    /* load kernel if provided */
    ibex_load_kernel(cpu);
}

static void ot_dj_soc_init(Object *obj)
{
    OtDjSoCState *s = RISCV_OT_DJ_SOC(obj);

    s->devices = ibex_create_devices(ot_dj_soc_devices,
                                     ARRAY_SIZE(ot_dj_soc_devices), DEVICE(s));

    qdev_init_gpio_in_named(DEVICE(obj), &ot_dj_soc_hw_reset, OT_DJ_SOC_RST_REQ,
                            1);
}

static void ot_dj_soc_class_init(ObjectClass *oc, void *data)
{
    OtDjSoCClass *sc = RISCV_OT_DJ_SOC_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(dc);
    (void)data;

    resettable_class_set_parent_phases(rc, NULL, &ot_dj_soc_reset_hold,
                                       &ot_dj_soc_reset_exit,
                                       &sc->parent_phases);
    rc->child_foreach = &ot_dj_soc_reset_child_foreach;
    dc->realize = &ot_dj_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo ot_dj_soc_type_info = {
    .name = TYPE_RISCV_OT_DJ_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtDjSoCState),
    .instance_init = &ot_dj_soc_init,
    .class_init = &ot_dj_soc_class_init,
    .class_size = sizeof(OtDjSoCClass),
};

static void ot_dj_soc_register_types(void)
{
    type_register_static(&ot_dj_soc_type_info);
}

type_init(ot_dj_soc_register_types);

/* ------------------------------------------------------------------------ */
/* Board */
/* ------------------------------------------------------------------------ */

static void ot_dj_board_child_foreach(Object *obj, ResettableChildCallback cb,
                                      void *opaque, ResetType type)
{
    OtDjBoardState *s = RISCV_OT_DJ_BOARD(obj);

    for (unsigned ix = 0; ix < OT_DJ_BOARD_DEV_COUNT; ix++) {
        cb(OBJECT(s->devices[ix]), opaque, type);
    }
}

static void ot_dj_board_realize(DeviceState *dev, Error **errp)
{
    OtDjBoardState *board = RISCV_OT_DJ_BOARD(dev);

    DeviceState *soc = board->devices[OT_DJ_BOARD_DEV_SOC];
    object_property_add_child(OBJECT(board), "soc", OBJECT(soc));

    /* CTN memory region */
    MemoryRegion *ctn_mr = g_new0(MemoryRegion, 1u);
    memory_region_init(ctn_mr, OBJECT(dev), "ctn.xbar",
                       (uint64_t)OT_DJ_CTN_REGION_SIZE);

    /* CTN address space */
    AddressSpace *ctn_as = g_new0(AddressSpace, 1);
    address_space_init(ctn_as, ctn_mr, "ctn.as");
    Object *oas = object_new(TYPE_OT_ADDRESS_SPACE);
    object_property_add_child(OBJECT(dev), ctn_as->name, oas);
    ot_address_space_set(OT_ADDRESS_SPACE(oas), ctn_as);

    OtDjSoCState *s = RISCV_OT_DJ_SOC(soc);

    BusState *bus = sysbus_get_default();
    qdev_realize_and_unref(soc, bus, &error_fatal);

    /* CTN RAM */
    MemoryRegion *ctn_ram = g_new0(MemoryRegion, 1u);
    memory_region_init_ram_nomigrate(ctn_ram, OBJECT(s), "ctn.ram",
                                     OT_DJ_CTN_RAM_SIZE, errp);
    memory_region_add_subregion(ctn_mr, OT_DJ_CTN_RAM_ADDR, ctn_ram);

    /* CTN aliased memory in CPU address space */
    MemoryRegion *ctn_alias_mr = g_new0(MemoryRegion, 1u);
    memory_region_init_alias(ctn_alias_mr, OBJECT(dev), "ctn.alias", ctn_mr, 0u,
                             (uint64_t)OT_DJ_CTN_REGION_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                (hwaddr)OT_DJ_CTN_REGION_OFFSET, ctn_alias_mr);

    DeviceState *spihost = s->devices[OT_DJ_SOC_DEV_SPI_HOST0];
    DeviceState *flash = board->devices[OT_DJ_BOARD_DEV_FLASH];
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

    DeviceState *devproxy = board->devices[OT_DJ_BOARD_DEV_DEV_PROXY];
    object_property_add_child(OBJECT(board), "devproxy", OBJECT(devproxy));
    qdev_realize_and_unref(devproxy, NULL, errp);
}

static void ot_dj_board_init(Object *obj)
{
    OtDjBoardState *s = RISCV_OT_DJ_BOARD(obj);

    s->devices = g_new0(DeviceState *, OT_DJ_BOARD_DEV_COUNT);
    s->devices[OT_DJ_BOARD_DEV_SOC] = qdev_new(TYPE_RISCV_OT_DJ_SOC);
    s->devices[OT_DJ_BOARD_DEV_FLASH] = qdev_new("is25wp128");
    s->devices[OT_DJ_BOARD_DEV_DEV_PROXY] = qdev_new(TYPE_OT_DEV_PROXY);
}

static void ot_dj_board_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    (void)data;

    dc->realize = &ot_dj_board_realize;

    ResettableClass *rc = RESETTABLE_CLASS(oc);
    rc->child_foreach = &ot_dj_board_child_foreach;
}

static const TypeInfo ot_dj_board_type_info = {
    .name = TYPE_RISCV_OT_DJ_BOARD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OtDjBoardState),
    .instance_init = &ot_dj_board_init,
    .class_init = &ot_dj_board_class_init,
};

static void ot_dj_board_register_types(void)
{
    type_register_static(&ot_dj_board_type_info);
}

type_init(ot_dj_board_register_types);

/* ------------------------------------------------------------------------ */
/* Machine */
/* ------------------------------------------------------------------------ */

static bool ot_dj_machine_get_no_epmp_cfg(Object *obj, Error **errp)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(obj);
    (void)errp;

    return s->no_epmp_cfg;
}

static void ot_dj_machine_set_no_epmp_cfg(Object *obj, bool value, Error **errp)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(obj);
    (void)errp;

    s->no_epmp_cfg = value;
}

static bool ot_dj_machine_get_ignore_elf_entry(Object *obj, Error **errp)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(obj);
    (void)errp;

    return s->ignore_elf_entry;
}

static void
ot_dj_machine_set_ignore_elf_entry(Object *obj, bool value, Error **errp)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(obj);
    (void)errp;

    s->ignore_elf_entry = value;
}

static ResettableState *ot_dj_machine_get_reset_state(Object *obj)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(obj);

    return &s->reset;
}

static void ot_dj_machine_child_foreach(Object *obj, ResettableChildCallback cb,
                                        void *opaque, ResetType type)
{
    Object *board = object_property_get_link(obj, "board", &error_fatal);

    cb(board, opaque, type);
}

static void ot_dj_machine_reset(MachineState *ms, ResetType reason)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(ms);

    g_assert(reason == RESET_TYPE_COLD);

    resettable_reset(OBJECT(s), reason);
}

static void ot_dj_machine_instance_init(Object *obj)
{
    OtDjMachineState *s = RISCV_OT_DJ_MACHINE(obj);

    s->no_epmp_cfg = false;
    object_property_add_bool(obj, "no-epmp-cfg", &ot_dj_machine_get_no_epmp_cfg,
                             &ot_dj_machine_set_no_epmp_cfg);
    object_property_set_description(obj, "no-epmp-cfg",
                                    "Skip default ePMP configuration");
    object_property_add_bool(obj, "ignore-elf-entry",
                             &ot_dj_machine_get_ignore_elf_entry,
                             &ot_dj_machine_set_ignore_elf_entry);
    object_property_set_description(obj, "ignore-elf-entry",
                                    "Do not set vCPU PC with ELF entry point");
}

static void ot_dj_machine_init(MachineState *state)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_OT_DJ_BOARD);

    object_property_add_child(OBJECT(state), "board", OBJECT(dev));

    qdev_realize(dev, NULL, &error_fatal);
}

static void ot_dj_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    (void)data;

    mc->desc = "RISC-V Board compatible with OpenTitan Darjeeling platform";
    mc->init = &ot_dj_machine_init;
    mc->reset = &ot_dj_machine_reset;
    mc->max_cpus = 1u;
    mc->default_cpus = 1u;

    /*
     * Implement the resettable interface to ensure the proper initialization
     * sequence.
     */
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    rc->get_state = &ot_dj_machine_get_reset_state;
    rc->child_foreach = &ot_dj_machine_child_foreach;
}

static const TypeInfo ot_dj_machine_type_info = {
    .name = TYPE_RISCV_OT_DJ_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OtDjMachineState),
    .instance_init = &ot_dj_machine_instance_init,
    .class_init = &ot_dj_machine_class_init,
    .interfaces = (InterfaceInfo[]){ { TYPE_RESETTABLE_INTERFACE }, {} },
};

static void ot_dj_machine_register_types(void)
{
    type_register_static(&ot_dj_machine_type_info);
}

type_init(ot_dj_machine_register_types);
