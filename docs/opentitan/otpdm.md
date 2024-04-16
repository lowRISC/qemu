# `dtm.py`

`otpdm.py` gives access to the OTP Controller through the JTAG/DTM/DM interface.

## Usage

````text
usage: otpdm.py [-h] [-H HOST] [-P PORT] [-Q] [-l IR_LENGTH] [-b BASE] [-j HJSON] [-a ADDRESS]
                [-p PARTITION] [-i ITEM] [-L] [-r] [-w WRITE] [-D] [-v] [-d]

OTP controller access through the RISC-V Debug Module

options:
  -h, --help            show this help message and exit

Virtual machine:
  -H HOST, --host HOST  JTAG host (default: localhost)
  -P PORT, --port PORT  JTAG port, default: 3335
  -Q, --no-quit         do not ask the QEMU to quit on exit

DMI:
  -l IR_LENGTH, --ir-length IR_LENGTH
                        bit length of the IR register
  -b BASE, --base BASE  define DMI base address

OTP:
  -j HJSON, --otp-map HJSON
                        input OTP controller memory map file
  -a ADDRESS, --address ADDRESS
                        base address the OTP controller, default: 0x30130000
  -p PARTITION, --partition PARTITION
                        select a partition
  -i ITEM, --item ITEM  select a partition item
  -L, --list            list the partitions and/or the items
  -r, --read            read the value of the selected item
  -w WRITE, --write WRITE
                        write the value to the selected item
  -D, --digest          show the OTP HW partition digests

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode````
````

### Arguments

* `-a` specify an alternative address for the OTP controller on the OT bus.

* `-b` specify the DMI base address for the RISC-V Debug Module.

* `-D` show partition digest(s). If no parition is selected (see option `-p`), the digest of all
       partitions are shown; otherwise the digest of the selected partition is shown. Requires
       option `-j`.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-H` specify the address of the QEMU VM.

* `-i` select a specific item from a partition. See option `-L` to get a list of valid item names
       for the currently selected partition. Requires `-p` option.

* `-j` specify the path to the OpenTitan OTP controller map, _e.g._ `otp_ctrl_mmap.hjson`.

* `-L` list the names of the partitions if no partition is selected (see option `-p`). If option
       `-p` is used, list the names of the selected partition items if no item is selected (see
       option `-i`). If option `-i` is used, show all the properties of the selected item.

* `-l` specify the length of the TAP instruction register length.

* `-P` specify the TCP port of the JTAG server in the QEMU VM, should match the port part of `-jtag`
       option for invoking QEMU.

* `-p` select a partition using its name. See option `-L` to get a list of valid partition names.
       Requires option `-j`.

* `-Q` do not send QEMU a request for termination when this script exits.

* `-r` load and show the value of the selected item. Requires options `-p` and `-i`.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

* `-w` store the value into the selected item. Requires options `-p` and `-i`. The value should be
       specifed as an hexadecimal or decimal integer for item whose size is less or equal to 8
       bytes. For larger items the value should be specified as a sequence of hexa-encoded bytes.

### Examples

Running QEMU VM with the `-jtag tcp::3335` option:

* List all supported partitions
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -L
  ````

* List all supported items of a partition
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG1 -L
  ````

* List all properties of an item
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG1 -i EN_SRAM_IFETCH -L
  ````

* Show all digests
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -D
  ````

* Show the digest of a single partition (parsable output)
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG1 -D
  ````

* Read the value of an item along with the item properties
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG1 -i EN_SRAM_IFETCH -r
  ````

* Read the value of an item along (parsable output)
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG1 -i EN_SRAM_IFETCH -r
  ````

* Write the value of an integer item
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG1 -i EN_SRAM_IFETCH -w 0xff
  ````

* Write the value of a long item
  ````sh
  ./scripts/opentitan/otpdm.py -j .../otp_ctrl_mmap.hjson -p HW_CFG0 -i DEVICE_ID \
    -w 4c6f72656d20697073756d20646f6c6f722073697420616d65742c20636f6e73
  ````

### Troubleshooting

See the [Troubleshooting](jtag-dm.md#troubleshooting) section for details.

