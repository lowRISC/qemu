# OpenTitan Reset Manager

It is possible to limit the number of times the VM reboots the guest. This option may be useful
during the development process when an issue in the early FW stages - such as the ROM - causes an
endless reboot cycles of the guest.

To limit the reboot cycles, use the `-global ot-rstmgr.fatal_reset=<N>` option, where `N` is an
unsigned integer. This option forces the QEMU VM to exit the N^th^ time the reset manager receives
a reset request, rather than rebooting the whole machine endlessly as the default behavior.
