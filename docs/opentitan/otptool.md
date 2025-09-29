# `otptool.py`

`otptool.py` is a tool to deal with QEMU OT RAW image files, which are managed by the OpenTitan OTP
controller virtual device.

## Usage

````text
usage: otptool.py [-h] [-j HJSON] [-m VMEM] [-l SV] [-o FILE] [-r RAW]
                  [-x EXPORT] [-k {auto,otp,fuz}] [-e BITS] [-C CONFIG]
                  [-c INT] [-i INT] [-w] [-n] [-f PART:FIELD] [--no-version]
                  [-s] [-E] [-D] [-U] [-g {LCVAL,LCTPL,PARTS,REGS}] [-F]
                  [-G PART] [--change PART:FIELD=VALUE] [--empty PARTITION]
                  [--erase PART:FIELD] [--clear-bit CLEAR_BIT]
                  [--set-bit SET_BIT] [--toggle-bit TOGGLE_BIT]
                  [--write ADDR/HEXBYTES] [--patch-token NAME=VALUE]
                  [--out-kind {qemu,bmtest}] [--top-name TOP_NAME] [-v] [-d]

QEMU OT tool to manage OTP files.

options:
  -h, --help            show this help message and exit

Files:
  -j, --otp-map HJSON   input OTP controller memory map file
  -m, --vmem VMEM       input VMEM file
  -l, --lifecycle SV    input lifecycle system verilog file
  -o, --output FILE     output filename (default to stdout)
  -r, --raw RAW         QEMU OTP raw image file
  -x, --export EXPORT   Export data to a VMEM file

Parameters:
  -k, --kind {auto,otp,fuz}
                        kind of content in VMEM input file, default: auto
  -e, --ecc BITS        ECC bit count
  -C, --config CONFIG   read Present constants from QEMU config file
  -c, --constant INT    finalization constant for Present scrambler
  -i, --iv INT          initialization vector for Present scrambler
  -w, --wide            use wide output, non-abbreviated content
  -n, --no-decode       do not attempt to decode OTP fields
  -f, --filter PART:FIELD
                        filter which OTP fields are shown
  --no-version          do not report the OTP image version

Commands:
  -s, --show            show the OTP content
  -E, --ecc-recover     attempt to recover errors with ECC
  -D, --digest          check the OTP HW partition digest
  -U, --update          update RAW file after ECC recovery or bit changes
  -g, --generate {LCVAL,LCTPL,PARTS,REGS}
                        generate code, see doc for options
  -F, --fix-ecc         rebuild ECC
  -G, --fix-digest PART
                        rebuild HW digest
  --change PART:FIELD=VALUE
                        change the content of an OTP field
  --empty PARTITION     reset the content of a whole partition, including its
                        digest if any
  --erase PART:FIELD    clear out an OTP field
  --clear-bit CLEAR_BIT
                        clear a bit at specified location
  --set-bit SET_BIT     set a bit at specified location
  --toggle-bit TOGGLE_BIT
                        toggle a bit at specified location
  --write ADDR/HEXBYTES
                        write bytes at specified location
  --patch-token NAME=VALUE
                        change a LC hashed token, using Rust file
  --out-kind {qemu,bmtest}
                        select output format for code generation (default:
                        qemu)
  --top-name TOP_NAME   optional top name for code generation (default: auto)

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

* `-C` specify a QEMU [configuration file](otcfg.md) from which to read the Present constants that
  are required for digest computation. It is a convenience switch to replace both `-i` and options.
  See [`cfggen.py`](cfggen.md) tool to generate such a file.

* `-c` specify the initialization constant for the Present scrambler used for partition digests.
  This option is required when `-D` Present digest checking is used. See also `-i` option switch.
  Override option `-C` if any.

* `-D` performs a partition digest checks for all partitions with a defined digest. The Present
  constant should be defined to perform digest verification. They can be specified with the `-c` and
  `-i` options switches, or when using a QEMU OTP RAW v2 file that stores these constants, or when
  a QEMU configuration file is specified with the `-C` option.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-E` use ECC data to fix recoverable errors

* `-e` specify how many bits are used in the VMEM file to store ECC information.

* `-F` may be used to rebuild the ECC values for all slots that have been modified using any
  modification operation, and any detected error.

* `-f` select which partition(s) and partition field(s) should be shown when option `-s` is used.
  When not specified, all partitions and fields are reported.

* `-G` can be used to (re)build the HW digest of a partition after altering one or more of its
  fields, see `--change` option.

* `-g` can be used to generate skeleton files, from OTP and LifeCycle known definitions. See the
  [Generation](#generation) section for details. See option `-o` to specify the path to the file to
  generate. See also the `--out-kind` option for output formats.

* `-i` specify the initialization vector for the Present scrambler used for partition digests.
  This value is "usually" found within the `hw/ip/otp_ctrl/rtl/otp_ctrl_part_pkg.sv` OT file,
  from the last entry of `RndCnstDigestIV` array, _i.e._ item 0. It is used along with option
  `-D` to verify partition digests, and stored in the optional output OTP image file for use by
  the virtual OTP controller when used along with the `-o` option. Override option `-C` if any.

* `-j` specify the path to the HJSON OTP controller map file, usually stored in OT
  `hw/ip/otp_ctrl/data/otp_ctrl_mmap.hjson`. This file is required with many options when the OTP
  image file needs to be interpreted, such as digest verification, content dump, C file generation,
  ...

* `-k` specify the kind of input VMEM file, either OTP or Fuse kind. The script attempts to detect
  the kind of the input VMEM file from its content when this option is not specified or set to
  `auto`. It is fails to detect the file kind or if the kind needs to be enforced, use this option.

* `-l` specify the life cycle system verilog file that defines the encoding of the life cycle
  states. This option is not required to generate a RAW image file, but required when the `-L`
  option switch is used.

* `-m` specify the input VMEM file that contains the OTP fuse content. See also the `-k` option.

* `-n` tell the script not to attempt to decode the content of encoded fields, such as the hardened
  booleans values. When used, the raw value of each field is printed out.

* `-o` specify the path to the output C file to generate, see `-G` option. If not specified, the
  generated file is emitted on the standard output.

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

* `-x` export the current data and ECC content into a text file using the VMEM 24 encoding format,
  _i.e._ 24-bit hex chunks where the first byte depicts the 6-bit ECC and the remaining two bytes
  contain a 16-bit value.

* `--clear-bit` clears the specified bit in the OTP data. This flag may be repeated. This option is
  only intended to corrupt the OTP content so that HW & SW behavior may be exercised should such
  a condition exists. See [Bit position syntax](#bit-syntax) for how to specify a bit.

* `--change` updates the value of a field in the specified partition. The flag may be repeated.
  Note that changing the partition content without specifying the `-F` flag may result into a
  corrupted partition. `otptool.py` accepts the following value syntax:

  - `"string"` or `'string'`: an arbitrary string, which is automatically encoded into UTF8 raw
    bytes. Note that the shell may interpret the single or double quote characters, so it may be
    required to use `'part:field="string"'` as the argument to bypass any shell replacement,
  - `true`, `false`, `on`, `off`, `enabled`, `disabled` as a boolean specifier. The boolean value is
    encoded according to the OTP map into an hardened boolean or a MUBI value,
  - an hexadecimal value starting with with `0x`
  - a string of hexadecimal nibbles, _e.g._ `01020304abcdef`, which is converted into raw bytes

  If the partition field is larger than the specified value, the value is left-extended with NUL
  bytes. Values larger than the partition field length are rejected.

* `--empty` reset a whole partition, including its digest if any and ECC bits. This option is only
  intended for test purposes. This flag may be repeated. Partition(s) can be specified either by
  their index or their name.

* `--erase` reset a specific field within a partition. The flag may be repeated.

* `--no-version` disable OTP image version reporting when `-s` is used.

* `--out-kind` define the output format for code generation for the `-g` option. Note that only a
  subset of the generation types are available in some output kinds.

* `--patch-token` patch a Life Cycle hashed token. This feature is primary aimed at testing the
  Life Cycle controller. With this option, the partition to update is automatically found using
  the token `NAME`. If the partition containing the token to update is already locked, its digest is
  automatically patched; otherwise the digest is left empty. The token `VALUE` should be specified
  as a hexadecimal string with no `0x` prefix. The token value is expected to be the plaintext
  token, as the script takes care of hashing it and storing the hashed value into the OTP image.
  To change a LC token file with an immediate value with no further processing, see the `--change`
  option.

* `--set-bit` sets the specified bit in the OTP data. This flag may be repeated. This option is
   only intended to corrupt the OTP content so that HW & SW behavior may be exercised should such
   a condition exists. See [Bit position syntax](#bit-syntax) for how to specify a bit.

* `--toggle-bit` toggles the specified bit in the OTP data. This flag may be repeated. This option
  is only intended to corrupt the OTP content so that HW & SW behavior may be exercised should such
  a condition exists. See [Bit position syntax](#bit-syntax) for how to specify a bit.

* `--top-name` defines the OpenTitan top name for some code generation feature. It overrides any
  top name that may be automatically retrieved from the configuration file hierarchy.

* `--write` overrides any data (not ECC). If can be combined with `--fix-ecc` to automatically
  rebuild the ECC of the data slot that have been altered. This option may be repeated. The argument
  should comply with the `offset/hexdata` syntax, where the _offset_ part is defined in the
  [Bit position syntax](#bit-syntax) and _hexdata_ is a byte sequence specified as a hexadecimal
  string.

All modification features can only be performed on RAW image, VMEM images are never modified. To
modify RAW file content, either a VMEM file is required in addition to the RAW file as the data
source, or the `-U` is required to tell that the RAW file should be read, modified and written back.

#### Note

Earlgrey OTP virtual device has not been updated to support Present scrambler, so neither `-c` nor
`-i` option should be used to generate an Earlgrey-compatible RAW image.

### Bit position specifier [#bit-syntax]

`--clear-bit` and `--set-bit` options expect the location of the bit to alter. The syntax to specify
a bit is defined as `<offset>/<bit>` where `offset` is the byte offset in the OTP data stream and
`bit` is the bit within the specified location.

The address is rounded down to the granule size, _e.g._ if OTP fuses are organized as 16-bit slots,
address 2N and 2N+1 are considered the same.

As a special syntax, offset may be specified as `0h...`, in which case the offset value is doubled,
which enables to directly specify the half-word offset encoded in the input VMEM 24 file. For
example, to write at @0006, either use `0xc` or `0h6` as the offset specifier.

If the bit is larger than the data slot, it indicates the location with the ECC part, _e.g._ if OTP
fuses are organized as 16-bit slots wtih 6-bit ECC, bit 0 to 15 indicates a bit into the data slot,
while bit 16 to 21 indicates an ECC bit.

It is possible to tell the script to rebuild the ECC value for the modified bits, using `-F`.
The default behavior is to not automatically update the ECC value, as the primary usage for these
bit modification operations is to test the error detection and correction feature.

### Generation [#generation]

The generation feature may be used to generate part of the OTP and LifeCycle QEMU implementation,
based on known definitions from the OpenTitan constants. This option accepts on of the following
argument:

* `LCVAL` generates a file describing the LifeCycle constants as C arrays. Requires `-l` option.
* `LCTPL` generates a file describing the LifeCycle State encoding as a C array. . Requires `-l`
  option.
* `PARTS` generates a file describing the OTP partition properties as C arrays. Requires `-j`
  option.
* `REGS` generates a file describing the OTP partition registers as C definitions. Requires `-j`
  option.

 See `-o` to specify an output file for the generated file.

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

Generate a QEMU RAW v2 image for the virtual OTP controller, here with an RMA OTP configuration,
load Present constants from a QEMU configuration file.
````sh
scripts/opentitan/otptool.py -m img_rma.24.vmem -r otp.raw -i docs/config/opentitan/earlgrey.cfg
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
scripts/opentitan/otptool.py -g LCVAL -l lc_ctrl_state_pkg.sv -o lc_state.c
````

Generates a C source file with OTP partition properties:
````sh
scripts/opentitan/otptool.py -j otp_ctrl_mmap.hjson -g PARTS -o otp_part.c
````
