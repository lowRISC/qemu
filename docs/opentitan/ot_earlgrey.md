# EarlGrey

## Supported version

[Earlgrey 1.0.0](https://github.com/lowRISC/opentitan/tree/earlgrey_1.0.0)

## Supported devices

### Near feature-complete devices

* [AES](ot_aes.md)
* Alert controller
  * ping mechanism is not supported
* AON Timer
* CSRNG
* EDN
* [Flash controller](ot_flash.md)
  * largely functional but without ECCs/ICVs, scrambling functionality & alerts
  * no modelling of erase suspend or RMA entry
  * lc_ctrl NVM debug signal not implemented, escalation partially implemented
* HMAC
* [IBEX CPU](ibex_cpu.md)
* [Key manager](ot_keymgr.md)
  * Almost feature complete
  * Missing entropy reseeding, and support for KMAC masking (when available)
* [OTBN](ot_otbn.md)
* [OTP controller](ot_otp.md)
  * read and write features are supported,
  * Present scrambling is supported with digest checks,
  * ECC (detection and correction) is supported
* [ROM controller](ot_rom_ctrl.md)
* SPI data flash (from QEMU upstream w/ fixes)
* [SPI Host controller](ot_spi_host.md)
  * HW bus config is ignored (SPI mode, speed, ...)
* [SPI Device](ot_spi_device.md)
* Timer
* [UART](ot_uart.md)
  * missing RX timeout, TX break not supported
  * bitrate is not paced vs. selected baurate

### Partially implemented devices

Devices in this group implement subset(s) of the real HW.

* Clock Manager
  * Runtime-configurable device, through properties
  * Manage clock dividers, groups, hints, software configurable clocks
  * Propagate clock signals from source (AST, ...) to devices
  * Hint management and measurement are not implemented
* Entropy Src
   * test/health features are not supported
* [I2C controller](ot_i2c.md)
  * Supports only one target mode address - ADDRESS1 and MASK1 are not implemented
  * Timing features are not implemented
  * Loopback mode is not implemented
* [Ibex wrapper](ot_ibex_wrapper.md)
  * random source (connected to CSR), FPGA version, virtual remapper, fetch enable can be controlled
    from Power Manager
* KMAC
  * Masking is not supported
* Lifecycle controller
  * [LC controller](lc_ctrl_dmi.md) can be accessed through JTAG using a DM-TL bridge
* [ROM controller](ot_rom_ctrl.md)
* SRAM controller
  * Initialization and scrambling from OTP key supported
  * Wait for init completion (bus stall) emulated
* [USB Device](ot_usbdev.md)

### Sparsely implemented devices

In this group, device CSRs are supported (w/ partial or full access control & masking) but only some
features are implemented.

* Analog Sensor Top
  * noise source only (from host source)
  * configurable clock sources
* [GPIO](ot_gpio.md)
  * Connections with pinmux not implemented (need to be ported from [Darjeeling](ot_darjeeling.md)
    version)
* Power Manager
  * Fast FSM is partially supported, Slow FSM is bypassed
  * Interactions with other devices (such as the Reset Manager) are limited
* [Reset Manager](ot_rstmgr.md)
  * HW and SW reset requests are supported

### Dummy devices

Devices in this group are mostly implemented with a RAM backend or real CSRs but do not implement
any useful feature (only allow guest test code to execute as expected).
Some just use generic `UNIMP` devices to define a memory region.

* Pattern Generator
* Pinmux
* PWM
* Sensor Control
* System Reset Controller

## Running the virtual machine

See [OpenTitan machine](ot_machine.md) documentation for options.

### Arbitrary application

````sh
qemu-system-riscv32 -M ot-earlgrey,no_epmp_cfg=true -display none -serial mon:stdio \
  -readconfig docs/config/opentitan/earlgrey.cfg \
  -global ot-ibex_wrapper.lc-ignore=on  -kernel hello.elf
````

### Boot sequence ROM, ROM_EXT, BLO

````sh
qemu-system-riscv32 -M ot-earlgrey -display none -serial mon:stdio \
  -readconfig docs/config/opentitan/earlgrey.cfg \
  -object ot-rom_img,id=rom,file=rom_with_fake_keys_fpga_cw310.elf \
  -drive if=pflash,file=otp-rma.raw,format=raw \
  -drive if=mtd,bus=2,file=flash.raw,format=raw
````

where `otp-rma.raw` contains the RMA OTP image and `flash.raw` contains the signed binary file of
the ROM_EXT and the BL0. See [`otptool.py`](otptool.md) and [`flashgen.py`](flashgen.md) tools to
generate the `.raw` image files.

## Buses

EarlGrey emulation supports the following buses:

| **Type** | **Num** | **Usage**                                                    |
| -------- | ------- | -------------------------------------------------------------|
| `mtd`    |    0    | [SPI host 0](ot_spi_host.md), [SPI device](ot_spi_device.md) |
| `mtd`    |    1    | [SPI host 1](ot_spi_host.md)                                 |
| `mtd`    |    2    | [Embedded Flash](ot_flash.md)                                |
| `pflash` |    0    | [OTP](ot_otp.md)                                             |

## Tools

See [`tools.md`](tools.md)

## Useful debugging options

See [debug option](debug.md) for details.
