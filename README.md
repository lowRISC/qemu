# QEMU OpenTitan README

[![.github/workflows/build_test.yaml](https://github.com/lowRISC/qemu/actions/workflows/build_test.yaml/badge.svg?branch=ot-darjeeling-8.2.0)](https://github.com/lowRISC/qemu/actions/workflows/build_test.yaml)

QEMU is a generic and open source machine & userspace emulator and virtualizer.

QEMU is capable of emulating a complete machine in software without any need for hardware
virtualization support. By using dynamic translation, it achieves very good performance.

This branch contains a fork of QEMU 8.2.0 dedicated to support lowRISC Ibex platforms:
  * [OpenTitan](https://opentitan.org) [Darjeeling](docs/opentitan/darjeeling.md) with FPGA "Bergen Board"
    CW310 memory map.
  * [OpenTitan](https://opentitan.org) [EarlGrey](docs/opentitan/earlgrey.md) with FPGA "Bergen Board"
    CW310 memory map, no longer under active QEMU development.
  * [lowRISC](https://github.com/lowRISC/ibex-demo-system) [IbexDemo](ibexdemo.md) with Digilent Arty7
    board memory map.

See [installation instructions](docs/opentitan/index.md)

See also original [QEMU README file](README_QEMU.rst)
