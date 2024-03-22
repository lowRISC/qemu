# `gpiodev.py`

`gpiodev.py` acts as a GPIO CharDev backend to record and verify GPIO updates.

It can be used to run regression tests and verify that the guest emits a predefined sequence of
GPIO updates.

## Usage

````text
usage: gpiodev.py [-h] [-p PORT] [-c CHECK] [-r RECORD] [-e END] [-q] [-s] [-t] [-v] [-d]

GPIO device tiny simulator.

options:
  -h, --help            show this help message and exit
  -p PORT, --port PORT  remote host TCP port (defaults to 8007)
  -c CHECK, --check CHECK
                        input file to check command sequence
  -r RECORD, --record RECORD
                        output file to record command sequence
  -e END, --end END     emit the specified value to trigger remote exit on last received command
  -q, --quit-on-error   exit on first error
  -s, --single          run once: terminate once first remote disconnects
  -t, --log-time        emit time in log messages
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

### Arguments

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-p` specify an alternative TCP port to listen for incoming QEMU CharDev socket requests

* `-c` specify a pre-recorded GPIO backend text file - which only contains ASCII characters and
  compare that incoming GPIO requests against the recorded data

* `-r` specify a file to record incoming GPIO requests

* `-e` emit a pre-defined GPIO input command - sent to the QEMU GPIO CharDev backend - that may be
  recognized by the QEMU guest code as a special marker to end a test sequence. Requires the _check_
  option for detecting the last expected GPIO request.

* `-q` close the QEMU socket connection on first error or mismatched GPIO request when used with
  the _check_ option.

* `-s` quite the script on the first QEMU socket disconnection. Default is to listen for a new QEMU
  CharDev socket connection, restarting from a fresh state.

* `-t` add time to log messages

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

### Exit Status

This script returns 0 if no error has been detected, or 1 if an error has been detected or the
GPIO command stream has not been validated against the check file if any.

### Examples

With the following examples:

`-chardev socket,id=gpio,host=localhost,port=8007 -global ot-gpio-$OTMACHINE.chardev=gpio` has been
added to the QEMU command line to connect the GPIO backend to the incoming socket opened from the
script. See the [GPIO documentation](gpio.md) for details and the role of the `OTMACHINE`
environment variable.

* Record a GPIO request sequence
  ````sh
  ./scripts/opentitan/gpiodev.py -vv -r gpio.txt -s
  ````

* Check a GPIO request sequence against a previously record one, then triggers a VM guest app
  triggered termination by setting GPIO 31 to a high level.
  ````sh
  ./scripts/opentitan/gpiodev.py -vv -c gpio.txt -s -e 0x8000_0000
  ````
