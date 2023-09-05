# OT GPIO

## Initial configuration

It is possible to configure initial values of the GPIO input pins:

OpenTitan GPIO driver accept a global option:

- `ot-gpio.in` that defines the input values of the GPIO port as a 32-bit value

### Example

```
-global ot-gpio.in=0x00ffff00
```
