# `flashgen.py`

`flashgen.py` populates a QEMU RAW image file which can be loaded by the OpenTitan integrated
flash controller virtual device.

## Usage

````text
usage: flashgen.py [-h] [-D] [-a {0,1}] [-s OFFSET] [-x file] [-X elf] [-b file] [-B elf]
                   [-t OTDESC] [-v] [-d] [-U] flash

Create/update an OpenTitan backend flash file.

options:
  -h, --help            show this help message and exit

Image:
  flash                 virtual flash file
  -D, --discard         Discard any previous flash file content
  -a {0,1}, --bank {0,1}
                        flash bank for data (default: 0)
  -s OFFSET, --offset OFFSET
                        offset of the BL0 file (default: 0x10000)

Files:
  -x file, --exec file  rom extension or application
  -X elf, --exec-elf elf
                        ELF file for rom extension or application, for symbol tracking (default: auto)
  -b file, --boot file  bootloader 0 file
  -B elf, --boot-elf elf
                        ELF file for bootloader, for symbol tracking (default: auto)
  -t OTDESC, --otdesc OTDESC
                        OpenTitan style file descriptor, may be repeated

Extra:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
  -U, --unsafe-elf      Discard sanity checking on ELF files
````

The (signed) binary files contain no symbols, which can make low-level debugging in QEMU difficult
when using the `-d in_asm` and/or `-d exec` option switches.

If an ELF file with the same radix as a binary file is located in the directory of the binary
file, or its location is specified in the command line, the path to the ELF file is encoded
into the flash image within a dedicated debug section.

The OT flash controller emulation, when the stored ELF file exists, attempts to load the symbols
from this ELF file into QEMU disassembler. This enables the QEMU disassembler - used with the
`in_asm`/`exec` QEMU options - to output the name of each executed function as an addition to the
guest PC value.

It is therefore recommended to ensure the ELF files are located in the same directory as their
matching signed binary files to help with debugging.

### Arguments

* `-a bank` specify the data partition to store the binary file into, mutually exclusive with `-t`.

* `-B elf` specify an alternative path to the BL0 ELF file. If not specified, the ELF path file is
  reconstructed from the specified binary file (from the same directory). The ELF file is only used
  as a hint for QEMU loader. Requires option `-b`, mutually exclusive with `-t`.

* `-b file` specify the BL0 (signed) binary file to store in the data partition of the flash
  image file. The Boot Loader 0 binary file is stored in the data partition at the offset
  specified with the -s option, mutually exclusive with `-t`.

* `-D discard` discard any flash content that may exist in the QEMU RAW image file.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-s offset` the offset of the BootLoader image file in the data partition. Note that this offset
  may also be hardcoded in the ROM_EXT application. Changing the default offset may cause the
  ROM_EXT to fail to load the BL0 application.

* `-t binfile@address[:elfile]` specify a binary file to store into the data partition of the flash.
  This is a compatibility option introduced to support the original `opentitantool image assemble`
  syntax. In this mode, the file kind and the destination flash bank are guessed from the specified
  address. The address should be specified in hexadecimal format. It is possible to specify an
  matching ELF file by appending its path after a column separator following the address value. This
  option may be repeated to specify multiple files such as the ROM EXT and a bootloader for example.
  This option is mutually exclusive with `-b`, `-B`, `-x`, `-X` and `-a`.

* `-X elf` specify an alternative path to the ROM_EXT ELF file. If not specified, the ELF path file
  is reconstructed from the specified binary file (from the same directory). The ELF file is only
  used as a hint for QEMU loader. Requires option `-x`, mutually exclusive with `-t`.

* `-x file` specify the ROM_EXT (signed) binary file or the application to store into the data
  partition of the flash image file. Alternatively this file can be any (signed) binary file such as
  an OpenTitan test application. Note that if a BL0 binary file is used, the ROM_EXT/test binary
  file should be smaller than the selected offset for the bootloader location, mutually exclusive
  with `-t`.

* `-U` tell the script to ignore any discrepancies found between a binary file and an ELF file. If
  the path to an ELF file is stored in the flash image file, the ELF content is validated against
  the binary file. Its code position and size, overall size and entry point should be coherent.
  Moreover, the modification time of the binary file should not be younger than the ELF origin file.
  Note that contents of both files are not compared, as the binary file may be amended after the ELF
  file creation.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.
