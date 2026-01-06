# OpenTitan machine

### Machine configuration

To start up an OpenTitan virtual machine, a configuration file is required. See [OT config](otcfg.md)
documentation for details. The configuration file should be specified with the `-readconfig` option.

### `-M <machine>` optional arguments

* `no_epmp_cfg=true` can be appended to the machine option switch, _i.e._
  `-M ot-earlgrey,no_epmp_cfg=true` to disable the initial ePMP configuration, which can be very
  useful to execute arbitrary code on the Ibex core without requiring an OT ROM image to boot up.

* `ignore_elf_entry=true` can be appended to the machine option switch, _i.e._
  `-M ot-earlgrey,ignore_elf_entry=true` to prevent the ELF entry point of a loaded application to
  update the vCPU reset vector at startup. When this option is used, with `-kernel` option for
  example, the application is loaded in memory but the default machine reset vector is used.

* `verilator=true` can be appended to the machine option switch, to select Verilator lowered clocks:
  _i.e._ `-M ot-earlgrey,verilator=true` to select Verilator reduced clock rates. It enables to run
  FW which has been built for the Verilator target in OpenTitan main repository.

See also [ot-ibex_wrapper.lc-ignore option](ot_ibex_wrapper.md) to enable VM start up without a
valid [OTP](ot_otp.md) image file.
