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
  IbexDemo platform.

    * `--enable-gtk` should be used on Linux hosts
    * `--enable-cocoa` should be used on macOS hosts

### Useful build options

 * `--enable-debug`
 * `--enable-sanitizers`

## Supported platforms

 * [IbexDemo](ibexdemo.md) built for Digilent Arty7 board
