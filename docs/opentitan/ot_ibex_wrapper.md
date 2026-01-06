# OpenTitan Ibex Wrapper

* `-global ot-ibex_wrapper.lc-ignore=on` should be used whenever no OTP image is provided, or if
  the current LifeCycle state stored in the OTP image does not allow the Ibex core to fetch data.
  This switch forces the Ibex core to execute whatever the LifeCycle broadcasted signal, which
  departs from the HW behavior but maybe helpful to run the machine without a full OTP set up. The
  alternative to allow the Ibex core to execute guest code is to provide a valid OTP image with one
  of the expected LifeCycle state, such as TestUnlock*, Dev, Prod or RMA.

* `-global ot-ibex_wrapper.lc-ignore-ids=<ids>` acts as `lc-ignore`, enabling the selection of
  specific ibex wrapper instance based on their unique identifiers. See `ot_id` property in the
  machine definition file for a list of valid identifiers. `<ids>` should be defined as a comma-
  separated list of valid identifiers. It is only possible to ignore LifeCycle states with this
  option, not to enforce them.

The `FPGA_INFO` register of the Ibex Wrapper device is used to report that the HW platform is a QEMU
virtual machine. It contains three ASCII chars `QMU` followed with a configurable _version_ field in
the MSB, whose meaning is not defined. It can be any 8-byte value, and defaults to 0x0. To configure
this version field, use the `qemu_version` property of the Ibex Wrapper device.

The `DV_SIM_STATUS` register (address 0 of the `DV_SIM_WINDOW`) can be used to exit QEMU with a
passing or failing status code. The lower half of the word is written either `900d` (good) or `baad`
(bad). If an error code is written to the upper half of the word, QEMU will exit with that status
code for failures. The `-global ot-ibex_wrapper.dv-sim-status-exit=[on|off]` can be used to control
whether QEMU shuts down when this register is written.

There are two modes to handle address remapping, with different limitations:

- default mode: use an MMU-like implementation (via ot_vmapper) to remap addresses. This mode
  enables to remap instruction accesses and data accesses independently, as the real HW. However,
  due to QEMU limitations, addresses and mapped region sizes should be aligned and multiple of 4096
  bytes, i.e. a standard MMU page size. This is the recommended mode.

- legacy mode: This mode has no address nor size limitations, however it cannot distinguish
  instruction accesses from data accesses, which means that both kind of accesses must be defined
  for each active remapping slot for the remapping to be enabled. Moreover it relies on MemoryRegion
  aliasing and may not be as robust as the default mode. It is recommended to use the default mode
  whenever possible. To enable this legacy mode, set the `alias-mode` property to true:
  `-global ot-ibex_wrapper.alias-mode=true`
