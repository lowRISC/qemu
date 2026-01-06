# OpenTitan Earl Grey Pad Ring

## Power-on Reset (POR)

The Earl Grey Pad Ring currently only supports the negated Power-on Reset (PoR) pad, which can
be signalled by setting the `por_n` property and resetting the VM to perform a Power-on Reset.
This can for example be done with the QEMU Monitor via the following command sequence:
```
> qom-set ot-eg-pad-ring.0 por_n low
> system_reset
> qom-set ot-eg-pad-ring.0 por_n high
```

Equivalent QMP JSON commands can also be used.

Note that the current implementation directly invokes a reset request on any reset where a falling
edge is detected (i.e. the reset strapping is asserted), and it is not well supported to "hold" the
device in reset. If it is desired to emulate this time, you should stop and resume the VM for
for the duration of the reset, e.g.:
```
/* Asserting the POR signal */
> stop
> qom-set ot-eg-pad-ring.0 por_n low
> system_reset
/* ... wait for the duration of the reset ... */
/* De-asserting the POR signal */
> qom-set ot-eg-pad-ring.0 por_n high
> cont
```

## MIO Pads

Currently, Earl Grey's MIO pads are not connected in the Pad Ring / Pinmux.
