# Darjeeling CW310

## Supported version

Please check out `hw/opentitan/ot_ref.log`

## Supported features

* ePMP
* Zbr ISA extension (crc32 instructions)

## Supported devices

### Near feature-complete devices

* AES
  * missing side-loading
* Alert controller
  * ping mechanism is not supported
* AON Timer
* CSRNG
* EDN
* HMAC
* JTAG (compatible with OpenOCD/Spike "remote bitbang" protocol)
* Mailbox
   * [JTAG mailbox](jtagmbx.md) can be accessed through JTAG using a DM-TL bridge
* OTBN
  * missing side-loading
* OTP controller
  * read and write features are supported, Present scrambling is supported w/ digest checks,
    ECC (detection and correction) is supported.
* [RISC-V Debug Module](jtag-dm.md) and Pulp Debug Module
* [ROM controller](rom_ctrl.md)
* SoC Debug controller
* SPI data flash (from QEMU upstream w/ fixes)
* SPI host controller
  * HW bus config is ignored (SPI mode, speed, ...)
* Timer
* UART
  * missing RX timeout, break support
  * bitrate is not paced vs. selected baurate

### Partially implemented devices

Devices in this group implement subset(s) of the real HW.

* DMA
  * Only memory-to-memory transfers (inc. hashing) are supported, Handshake modes are not supported
* Flash controller
  * read-only features only
* Entropy Src
   * test/health features are not supported
* [GPIO](gpio.md)
   * A CharDev backend can be used to get GPIO outputs and update GPIO inputs,
* KMAC
  * Side loading is not supported
* Lifecycle controller
  * [LC controller](lc_ctrl_dmi.md) can be accessed through JTAG using a DM-TL bridge
  * Escalation is not supported
* Power Manager
  * Fast FSM is partially supported, Slow FSM is bypassed
  * Interactions with other devices (such as the Reset Manager) are limited
* SPI device controller (only Flash mode is supported)
* SRAM controller
  * Initialization and scrambling from OTP key supported
  * Wait for init completion (bus stall) emulated
* SoC Proxy only supports IRQ routing/gating

### Sparsely implemented devices

In this group, device CSRs are supported (w/ partial or full access control & masking) but only some
features are implemented.

* AST
  * entropy source only (from host source)
* Clock Manager
  * Clock hints only
* Ibex wrapper
  * random source (connected to CSR), FPGA version, virtual remapper, fetch enable can be controlled
    from Power Manager
* Reset Manager
  * HW and SW reset requests are supported
* Pinmux
  * Basic features (pull up/down, open drain) are supported for GPIO pin only

### Dummy devices

Devices in this group are mostly implemented with a RAM backend or real CSRs but do not implement
any useful feature (only allow guest test code to execute as expected).

* Key manager
* Sensor

### Additional devices

* [DevProxy](devproxy.md) is a CharDev-enabled component that can be remotely controlled to enable
  communication with the system-side buses of the mailboxes and DMA devices. A Python library is
  available as `scripts/opentitan/devproxy.py` and provide an API to remote drive the devproxy
  communication interface.

## Running the virtual machine

### Arbitrary application

````sh
qemu-system-riscv32 -M ot-darjeeling,no_epmp_cfg=true -display none -serial mon:stdio \
  -global ot-ibex_wrapper-dj.lc-ignore=on -kernel hello.elf
````
See the section "Useful execution options" for documentation about the `no_epmp_cfg` and
`ot-ibex_wrapper-dj.lc-ignore=on` option.

### Boot sequence ROM, ROM_EXT, BLO

````sh
qemu-system-riscv32 -M ot-darjeeling -display none -serial mon:stdio \
  -object ot-rom_img,id=rom,file=rom_with_fake_keys_fpga_cw310.elf \
  -drive if=pflash,file=otp-rma.raw,format=raw \
  -drive if=mtd,bus=1,file=flash.raw,format=raw
````

where `otp-rma.raw` contains the RMA OTP image and `flash.raw` contains the signed binary file of the
ROM_EXT and the BL0. See [`otptool.py`](otptool.md) and [`flashgen.py`](flashgen.md) tools to
generate the `.raw` image files.

See [`rom_ctrl.md`](rom_ctrl.md) for information on ROM option.

## Tools

See [`tools.md`](tools.md)

## Useful execution options

### vCPU

* `-icount 6` reduces the execution speed of the vCPU (Ibex core) to 1GHz >> 6, _i.e._ ~15MHz,
  which should roughly match the expected speed of the Ibex core running on the CW310 FPGA, which
  is set to 10 MHz. This option is very useful/mandatory to run many OpenTitan tests that rely on
  time or CPU cycle to validate features. Using `-icount` option slows down execution speed though,
  so it is not recommended to use it when the main goal is to develop SW to run on the virtual
  machine. An alternative is to use `-icount shift=auto`, which offers fatest emulation execution,
  while preserving an accurate ratio between the vCPU clock and the virtual devices.

* `no_epmp_cfg=true` can be appended to the machine option switch, _i.e._
  `-M ot-darjeeeling,no_epmp_cfg=true` to disable the initial ePMP configuration, which can be very
  useful to execute arbitrary code on the Ibex core without requiring an OT ROM image to boot up.

* `ignore_elf_entry=true` can be appended to the machine option switch, _i.e._
  `-M ot-darjeeeling,ignore_elf_entry=true` to prevent the ELF entry point of a loaded application
  to update the vCPU reset vector at startup. When this option is used, with `-kernel` option for
  example, the application is loaded in memory but the default machine reset vector is used.

* `-global ot-ibex_wrapper-dj.lc-ignore=on` should be used whenever no OTP image is provided, or if
  the current LifeCycle state stored in the OTP image does not allow the Ibex core to fetch data.
  This switch forces the Ibex core to execute whatever the LifeCycle broadcasted signal, which
  departs from the HW behavior but maybe helpful to run the machine without a full OTP set up. The
  alternative to allow the Ibex core to execute guest code is to provide a valid OTP image with one
  of the expected LifeCycle state, such as TestUnlock*, Dev, Prod or RMA.

* `-global ot-ibex_wrapper-dj.lc-ignore-ids=<ids>` acts as `lc-ignore`, enabling the selection of
  specific ibex wrapper instance based on their unique identifiers. See `ot_id` property in the
  machine definition file for a list of valid identifiers. `<ids>` should be defined as a comma-
  separated list of valid identifiers. It is only possible to ignore LifeCycle states with this
  option, not to enforce them.

* `-cpu lowrisc-ibex,x-zbr=false` can be used to force disable the Zbr experimental-and-deprecated
  RISC-V bitmap extension for CRC32 extension.

### AES

* `-global ot-aes.fast-mode=false` can be used to better emulate AES HW IP, as some OT tests expect
  the Ibex core to execute while the HW is performing AES rounds. Without this option, the virtual
  HW may only give back execution to the vCPU once the AES operation is complete, which make those
  OT tests to fail. Disabling fast mode better emulates the HW to the expense of higher AES latency
  and throughput.

### Display

 * `-display none` can be used to prevent QEMU to open a semi-graphical windows as the default
   console, and use the current shell instead.

### Flash

* `-drive if=mtd,bus=1,file=<filename>,format=raw` should be used to specify a path to a QEMU RAW
  image file used as the OpenTitan internal flash controller image. This _RAW_ file should have
  been generated with the [`flashgen.py`](flashgen.md) tool.

  Note: for now, bus 1 is assigned to the internal controller with the embedded flash storage. See
  also SPI Host section.

### GPIO

  See [gpio](gpio.md) documentation for options.

### OTBN

* `-global ot-otbn.logfile=<filename>` dumps executed instructions on OTBN core into the specified
  filename. Beware that is even further slows down execution speed, which could likely result into
  guest application on the Ibex core to time out.

### OTP

* `-drive if=pflash,file=otp.raw,format=raw` should be used to specify a path to a QEMU RAW image
  file used as the OpenTitan OTP image. This _RAW_ file should have been generated with the
  [`otptool.py`](otptool.md) tool.

### SoC Debug controller

SoC debug controller manages SoC debug policies based on external signals - such as GPIO, Power
Manager states and LifeCycle states, the later being defined from the OTP content. If no OTP image
is provided, or a RAW (blank) image is provided, or if the OTP image defines a LifeCycle in any of
TEST* or RMA states, a Darjeeling machine that features a SoC Debug controller may enter the DFT
("Debug For Test") execution mode, where the Ibex core may not resume execution till a JTAG debugger
triggers it.

To force QEMU to execute an application despite this feature, bypassing the DFT mode, use
`-global ot-socdbg_ctrl.dft-ignore=on` QEMU option.

### SPI Device

* See [SPI device](spi_devide.md) for options.

### SPI Host

* `-drive if=mtd,bus=0,file=<filename>,format=raw` should be used to specify a path to a QEMU RAW
  image file used as the ISSP IS25WP128 SPI data flash backend file. This _RAW_ file should have
  been created with the qemu-img tool. There is no dedicated tool to populate this image file for
  now.

  ````sh
  qemu-img create -f raw spi.raw 16M
  ````

  For now, bus 0 is assigned to the SPI Host controller with an external flash storage. See also
  Flash controller section.

### Reset Manager

It is possible to limit the number of times the VM reboots the guest. This option may be useful
during the development process when an issue in the early FW stages - such as the ROM - causes an
endless reboot cycles of the guest.

To limit the reboot cyckes, use the `-global ot-rstmgr.fatal_reset=<N>` option, where `N` is an
unsigned integer. This option forces the QEMU VM to exit the N^th^ time the reset manager receives
a reset request, rather than rebooting the whole machine endlessly as the default behavior.

### UART

* `-serial mon:stdio`, used as the first `-serial` option, redirects the virtual UART0 to the
  current console/shell.

* `-chardev socket,id=serial1,host=localhost,port=8001,server=on,wait=off` and
  `-serial chardev:serial1` can be used to redirect UART1 (in this example) to a TCP socket. These
  options are not specific to OpenTitan emulation, but are useful to communicate over a UART.
  Note that QEMU offers many `chardev` backends, please check QEMU documentation for details.

## Useful debugging options

### Device log traces

Most OpenTitan virtual devices can emit log traces. To select which traces should be logged, a plain
text file can be used along with QEMU `-trace` option.

To populate this file, the easiest way is to dump all available traces and filter them with a
pattern, for example to get all OpenTitan trace messages:

````sh
qemu-system-riscv32 -trace help | grep -E '^ot_' > ot_trace.log
qemu-system-riscv32 -trace events=ot_trace.log -D qemu.log ...
````

* It is *highly* recommended to use the `-D` option switch when any `-trace` or `-d` (see below) is
selected, to avoid saturating the standard output stream with traces and redirect them into the
specified log file.

### QEMU log traces

QEMU provides another way of logging execution of the virtual machine using the `-d` option. Those
log messages are not tied to a specific device but rather to QEMU features. `-d help` can be used
to enumerate these log features, however the most useful ones are enumerated here:

   * `unimp` reports log messages for unimplemented features, _e.g._ when the vCPU attempts to
     read from or write into a memory mapped device that has not been implemented.
   * `guest_errors` reports log messages of invalid guest software requests, _e.g._ attempts to
     perform an invalid configuration.
   * `int` reports all interruptions *and* exceptions handled by the vCPU. It may be quite verbose
     but also very useful to track down an invalid memory or I/O access for example. This is the
     first option to use if the virtual machine seems to stall on start up.
   * `in_asm` reports the decoded vCPU instructions that are translated by the QEMU TCG, _i.e._ here
     the RISC-V instructions. Note that transcoded instructions are cached and handled by blocks,
     so the flow of transcoded instruction do not exactly reflect the stream of the executed guest
     instruction, e.g. may only appear once in a loop. Use the next log option, `exec`, to get
     more detailed but also much more verbose log traces.
   * `exec` reports the vCPU execution stream.

Those options should be combined with a comma separator, _e.g._ `-d unimp,guest_errors,int`

`in_asm` option may be able to report the name of the guest executed function, as long as the guest
application symbols have been loaded. This is the case when the `-kernel` option is used to load
an ELF non-stripped file. Unfortunately, this feature is not available for guest applications that
are loaded from a raw binary file (`.bin`, `.signed.bin`, ...). However the
[`flashgen.py`](flashgen.md) script implements a workaround for this feature, please refer to this
script for more details.

Finally, a Rust demangler has been added to QEMU, which enables the QEMU integrated dissambler to
emit the demangled names of the Rust symbols for Rust-written guest applications rather than their
mangled versions as stored in the ELF file.
