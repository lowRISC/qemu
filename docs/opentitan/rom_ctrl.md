# OpenTitan ROM Controller

The OpenTitan ROM Controller computes a digest of the ROM content on boot, using one of the
application interfaces of the KMAC. When the ROM check is complete, the ROM Controller sends a
signal to the Power Manager. This triggers CPU startup if the ROM check was successful.

On real hardware, the ROM digest is computed on the ROM content _including ECC_ and the expected
digest value is stored at the end of the scrambled ROM (with invalid ECC to make sure it's not
accessible by software).

On the QEMU emulated ROM Controller, if the ROM controller is submitted a scrambled ROM image file,
its digest is used and verified. Otherwise, a fake digest is generated, i.e. no actual ROM content
validation is performed.

## QEMU ROM Options

### Supported image file formats

The type of a ROM image file format is automatically detected from its content.

The ROM file digest can be provided on the QEMU command line using this option:

```
-object ot-rom_img,id=<romid>,file=/path/to/rom
```

#### ELF ROM file

ELF32 RISC-V file can be used as a ROM controller image file. Such file format neither supports
digest nor ECC. Digest is faked in this case.

#### VMEM ROM file

Two kinds of VMEM files are supported:

* 32-bit VMEM files, _i.e._ VMEM file without ECC. Such file should contain non-scrambled data.
* 39-bit VMEM files, _i.e._ VMEM file with 7-bit ECC and scrambled data.

39-bit scrambled ECC VMEM files are only supported when the QEMU machine has instantiated the ROM
controller with `key` and `nonce` arguments.

#### HEX ROM file

39-bit HEX files, _i.e._ HEX file with 7-bit ECC and scrambled data.

39-bit scrambled ECC HEX files are only supported when the QEMU machine has instantiated the ROM
controller with `key` and `nonce` arguments.

Note that HEX file format differs from IHEX file format. The former only contains hexadecimal-
encoded data, where the two first digits contain the 7-bit ECC value, and the remaining digits
contains the 32-bit data value.

#### Binary ROM file

Flat, raw binary file can be used as a ROM controller image file. Such file format neither supports
digest nor ECC. Digest is faked in this case.

### ROM identifiers [#romid]

The ROM image ID may depend on the SoC.

* for EarlGrey which has a single ROM, the ID is expected to be `rom`.
* for a SoC with two ROMs, the IDs would be expected to be `rom0` and `rom1`.
* for a machine with multiple SoCs, the IDs would be additionally prefixed with the SoC name and a
  full stop, _e.g._ `soc0.rom0`

### ROM unscrambling constants

Each machine and each ROM controller uses a different pair of (key, nonce) constants to unscramble
the ROM content.

These constants may be defined in the machine, or in an external configuration file so that these
constants are present neither in the QEMU source code nor the QEMU binary. The standard QEMU
`-readconfig <file.cfg>` may be used to load those constants.

See [OpenTitan configuration file](otcfg.md) for details.

## Booting with and without ROM

### With ROM

This mode is useful to implement the standard OpenTitan boot flow.

In this mode, the ROM digest is always checked against the expected digest value if a scrambled ROM
image file is submitted.

### Without ROM

This mode is useful when starting an application using `-kernel` QEMU option: the application starts
directly at CPU reset.

If no ROM image is specified on QEMU command line (or if the ROM image does not have the expected ID
for the machine), it does not prevent the CPU from starting:
- ROM Controller provides an empty ROM region
- ROM Controller activates _fake digest_ mode: digest is computed on the empty ROM region and
  expected digest value is faked. ROM is "valid" so signal is sent to Power Manager to start the
CPU.

When the `-kernel` option is used, the default `resetvec` of the machine is overridden with the
entry point defined in the ELF file. The default `mtvec` is not modified - it is expected the ELF
early code updates the `mtvec` with a reachable location if no ROM is used.
