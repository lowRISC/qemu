# Ibex SoC emulation

## Requirements

1. A native toolchain for C and Rust
2. Ninja build tool

Note: never tested on Windows hosts.

## Building

````sh
mkdir build
cd build
../configure --target-list=riscv32-softmmu --without-default-features --enable-tcg \
    --enable-tools --enable-trace-backends=log [--enable-gtk | --enable-cocoa]
ninja
ninja qemu-img
````

* `--enable-gtk` and `--enable-cococa` are only useful when using a graphical display, such as the
  IbexDemo platform. It is mosly useless with the OpenTitan platform.

    * `--enable-gtk` should be used on Linux hosts
    * `--enable-cocoa` should be used on macOS hosts

* `--extra-cflags=-Wno-deprecated-declarations` and
  `--extra-ldflags=-Wl,-no_warn_duplicate_libraries` may be required to build on recent relases of
   macOS (QEMU issues which are not related to the OpenTitan port)

### Useful build options

 * `--enable-debug`
 * `--enable-sanitizers`

## Supported platforms

 * [IbexDemo](ibexdemo.md) built for Digilent Arty7 board
 * [Darjeeling](darjeeling.md) build for CW310 "Bergen" board
 * [EarlGrey](earlgrey.md) build for CW310 "Bergen" board
