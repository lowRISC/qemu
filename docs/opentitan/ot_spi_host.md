# OpenTitan SPI Host

* `-drive if=mtd,bus=<bus>,file=<filename>,format=raw` should be used to specify a path to a QEMU RAW
  image file used as the SPI data flash backend file. This _RAW_ file should have been created with
  the qemu-img tool. There is no dedicated tool to populate this image file for now.

  ````sh
  qemu-img create -f raw spi.raw 16M
  ````

  See the machine section for supported `<bus>` values.

* `-global ot-spi_host.start-delay=<time>` may be used to change the default SPI Host FSM start
  delay. This delay is used to yield back the control to the vCPU before kicking of the execution
  of a new SPI Host command, so that the guest code gets a chance to check the statuses of the SPI
  Host right after pushing the command. Without this delay, the SPI Host state may change quickly
  and report statuses that might be different than the real HW, _e.g._ a command may already be
  completed when the guest code reads back the SPI Host status. Time should be specified in ns,
  and defaults to 20000 (20 µs).

* -`global ot-spi_host.completion-delay=<time>` may be forced to discard the experimental SPI Host
  clock pacing, which helps to achieve the requested bandwidth on the SPI Host bus, which is always
  caped with the performances of the QEMU host. Forcing a small value here can help achieving the
  best SPI Host transfer performances, but decreases accuracy of the SPI Host clock settings. Time
  should be specified in ns, and defaults to 0 that indicates automatic SPI bus clock management.

## Machine specific options

### EarlGrey Machine

MTD bus 0 is assigned to the SPI0 Host controller and MTD bus 1 is assigned to the SPI1 Host
controller. See also the [Embedded Flash controller](ot_flash.md) documentation.

* `-global ot-earlgrey-board.spiflash<bus>=<flash_type>` should be used to instantiate a SPI
  dataflash device of the specified type to the first device (/CS0) of the specified bus.
  Any SPI dataflash device supported by QEMU can be used. To list the supported devices, use
  `grep -F 'INFO("' hw/block/m25p80.c | cut -d'"' -f2`

### Darjeeling Machine

MTD bus 0 is assigned to the unique SPI0 Host controller.

The Darjeeling machine is configured with an ISSP IS25WP128 SPI data flash device. It is not
possible to select another SPI data flash device model from the command line.
