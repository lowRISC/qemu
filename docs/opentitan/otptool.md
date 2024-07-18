# `otptool.py`

`otptool.py` is a tool to deal with QEMU OT RAW image files, which are managed by the OpenTitan OTP
controller virtual device.

## Usage

````text
usage: otptool.py [-h] [-j HJSON] [-m VMEM] [-l SV] [-o C] [-r RAW]
                  [-k {auto,otp,fuz}] [-e BITS] [-c INT] [-i INT] [-w] [-n]
                  [-s] [-E] [-D] [-U] [--clear-bit CLEAR_BIT]
                  [--set-bit SET_BIT] [--toggle-bit TOGGLE_BIT] [-L | -P | -R]
                  [-v] [-d]

QEMU OT tool to manage OTP files.

options:
  -h, --help            show this help message and exit

Files:
  -j HJSON, --otp-map HJSON
                        input OTP controller memory map file
  -m VMEM, --vmem VMEM  input VMEM file
  -l SV, --lifecycle SV
                        input lifecycle system verilog file
  -o C, --output C      output C file
  -r RAW, --raw RAW     QEMU OTP raw image file

Parameters:
  -k {auto,otp,fuz}, --kind {auto,otp,fuz}
                        kind of content in VMEM input file, default: auto
  -e BITS, --ecc BITS   ECC bit count
  -c INT, --constant INT
                        finalization constant for Present scrambler
  -i INT, --iv INT      initialization vector for Present scrambler
  -w, --wide            use wide output, non-abbreviated content
  -n, --no-decode       do not attempt to decode OTP fields

Commands:
  -s, --show            show the OTP content
  -E, --ecc-recover     attempt to recover errors with ECC
  -D, --digest          check the OTP HW partition digest
  -U, --update          force-update QEMU OTP raw file after ECC recovery
  --clear-bit CLEAR_BIT
                        clear a bit at specified location
  --set-bit SET_BIT     set a bit at specified location
  --toggle-bit TOGGLE_BIT
                        toggle a bit at specified location
  -L, --generate-lc     generate lc_ctrl C arrays
  -P, --generate-parts  generate partition descriptor C arrays
  -R, --generate-regs   generate partition register C definitions

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

This script can be used for several purposes:

1. Creating a QEMU OT OTP image file that is used as the OTP or Fuse controller backend, from a
   VMEM file that is generated when the HW files are created,
2. Showing and decoding the content of OTP image files, whether it is a pristine generated file
   or a file that has been modified by the QEMU machine,
3. Verifying the Digest of the OTP partitions that support HW digest (using Present scrambling),
4. Create QEMU C source files containining definition values that replicate the ones generated when
   the OT HW is built.

Please note that only the first feature is supported for Fuse (non-OpenTitan) images.

### QEMU OTP RAW image versions

There are two versions of the RAW image file:

v1 and v2 differs from the presence or not of the Present scrambling constants. When a QEMU OTP RAW
image file contains the constants, the v2 format is used, otherwise the v1 format is used. The
script supports both versions.

Fuse RAW images only use the v1 type.

### Arguments

* `-c` specify the finalization constant for the Present scrambler used for partition digests.
  This value is "usually" found within the `hw/ip/otp_ctrl/rtl/otp_ctrl_part_pkg.sv` OT file,
  from the last entry of `RndCnstDigestConst` array, _i.e._ item 0. It is used along with option
  `-D` to verify partition digests, and stored in the optional QEMU OTP RAW image file for use by
  the virtual OTP controller when used along with the `-r` option.

* `-c` specify the register file, which is only useful to decode OTP content (see `-s` option).
  This option is required when `-D` Present digest checking is used.

* `-D` performs a partition digest checks for all partitions with a defined digest. The Present
  constant should be defined to perform digest verification. They can be specified with the `-c` and
  `-i` options switches, or when using a QEMU OTP RAW v2 file that stores these constants.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-E` use ECC data to fix recoverable errors

* `-e` specify how many bits are used in the VMEM file to store ECC information. Note that ECC
  information is not stored in the QEMU RAW file for now.

* `-j` specify the path to the HJSON OTP controller map file, usually stored in OT
  `hw/ip/otp_ctrl/data/otp_ctrl_mmap.hjson`. This file is required with many options when the OTP
  image file needs to be interpreted, such as digest verification, content dump, C file generation,
  ...

* `-k` specify the kind of input VMEM file, either OTP or Fuse kind. The script attempts to detect
  the kind of the input VMEM file from its content when this option is not specified or set to
  `auto`. It is fails to detect the file kind or if the kind needs to be enforced, use this option.

* `-L` generate a file describing the LifeCycle contants as C arrays. Mutually exclusive with the
  `-P` and `-R` option switches. See `-o` to specify an output file.

* `-l` specify the life cycle system verilog file that defines the encoding of the life cycle
  states. This option is not required to generate a RAW image file, but required when the `-L`
  option switch is used.

* `-i` specify the initialization vector for the Present scrambler used for partition digests.
  This value is "usually" found within the `hw/ip/otp_ctrl/rtl/otp_ctrl_part_pkg.sv` OT file,
  from the last entry of `RndCnstDigestIV` array, _i.e._ item 0. It is used along with option
  `-D` to verify partition digests, and stored in the optional output OTP image file for use by
  the virtual OTP controller when used along with the `-o` option.

* `-m` specify the input VMEM file that contains the OTP fuse content. See also the `-k` option.

* `-n` tell the script not to attempt to decode the content of encoded fields, such as the hardened
  booleans values. When used, the raw value of each field is printed out.

* `-o` specify the path to the output C file to generate, see `-L`, `-P` and `-R` option switches.
  Defaults to the standard output.

* `-P` generate a file describing the OTP partition properties as C arrays. Mutually exclusive with
  the `-L` and `-R` option switches. See `-o` to specify an output file.

* `-R` generate a file describing the OTP partition registers as C defintion. Mutually exclusive
  with the `-L` and `-P` option switches. See `-o` to specify an output file.

* `-r` specify the path to the QEMU OTP RAW image file. When used with the `-m` option switch, a
  new QEMU OTP image file is generated. Otherwise, the QEMU OTP RAW image file should exist and is
  used as the data source for the OTP content, such as content decoding and digest verification.

* `-s` decodes some of the content of the OTP fuse values. This option requires the `-j` option.

* `-U` when a RAW file is loaded, update its contents after bit modification changes or ECC
  recovery. This option is mutually exclusive with VMEM

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

* `-w` tell the script not to truncate the values of the large fields, _i.e._ the fields than
  contain long sequence of bytes. If repeated, the empty long fields are also printed in full, as
  a sequence of empty bytes.

* `--clear-bit` clears the specified bit in the OTP data. This flag may be repeated. This option is
  only intended to corrupt the OTP content so that HW & SW behavior may be exercised should such
  a condition exists. See [Bit position syntax](#bit-syntax) for how to specify a bit.

* `--set-bit` sets the specified bit in the OTP data. This flag may be repeated. This option is
   only intended to corrupt the OTP content so that HW & SW behavior may be exercised should such
   a condition exists. See [Bit position syntax](#bit-syntax) for how to specify a bit.

* `--toggle-bit` toggles the specified bit in the OTP data. This flag may be repeated. This option
  is only intended to corrupt the OTP content so that HW & SW behavior may be exercised should such
  a condition exists. See [Bit position syntax](#bit-syntax) for how to specify a bit.

#### Note

Earlgrey OTP virtual device has not been updated to support Present scrambler, so neither `-C` nor
`-I` option should be used to generate an Earlgrey-compatible RAW image.

### Bit position specifier [#bit-syntax]

`--clear-bit` and `--set-bit` options expect the location of the bit to alter. The syntax to specify
a bit is defined as `<offset>/<bit>` where `offset` is the byte offset in the OTP data stream and
`bit` is the bit within the specified location.

The address is rounded down to the granule size, _e.g._ if OTP fuses are organized as 16-bit slots,
address 2N and 2N+1 are considered the same.

If the bit is larger than the data slot, it indicates the location with the ECC part, _e.g._ if OTP
fuses are organized as 16-bit slots wtih 6-bit ECC, bit 0 to 15 indicates a bit into the data slot,
while bit 16 to 21 indicates an ECC bit.

### Examples

Generate a QEMU RAW v1 image for the virtual OTP controller, here with an RMA OTP configuration:
````sh
scripts/opentitan/otptool.py -m img_rma.24.vmem -r otp.raw [-k otp]
````

Generate a QEMU RAW v1 image for the virtual OTP controller, here with an RMA OTP configuration:
````sh
scripts/opentitan/otptool.py -m fusemap.hex -r fuse.raw [-k fuz]
````

The following examples only apply with the OpenTitan OTP image files:

Generate a QEMU RAW v2 image for the virtual OTP controller, here with an RMA OTP configuration:
````sh
scripts/opentitan/otptool.py -m img_rma.24.vmem -r otp.raw \
    -i 0x0123456789abcdef -c 0x00112233445566778899aabbccddeeff
````

Decode the content of an OTP VMEM file:
````sh
scripts/opentitan/otptool.py -m img_rma.24.vmem -j otp_ctrl_mmap.hjson -s
````

Decode the content of a QEMU OTP RAW file:
````sh
scripts/opentitan/otptool.py -r otp.raw -j otp_ctrl_mmap.hjson -s
````

Decode the content of a QEMU OTP RAW file along with the lifecycle information:
````sh
scripts/opentitan/otptool.py -r otp.raw -j otp_ctrl_mmap.hjson -l lc_ctrl_state_pkg.sv -s
````

Verify the HW digests of a QEMU OTP RAW v2 file:
````sh
scripts/opentitan/otptool.py -r otp.raw -j otp_ctrl_mmap.hjson -D
````

Verify the HW digests of a QEMU OTP RAW v1 file:
````sh
scripts/opentitan/otptool.py -r otp.raw -j otp_ctrl_mmap.hjson -D \
    -i 0x0123456789abcdef -c 0x00112233445566778899aabbccddeeff
````

Generate a C source file with LifeCycle constant definitions:
````sh
scripts/opentitan/otptool.py -L -l lc_ctrl_state_pkg.sv -o lc_state.c
````

Generates a C source file with OTP partition properties:
````sh
scripts/opentitan/otptool.py -j otp_ctrl_mmap.hjson -P -o otp_part.c
````
