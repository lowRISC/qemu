# OpenTitan ROM Controller

The OpenTitan ROM Controller computes a digest of the ROM content on boot, using one of the
application interfaces of the KMAC. When the ROM check is complete, the ROM Controller sends a
signal to the Power Manager. This triggers CPU startup if the ROM check was successful.

On real hardware, the ROM digest is computed on the ROM content _including ECC_ and the expected
digest value is stored at the end of the scrambled ROM (with invalid ECC to make sure it's not
accessible by software).

On the QEMU emulated ROM Controller, the digest is instead computed on the ROM content _without
ECC_. The expected digest value either comes from the command line (see section "QEMU ROM options")
or is faked (the expected digest registers are preloaded with the computed value just before the
comparison happens).

## QEMU ROM Options

### Digest

The ROM digest is either a sequence of 64 hex digits (_i.e._ 32 bytes) or the exact string `fake` to
enable _fake digest_ mode).

### ELF ROM file

The ROM ELF file and its digest can be provided on the QEMU command line using this option:

```
-object ot-rom-img,id=rom,file=/path/to/rom.elf,digest=<romdigest>
```

### Binary ROM file

The ROM binary file and its digest can be provided on the QEMU command line using this option:

```
-object ot-rom-img,id=rom,file=/path/to/rom.bin,digest=<romdigest>,addr=address
```

If `addr` is specifed, the file is loaded as a raw binary file, whatever its extension.

`addr` is specified as an absolute address in the CPU address space, _i.e._ not relative to the
ROM base address.

### ROM identifiers

The ROM image ID may depend on the SoC.

* for EarlGrey which has a single ROM, the ID is expected to be `rom`.
* for a SoC with two ROMs, the IDs would be expected to be `rom0` and `rom1`.

## Booting with and without ROM

### With ROM

This mode is useful to implement the standard OpenTitan boot flow.

In this mode, the ROM digest is always checked against the expected digest value. A _fake digest_
mode is implemented to avoid having to pass a valid digest: when enabled (using `digest=fake`), the
expected digest is seeded from the value that is computed in order for the check to always succeed.

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
