# `keymgr-dpe.py`

`keymgr-dpe.py` is a helper tool that can be used to generate or verify Key Manager DPE keys, using
the same parameters as the QEMU machine and the real HW, for debugging purposes.

## Usage

````text
usage: keymgr-dpe.py [-h] -c CFG -j HJSON [-l SV] [-m VMEM] [-R RAW] [-r ROM]
                     [-e BITS] [-z SIZE] [-v] [-d]
                     {generate,execute,verify} ...

QEMU OT tool to generate Key Manager DPE keys.

positional arguments:
  {generate,execute,verify}
                        Execution mode
    generate            generate a key
    execute             execute sqeuence
    verify              verify execution log

options:
  -h, --help            show this help message and exit

Files:
  -c, --config CFG      input QEMU OT config file
  -j, --otp-map HJSON   input OTP controller memory map file
  -l, --lifecycle SV    input lifecycle system verilog file
  -m, --vmem VMEM       input VMEM file
  -R, --raw RAW         input QEMU OTP raw image file
  -r, --rom ROM         input ROM image file, may be repeated

Parameters:
  -e, --ecc BITS        ECC bit count (default: 6)
  -z, --rom-size SIZE   ROM image size in bytes, may be repeated

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

### Arguments

* `-c` QEMU OT config file, which can be generated with the [`cfggen.py`](cfggen.md) script.
  See also predefined files from the `docs/config/opentitan` directory.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-e` specify how many bits are used in the HEX and VMEM files to store ECC information.

* `-j` specify the path to the HJSON OTP controller map file, usually stored in OT
  `hw/ip/otp_ctrl/data/otp_ctrl_mmap.hjson`, required to decode any OTP file.

* `-l` specify the life cycle system verilog file that defines the encoding of the life cycle
  states, required to decode the Life Cycle state, which is stored in the OTP file.

* `-m` specify the input VMEM file that contains the OTP fuse content, mutually exclusive with `-R`

* `-r` specify the path to a ROM file. This option should be repeated for each ROM file. The ROM
  file may either be a ELF or a binary file, in which case the same count of `-z` ROM size option,
  specified in the same order as the ROM file, is required. When such a ROM file format is used, the
  ROM digest is computed from the ROM file content. The ROM file can also be specified a `HEX`
  scrambled file, in which case the digest is read from the file itself. `VMEM` format is not yet
  supported.

* `-R` specify the path to the QEMU OTP RAW image file, which can be generated with the
  [`otptool.py`](otptool.md), mutually exclusive with option `-m`.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

* `-z` specify the size of each ROM file in bytes. It should be repeated for each specified ROM
 file, except if all ROM files are specified as HEX formatted files. It can either be specified as
 an integer or an integer with an SI-suffix (Ki, Mi), such as `-z 64Ki`.

Depending on the execution mode, the following options are available:

## Generate options

This mode can be used to generate a single output key, which can be stored into an output file.

```
usage: keymgr-dpe.py [-h] -c CFG -j HJSON [-l SV] [-m VMEM] [-R RAW] [-r ROM]
                     [-e BITS] [-z SIZE] [-v] [-d]
                     {generate,execute,verify} ...

QEMU OT tool to generate Key Manager DPE keys.

positional arguments:
  {generate,execute,verify}
                        Execution mode
    generate            generate a key
    execute             execute sequence
    verify              verify execution log

options:
  -h, --help            show this help message and exit

Files:
  -c, --config CFG      input QEMU OT config file
  -j, --otp-map HJSON   input OTP controller memory map file
  -l, --lifecycle SV    input lifecycle system verilog file
  -m, --vmem VMEM       input VMEM file
  -R, --raw RAW         input QEMU OTP raw image file
  -r, --rom ROM         input ROM image file, may be repeated

Parameters:
  -e, --ecc BITS        ECC bit count (default: 6)
  -z, --rom-size SIZE   ROM image size in bytes, may be repeated

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
```

### Arguments

* `-b` specify the software binding for an _advance_ stage of the key manager. It should be repeated
   for each new stage to execute. Note the first stage does not require a software binding value and
   is always executed. The expected format is a hexa-decimal encoded string (without 0x prefix). It
   is automatically padded with zero bytes up to the maximum software binding size supported by the
   HW.

* `-c` specify a QEMU [configuration file](otcfg.md) from which to read all the cryptographic
  constants. See the [`cfggen.py`](cfggen.md) tool to generate such a file.

* `-g` specify the kind of generation to perform. If not specified, it is inferred from the `-t`
  target option.

* `-j` specify the path to the HJSON OTP controller map file, usually named `otp_ctrl_mmap.hjson`.

* `-l` specify the life cycle system verilog file, usually named `lc_ctrl_state_pkg.sv`, that
  defines the encoding of the life cycle states.

* `-k` the version of the key to generate

* `-o` specify the output file path for the generated key

* `-R` when specified, the Key Manager output slot is encoded as a Rust constant, whose
   name is specified with this option. Default is to print the output slot in plain text.

* `-s` specify the salt for the _generate_ stage of the key manager. The expected format is a hexa-
   decimal encoded string (without 0x prefix). It is automatically padded with zero bytes up to the
   maximum salt size supported by the HW.

* `-t` specify the destination for the _generate_ stage.

### Examples

````sh
keymgr-dpe.py -vv -c docs/config/opentitan/darjeeling.cfg \
    -j otp_ctrl_mmap.hjson -m img_otp.vmem -l lc_ctrl_state_pkg.sv \
    -r base_rom.39.scr.hex -r second_rom.39.scr.hex -b 0 -b 0 -t AES -k 0 -s 0
````

````sh
keymgr-dpe.py -vv -c docs/config/opentitan/darjeeling.cfg \
    -j otp_ctrl_mmap.hjson -m img_otp.vmem -l lc_ctrl_state_pkg.sv \
    -r rom0.elf -r keymgr-dpe-basic-test -z 64Ki -z 32Ki -t SW -k 0 -s 0
````

## Execute options [execute-options]

This mode can be used to execute a sequence of steps, with multiple key generations.

It requires an input file containing the sequence of steps to execute, which follows the INI file
format. Each section in the file represents a step in the sequence, and the options within each
section specify the parameters for that step.

```
usage: keymgr-dpe.py execute [-h] -s INI

options:
  -h, --help          show this help message and exit
  -s, --sequence INI  execution sequence definition
```

### Arguments

* `-s` specify an INI-like configuration file containing the sequence of steps to execute

    Typical input file:

    ````ini
    [step_1]
    mode = initialize
    dst = 0

    [step_2]
    mode = advance
    binding = [0]
    src = 0
    dst = 1
    allow_child = true
    exportable = true
    retain_parent = true

    [step_3]
    mode = generate
    src = 1
    dst = otbn
    key_version = 0
    salt = "[49379059ff52399275666880c0e44716999612df80f1a9de481eae4045e2c7f0]"

    [step_4]
    mode = generate
    src = 1
    dst = none
    key_version = 0
    salt = "[49379059ff52399275666880c0e44716999612df80f1a9de481eae4045e2c7f0]"
    ````

## Verify options

This mode can be used to verify the execution of a unit test, which is expected to print out the
input parameters it sends to the KeyManger DPE and the output key which is generated by the latter.

Note that as the generated sideloaded key cannot usually be accessed by the Ibex core, the following
tricks are used:

* for AES sideloaded key, the AES key is used to encrypt a zeroed plaintext, and the verification is
performed on the ciphertext
* for KMAC sideloaded key, the KMAC key is used to perform a hash on a zeroed input buffer, and the
verification is performed on the resulting hash value.
* for OTBN sideloaded key, the OTBN key is read by the OTBN core and replicated into its data
memory, which is then read back by the Ibex core. In this cas, the direct key is verified.

```
usage: keymgr-dpe.py verify [-h] -l LOG

options:
  -h, --help          show this help message and exit
  -l, --exec-log LOG  execution log to verify
```

### Arguments

* `-l` specify the execution log to verify. The execution log is expected to contain the output of
  a test that has run on the OpenTitan platform. It should emit a syntax identical to the format
  described in the [Execute options](#execute-options) section, _i.e._ an INI-like syntax. To
  distinguish INI syntax from any other log output, each line of interest should be prefixed with a
  `T> ` marker.

    [`pyot.py`](pyot.md) script may be used to generate the log file, see `--log-file` option or the
    `log_file` test parameter.
