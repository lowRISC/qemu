# `uartmux.py`

`uartmux.py` is a tiny stream wrapper to help dealing with multiple QEMU output streams, typically multiple virtual UARTs.

## Usage

````text
usage: uartmux.py [-h] [-i IFACE] [-p PORT] [-c CHANNEL] [-v] [-d] [name ...]

QEMU UART muxer.

positional arguments:
  name                  assign name to input connection

options:
  -h, --help            show this help message and exit
  -i IFACE, --iface IFACE
                        specify TCP interface to listen to (default: localhost)
  -p PORT, --port PORT  specify TCP port to listen to (default: 9000)
  -c CHANNEL, --channel CHANNEL
                        expected comm channel count(default: 3)
  -s SEPARATOR, --separator SEPARATOR
                        repeat separator between each session
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

`uartmux.py` may be used with QEMU character devices, _i.e._ `chardev` defined as network streams,
such as `-chardev socket`.

`uartmux.py` listens on multiple input streams, and prints them one line at a time, colorizing each
stream with a different color when an ANSI terminal is used.

It enables a coherent output log when multiple virtual UARTs are emitting at once.

### Arguments

* `name` optional name(s) for prexifing each log message line with the known channel name
* `-c` / `--channel` how many input streams are expected, in order to assign the same ANSI color
  to the same input stream across subsequence QEMU sessions. Note that if not defined, default to
  the count of defined names
* `-d` / `--debug` only useful to debug the script, reports any Python traceback to the standard
  error stream.
* `-i` / `--iface` select an alternative interface for listening on, default to localhost
* `-p` / `--port` select an altenative TCP port for listening on, default to 9000
* `-s` / `--separator` emit this separator between each detected QEMU session
* `-v` / `--verbose` can be repeated to increase verbosity of the script, mostly for debug purpose.

### Example

QEMU using two virtual UART output identified as `uart0` and `uart1`:

* run `uartmux.py uart0 uart1` in one terminal
* run QEMU in another terminal with the following option switches:
  ```
  -chardev socket,id=uart0,host=127.0.0.1,port=9000
  -chardev socket,id=uart1,host=127.0.0.1,port=9000
  ```
