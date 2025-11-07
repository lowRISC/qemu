# Darjeeling

## Supported version

Please check out `hw/opentitan/ot_ref.log`

## Supported devices

### Near feature-complete devices

* [AES](ot_aes.md)
* Alert controller
  * ping mechanism is not supported
* AON Timer
* CSRNG
* EDN
* HMAC
* [IBEX CPU](ibex_cpu.md)
* [JTAG](jtag-dm.md) (compatible with OpenOCD/Spike "remote bitbang" protocol)
* Key manager DPE
  * Almost feature complete
  * Missing entropy reseeding, and support for KMAC masking (when available)
* Mailbox
   * [JTAG mailbox](jtagmbx.md) can be accessed through JTAG using a DM-TL bridge
* [OTBN](ot_otbn.md)
* [OTP controller](ot_otp.md)
  * read and write features are supported,
  * Present scrambling is supported with digest checks,
  * ECC (detection and correction) is supported
  * zero-ization is not yet supported
* [RISC-V Debug Module](jtag-dm.md) and Pulp Debug Module
* [ROM controller](ot_rom_ctrl.md)
* [SoC debug controller documentation](ot_soc_dbg_ctrl.md)
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
* DMA
  * Only memory-to-memory transfers (inc. hashing) are supported, Handshake modes are not supported
* Entropy Src
   * test/health features are not supported
* [GPIO](ot_gpio.md)
   * A CharDev backend can be used to get GPIO outputs and update GPIO inputs
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
  * Escalation is not supported
* [ROM controller](ot_rom_ctrl.md)
* SRAM controller
  * Initialization and scrambling from OTP key supported
  * Wait for init completion (bus stall) emulated
* SoC Proxy only supports IRQ routing/gating

### Sparsely implemented devices

In this group, device CSRs are supported (w/ partial or full access control & masking) but only some
features are implemented.

* Analog Sensor Top
  * noise source only (from host source)
  * configurable clock sources
* Pinmux
  * Basic features (pull up/down, open drain) are supported for GPIO pin only
* Power Manager
  * Fast FSM is partially supported, Slow FSM is bypassed
  * Interactions with other devices (such as the Reset Manager) are limited
* [Reset Manager](ot_rstmgr.md)
  * HW and SW reset requests are supported

### Dummy devices

Devices in this group are mostly implemented with a RAM backend or real CSRs but do not implement
any useful feature (only allow guest test code to execute as expected).

* Sensor

### Additional devices

* [DevProxy](devproxy.md) is a CharDev-enabled component that can be remotely controlled to enable
  communication with the system-side buses of the mailboxes and DMA devices. A Python library is
  available as `scripts/opentitan/devproxy.py` and provide an API to remote drive the devproxy
  communication interface.

## Running the virtual machine

See [OpenTitan machine](ot_machine.md) documentation for options.

### Arbitrary application

````sh
qemu-system-riscv32 -M ot-darjeeling,no_epmp_cfg=true -display none -serial mon:stdio \
  -readconfig docs/config/opentitan/darjeeling.cfg \
  -global ot-ibex_wrapper.lc-ignore=on -kernel hello.elf
````

### Boot sequence ROM, ROM_EXT, BLO

````sh
qemu-system-riscv32 -M ot-darjeeling -display none -serial mon:stdio \
  -readconfig docs/config/opentitan/darjeeling.cfg \
  -object ot-rom_img,id=rom,file=rom_with_fake_keys_fpga_cw310.elf \
  -drive if=pflash,file=otp-rma.raw,format=raw \
  -drive if=mtd,bus=0,file=flash.raw,format=raw
````

where `otp-rma.raw` contains the RMA OTP image and `flash.raw` contains the signed binary file of
the ROM_EXT and the BL0. See [`otptool.py`](otptool.md) and [`flashgen.py`](flashgen.md) tools to
generate the `.raw` image files.

## Buses

Darjeeling emulation supports the following buses:

| **Type** | **Num** | **Usage**                                                  |
| -------- | ------- | -----------------------------------------------------------|
| `mtd`    |    0    | [SPI host](ot_spi_host.md), [SPI device](ot_spi_device.md) |
| `pflash` |    0    | [OTP](ot_otp.md)                                           |

## Tools

See [`tools.md`](tools.md)

## Useful debugging options

See [debug option](debug.md) for details.
