# OpenTitan OTP

* `-drive if=pflash,file=otp.raw,format=raw` should be used to specify a path to a QEMU RAW image
  file used as the OpenTitan OTP image. This _RAW_ file should have been generated with the
  [`otptool.py`](otptool.md) tool.

* on LC escalate reception, it is possible to early abort VM execution. Specify
  `-global ot-otp-<top>.fatal_escalate=true` to enable this feature.
  where `top` should be defined as `eg` for the [EarlGrey](ot_earlgrey.md) machine, or `dj` for the
  [Darjeeling](ot_darjeeling.md) machine.
