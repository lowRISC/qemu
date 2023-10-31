# SPI Device support

## Supported modes

### FW/Generic mode

This mode is only partially supported, and is being deprecated in real HW, so no further work is
expected for this mode.

### Flash mode

This mode is fully supported (to the extend of the understanding of the HW...).

### Passthrough mode

This mode is not yet supported.

### TPM

This mode is not supported.

## Connection with a SPI Host

### CharDev simple bus header

To communicate with the SPI device emulation, it is possible to create and use any QEMU CharDev.

The CharDev is expected to used as a full bi-directional, stream based, asynchronous communication
channel.

CharDev always output as many payload bytes as it receives, like a regular SPI bus.

### Creating QEMU SPI Device CharDev

The most common/useful CharDev for SPI device is to use a TCP communication stream, which can be
instanciated this way from the command line:

````
-chardev socket,id=spidev,host=localhost,port=8004,server=on,wait=off
-global ot-spi_device.chardev=spidev
````

Note that `opentitantool` and association library do support this protocol when the `qemu` backend
is used:

````
opentitantool --interface qemu --help
A tool for interacting with OpenTitan chips.

Usage: opentitantool [OPTIONS] <COMMAND>

Commands:
  ...
  spi        Commands for interacting with a SPI EEPROM
  ...
      --qemu-host <QEMU_HOST>                            [default: localhost]
      --qemu-spidev-port <QEMU_SPIDEV_PORT>              [default: 8004]
````

### SPI device CharDev protocol

SPI clock is not emulated, but each byte exchanged over the communication channel represent 8-bit
SPI data. Dual and Quad lines are not emulated, all communications happen over a regular byte
stream - which does not prevent from using dual or quad SPI flash commands.

As some out-of-band or metadata is required, for example the status of the /CS line, a small header
is inserted by the SPI host for each SPI communication packet in the MOSI stream. There is no such
header inserted into the stream for the MISO channel. Moreover as the SPI header contains the length
of the payload, it also acts as a SPI packet delimiter. Each time the SPI host peer is disconnected,
the /CS line and the SPI device state machine are reset so it is possible to recover from a
corrupted communication w/o exiting QEMU.

A packet is limited to a payload of 65536 bytes, however an SPI transaction may extend over several
SPI communication packets.

A CharDev communication is always initiated by the remote SPI host. It transmits the 8-byte SPI
device header described below, where the first word can be considered as a magic number, and
the last word describes the payload that comes right after the header. The payload should contain
the <length> count of bytes (SPI MOSI data). The SPI device should start receiving the very same
<length> count of bytes (SPI MISO data). In other words, the payload in both direction have always
the same amount of bytes.

The host-to-device stream always starts with a 8-byte header, and there is no header, i.e. only
payload in the device-to-host stream. The header <c> bit defines whether the host releases the /CS
line once the SPI transfer is over, or whether the SPI device should expect a continuation SPI
transfer, which is always prefixed with another header. The last SPI transfer of a SPI transaction
should release the /CS line, i.e. <c> should be 0.

````
 Time -->

 MOSI: | Header1 | TX Payload1 <x bytes> | Header2 | TX Payload2 <y bytes> |
                 \                       \         \                       \
 MISO:            | RX Payload1 <x bytes> |         | RX Payload2 <y bytes> |
````

````
       +---------------+---------------+---------------+---------------+
       |       0       |       1       |       2       |       3       |
       +---------------+---------------+---------------+---------------+
       |0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F|
 0x00 -+---------------+---------------+---------------+---------------+
       |      '/'      |      'C'      |      'S'      |    version    |
 0x04 -+---------------+---------------+---------------+---------------+
       |p|a|t|r|  -  |c|       -       |      length (Byte count)      |
 0x08 -+---------------+---------------+---------------+---------------+
       |               |               |               |               |
 0x0c -+---------------+---------------+---------------+---------------+
       |                             . . .                             |
      -+---------------+---------------+---------------+---------------+
       |               |
      -+---------------+
````

  - `version`: protocol version, current version uses 0.
  - `length`: count of payload bytes (does not need to be a multiple of 4)
  - `p`: polarity, should match `CFG.CPOL` (not yet supported)
  - `a`: phase, should match `CFG.CPHA` (not yet supported)
  - `t`: tx order, see `CFG.TX_ORDER` (not yet supported)
  - `r`: rx order, see `CFG.TX_ORDER` (not yet supported)
  - `c`: whether to keep _/CS_ low (=1) or release _/CS_ (=0) when payload has been processed. Any
    SPI transaction should end with C=0 packet. However it is possible to use several SPI device
    CharDev packets to handle a single SPI transaction: example: JEDEC ID w/ continuation code,
    polling for BUSY bit, ...

Note that SPI device support bit reversing for both TX and RX, the bit configuration in the SPI
device header is only used for debugging (tracking configuration mismatch between the host and the
device). Polarity and Phase are not emulated however SPI stream is explitly corrupted
(bit inversion) if CPOL/CPHA do not match between host and device.
