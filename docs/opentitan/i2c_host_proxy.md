# OpenTitan I2C Host Proxy

The `ot-i2c-host-proxy` device provides a way to execute I2C transactions with QEMU I2C bus devices
through an externally driven `chardev` interface.

The device can perform read or write transactions with bus devices and is intended to be used to
drive the Opentitan I2C device Target mode, though it can be used to interact with other devices on
the QEMU I2C buses.

## Configuration

A chardev and proxy device can be created in QEMU with the following arguments:

`-chardev pty,i2cN -device ot-i2c-host-proxy,bus=<bus>,chardev=i2cN`

Where `bus` is any of the system's I2C buses. On Earl Grey for example, these are `ot-i2c0`,
`ot-i2c1`, and `ot-i2c2`.

## Protocol

The protocol over the chardev mirrors the I2C specification closely.

Before each transaction, the protocol version must be sent. This is sent as an `i` byte (for I2C),
followed by the version number byte (currently 1). The device will respond with an ACK byte
(see below) if the version matches the implementation, and a NACK byte otherwise.

A start and repeated start condition is signalled with an `S` byte, followed by a byte containing
the 7-bit target address of the transaction and the read/write bit as the least significant bit
(0 indicates a write transfer, 1 indicates a read transfer).

On a repeated start condition, the I2C target address of the first transfer will be used, and the
provided address is ignored (transfer direction can be changed). This is a limitation of QEMU.

When a read transfer is active, a `R` byte can be sent to read a byte of data from the target I2C
device.

When a write transfer is active, a `W` byte, followed by a data byte, can be sent to write that
byte to the target I2C device.

A stop condition to end the transaction is signalled with a `P` byte.

Following a start/repeated start condition and the address byte, as well as after every byte
written during a write transfer, an ACK is signalled with a `.` byte, and a NACK is signalled with
a `!` byte. If the start of a transfer is NACKed, the transaction should be terminated with a stop
condition.

On a parser error, a `x` byte will be returned, and the parser will not accept any more command
bytes until reset.

The implementation will throttle processing of written bytes to allow control to return to the
vCPU, so that OT I2C may process the current transaction and prepare response bytes for upcoming
reads.
