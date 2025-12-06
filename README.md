# QEMU OpenTitan README

[![.github/workflows/build_test.yaml](https://github.com/lowRISC/qemu/actions/workflows/build_test.yaml/badge.svg?branch=ot-10.1.0)](https://github.com/lowRISC/qemu/actions/workflows/build_test.yaml)

QEMU is a generic and open source machine & userspace emulator and virtualizer.

QEMU is capable of emulating a complete machine in software without any need for hardware
virtualization support. By using dynamic translation, it achieves very good performance.

This branch contains a fork of QEMU 10.1.0 dedicated to support lowRISC Ibex platforms:
  * [OpenTitan](https://opentitan.org) [EarlGrey](docs/opentitan/ot_earlgrey.md), based on
    [1.0.0](https://github.com/lowRISC/opentitan/tree/earlgrey_1.0.0).
  * [OpenTitan](https://opentitan.org) [Darjeeling](docs/opentitan/ot_darjeeling.md), based on
    [development version](https://github.com/lowRISC/opentitan/tree/master).
  * [lowRISC](https://github.com/lowRISC/ibex-demo-system) [IbexDemo](docs/opentitan/ibexdemo.md).

See [installation instructions](docs/opentitan/index.md)

See also original [QEMU README file](README_QEMU.rst)
