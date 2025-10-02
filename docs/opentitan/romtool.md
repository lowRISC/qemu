# `romtool.py`

`romtool.py` converts ROM image between different file formats.

Supported input formats:
* `ELF`: RISC-V RV32 executable file (only supported as input ROM file)
* `BIN`: RISC-V RV32 executable file
* `VMEM`: Plain VMEM text file
* `SVMEM`: Scrambled VMEM text file with 7-bit SEC-DED
* `HEX`: Scrambled HEX text file with 7-bit SEC-DED

## Usage

````text
usage: romtool.py [-h] -c CFG [-o OUTPUT] [-i ROM_ID] [-z SIZE]
                  [-f {HEX,BIN,SVMEM,VMEM}] [-s] [-v] [-d]
                  rom

QEMU OT tool to generate a scrambled ROM image.

options:
  -h, --help            show this help message and exit

Files:
  rom                   input ROM image file
  -c, --config CFG      input QEMU OT config file
  -o, --output OUTPUT   output ROM image file

Parameters:
  -i, --rom-id ROM_ID   ROM image identifier
  -z, --rom-size SIZE   ROM image size in bytes (accepts Ki suffix)
  -f, --output-format {HEX,BIN,SVMEM,VMEM}
                        Output file format (default: HEX)
  -s, --subst-perm      Enable legacy mode with S&P mode

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

### Arguments

* `-c` specify a QEMU [configuration file](otcfg.md) from which to read all the cryptographic
  constants. See [`cfggen.py`](cfggen.md) tool to generate such a file.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-f` output file format. `HEX` format always output scrambled/SEC-DED data, `SVMEM` specifies a
  VMEM format with scrambled/SEC-DED data. Note: input file format is automatically detected from
  the content of the input ROM file.

* `-i` ROM identifier. Required for platforms with multiple ROMs

* `-o` outfile file, default to stdout (beware when using a binary format)

* `-s` use legacy scrambling scheme, with extra substitute and permute stages, but less PRINCE
  stages. Only useful for older files.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

* `-z` ROM file size. Required for all input file formats but the `HEX` or `SVMEM` format.
  `Ki` (kilobytes) suffix is supported.

### Examples

Generate a scrambled with SEC-DED data in HEX format:
````sh
# EarlGrey
scripts/opentitan/romtool.py -c ot.cfg -z 32Ki -o rom.hex rom.elf
# Darjeeling (base ROM)
scripts/opentitan/romtool.py -c ot.cfg -z 32Ki -i 0 -o rom0.hex rom0.elf
````

Extract clear data from a scrambled HEX file:
````sh
# EarlGrey
scripts/opentitan/romtool.py -c ot.cfg -f VMEM -o rom.vmem rom.hex
# Darjeeling (base ROM)
scripts/opentitan/romtool.py -c ot.cfg -i 0 -f VMEM -o rom.vmem rom.hex
```
