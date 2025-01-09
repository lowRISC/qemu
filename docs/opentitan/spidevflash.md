# `spidevflash.py`

`spidevflash.py` is a tiny script to upload bootstrap image using the SPI device virtual device.

## Usage

````text
usage: spidevflash.py [-h] -f FILE [-a ADDRESS] [-S SOCKET] [-R RETRY_COUNT]
                      [-r HOST] [-p PORT] [-v] [-d]

SPI device flasher tool.

options:
  -h, --help            show this help message and exit
  -f, --file FILE       Binary file to flash
  -a, --address ADDRESS
                        Address in the SPI flash (default: 0)
  -S, --socket SOCKET   connection string
  -R, --retry-count RETRY_COUNT
                        connection retry count (default: 1)
  -r, --host HOST       remote host name (default: localhost)
  -p, --port PORT       remote host TCP port (defaults to 8004)
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

### Arguments

* `-a` specify an alernative start address'

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-f` specify the binary file to upload'

* `-p` specify an alternative port for the TCP connection on the QEMU instance. Mutually exclusive
  with `-S`.

* `-R` specify the number of connection attempts before giving up.

* `-r` specify the name or address of the remote host running the QEMU instance. Mutually exclusive
  with `-S`.

* `-S` specify a connection string to the remote host running the QEMU instance, _e.g._
  `tcp:localhost:8004` or `unix:/tmp/socket`. Mutually exclusive with `-r` and `-p`.
* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

### Examples

With the following examples:

`-chardev socket,id=spidev,host=localhost,port=8004,server=on,wait=off -global ot-spi_device.chardev=spidev` has been
added to the QEMU command line to create a TCP chardev and connect it the SPI Device backend. See the [SPI Device](spi_device.md) documentation for details.

* Upload a bootstrap binary
  ````sh
  ./scripts/opentitan/spidevflash.py -f test_bootstrap_virtual_sim_dv+manifest.bin
  ````
