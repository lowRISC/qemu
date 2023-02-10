# `flashgen.py`

`flashgen.py` populates a QEMU RAW image file which can be loaded by the OpenTitan integrated
flash controller virtual device.

## Usage

````text
usage: flashgen.py [-h] [-D] [-x file] [-l file] [-b {0,1}] [-s OFFSET] [-v] [-d] flash

Create/update an OpenTitan backend flash file.

positional arguments:
  flash                 virtual flash file

options:
  -h, --help            show this help message and exit
  -D, --discard         Discard any previous flash file content
  -x file, --rom-ext file
                        rom extension file
  -l file, --bootloader file
                        bootloader 0 file
  -b {0,1}, --bank {0,1}
                        flash bank for data (default: 0)
  -s OFFSET, --offset OFFSET
                        offset of the BL0 file (default: 0x10000)
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

The (signed) binary files contain no symbols, which can make low-level debugging in QEMU difficult
when using the `-d in_asm` and/or `-d exec` option switches.

If an ELF file with the same radix as a binary file is located in the directory of the binary
file, the path to the ELF file is encoded into the flash image into a dedicated debug section.

The OT flash controller emulation, when the stored ELF file exists, attemps to load the symbols
from this ELF file into QEMU disassembler. This enables the QEMU disassembler - used with the
`in_asm`/`exec` QEMU options - to output the name of each executed function as an addition to the
guest PC value.

It is therefore recommended to ensure the ELF files are located in the same directory as their
matching signed binary files to help with debugging.

### Arguments

* `-b bank` specify the data partition to store the binary file into.

* `-D discard` discard any flash content that may exist in the QEMU RAW image file.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-l file` specify the BL0 (signed) binary file to store in the data partition of the flash
  image file. The Boot Loader 0 binary file is stored in the data partition at the offset
  specified with the -s option.

* `-s offset` the offset of the BootLoader image file in the data partition. Note that this offset
  may also be hardcoded in the ROM_EXT application. Changing the default offset may cause the 
  ROM_EXT to fail to load the BL0 application.

* `-x file` specify the ROM_EXT (signed) binary file to store in the data partition of the flash
  image file. Alternatively this file can be any (signed) binary file such as an OpenTitan test
  application. Note that if a BL0 binary file is used, the ROM_EXT/test binary file should be
  smaller than the selected offset for the bootloader location.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.
