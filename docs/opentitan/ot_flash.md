# OpenTitan embedded flash

* `-drive if=mtd,id=eflash,bus=<bus>,file=<filename>,format=raw` should be used to specify a path to
  a QEMU RAW image file used as the OpenTitan internal flash controller image. This _RAW_ file
  should have been generated with the [`flashgen.py`](flashgen.md) tool.

## Machine specific options

### EarlGrey Machine

MTD bus 2 is assigned to the internal controller with the embedded flash storage, _i.e._ the command
line option should be written `-drive if=mtd,id=eflash,bus=2,file=<filename>,format=raw`.

### Darjeeling Machine

[Darjeeling](ot_darjeeling.md) machine does not support an embedded flash device.

## Notes

For external SPI data flash support, see [SPI host](ot_spi_host.md) documentation.
