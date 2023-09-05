# OT GPIO

## Initial configuration

It is possible to configure initial values of the GPIO input pins:

OpenTitan GPIO driver accept a global option:

- `ot-gpio.in` that defines the input values of the GPIO port as a 32-bit value

### Example

```
-global ot-gpio.in=0x00ffff00
```

## Character Device

OpenTitan GPIO driver can be connected to any CharDev device to expose as a very simple ASCII data
stream the GPIO input and output pins.

This CharDev device can be used to stimulate the GPIO and perform unit tests.

To connect the GPIO to its optional characted device, use the following QEMU option

```
-chardev type,id=gpio -global ot-gpio.chardev=gpio
```

where type is one of the supported QEMU chardev type, such as

- serial to connect to an existing serial communication device
- socket to use a TCP connection
- pipe to use a local pipe
- ...

`id` is an arbitrary string that should always match the value defined with the `-global` option.

### Protocol

The communication protocol is ASCII based and very simple.
Each frame follows the following format:

#### Format

```
<TYPE> : <HEXVALUE> <CR> <LF>
```

where:

1. `TYPE` is a single uppercase char
2. `HEXVALUE` is a 32-bit hex encoded lowercase value
3. `CR` is the carriage return character (0x0d)
4. `LF` is the line feed character, or end-of-line (0x0a)

Each frame is delimited with `LF` characters. `CR` are ignored and accepted to ease compatibity with
some terminals but are useless.

The hex value represents the 32-bit GPIO values.

#### Supported frame types

A type describes the meaning of the hex value. Supported types are:

* `D` direction, _i.e._ Output Enable in OpenTitan terminology (QEMU -> host)
* `I` input GPIO values (host -> QEMU)
* `O` output GPIO values (QEMU -> host)
* `Q` query input (QEMU -> host). QEMU may emit this frame, so that the host replies with a new
  `I` frame (hexvalue of `Q` is ignored)
* `R` repeat (host -> QEMU). The host may ask QEMU to repeat the last `D` and `O` frames

Frames are only emitted whenever the state of either peer (QEMU, host) change. QEMU should emit `D`
and `O` frames whenever the GPIO configuration or output values are updated. The host should emit
a `I` frame whenever its own input lines change.

`Q` and `R` are only emitted when a host connects to QEMU or when one side resets its internal
state.

### Example

The `scripts/opentitan/trellis` directory contains two Python files that may be copied to an
an Adafruit NeoTrellis M4 Express card initialized with Circuit Python 8.0+

These scripts provide a physical, visual interface to the virtual GPIO pins, which is connected to
the QEMU machine over a serial port (a USB CDC VCP in this case).

To connect to the NeoTrellis board, use a configuration such as:

```
-chardev serial,id=gpio,path=/dev/ttyACM1 -global ot-gpio.chardev=gpio
```

where /dev/ttyACM1 is the data serial port of the Neotreillis board.

Note: the first serial port of the board is reserved to its debug console.
