# Python modules for OpenTitan machines

The communication ports of the OpenTitan machines exposed through CharDev devices can be used
to access the devices from the local host (or also from a remote host when the CharDev is created
using a TCP socket).

* `python/qemu/jtag`: JTAG / TAP controller client, using the _Remote BitBang Protocol_
* `python/qemu/ot`: OpenTitan tools
  * `devproxy`: implementation of the communication channel with the QEMU devproxy device.
  * `dm`: RISC-V Debug Module support,
  * `dtm`: Debug Transport Module support,
  * `eflash`: Embedded Flash support,
  * `gpio`: GPIO support,
  * `km`: Key Manager support,
  * `lc_ctrl`: [Life Cycle controller](lc_ctrl_dmi.md) over JTAG/DMI support,
  * `mailbox`: support for accessing the responder and the requester sides of the DOE mailbox. Also
    support the [JTAG mailbox](jtagmbx.md) for accessing the mailbox from a JTAG/DMI link,
  * `otp`: support for parsing and verifing OTP VMEM images, as well as generating and decoding QEMU
    RAW image files,
  * `pyot`: implements the Python OpenTitan test orchestrator tool,
  * `rom`: support for parsing and verifying ROM images,
  * `socdbg`: support for communication with the SoC debug module of DMI,
  * `spi`: support SPI device communication, _i.e._ acts as a SPI master connected to QEMU SPI
     device port,
  * `util`: miscellaneous utililies such as ELF format tools and logging utilities,
     scrambler/descrambler used in OTP image files for HW digest verification.

Please check the [Python tools](tools.md) documentation for details and scripts that rely
on these APIs.
