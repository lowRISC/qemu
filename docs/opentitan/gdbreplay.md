# `gdbreplay.py`

`gdbreplay.py` uses QEMU log files containing `exec` messages to help a GDB client to replay
execution of a guest application.

It parses QEMU log files, and creates a GDB server serving GDB remote requests. GDB client can
add breakpoints and follow executed instructions by the guest, stepping or running till a
breakpoint is encountered. It is possible to run execution backward.

Supported GDB commands:

 * step/step instruction
 * reverse step/step instruction
 * continue
 * hardware breakpoint
 * memory dump (code only), requires to provide the application(s) with the help of ELF or binary
   files

QEMU traces do not contain register values so it is not possible to observe any register content or
data.

## Usage

````text
usage: gdbreplay.py [-h] [-t LOG] [-e ELF] [-a ADDRESS] [-b BIN] [-g GDB] [-v] [-d]

QEMU GDB replay.

options:
  -h, --help            show this help message and exit
  -t LOG, --trace LOG   QEMU execution trace log
  -e ELF, --elf ELF     ELF application
  -a ADDRESS, --address ADDRESS
                        Address to load each specified binary
  -b BIN, --bin BIN     Binary application
  -g GDB, --gdb GDB     GDB server (default to localhost:3333)
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

### Arguments

* `-a` specify an address where to load the matching RAW binary application, see `-b` option for
  details. May be repeated.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-b` specify a RAW binary application. May be repeated. For each `-b` argument, there should be
  a matching `-a` argument.

* `-e` specify a ELF application to load. May be repeated.

* `-g` specify an alternative interface/port to listen for GDB client

* `-t` specify the QEMU log to parse for `exec`ution traces (see `-d exec` QEMU option)

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

### Examples

* From one terminal, starts the GDB replay tool
  ````sh
  # Load two ELFs here, one for the ROM, one for the application to debug
  ./scripts/opentitan/gdbreplay.py -vv -t qemu.log \
      -e test_rom_fpga_cw310.elf -e csrng_kat_test_prog_fpga_cw310.elf
  ````

* From another terminal, starts a regular GDB client
  ````sh
  riscv64-unknown-elf-gdb
  ````
  then
  ````
  # Load the ELF file on GDB side so that it knows the symbols
  file csrng_kat_test_prog_fpga_cw310.elf
  # Connect to gdbreplay
  target remote :3333
  # Add a HW breakpoint to the entry function of the test
  hb test_main
  # Execute till breakpoint
  c
  ...
  # Move execution back to the `test_main` caller
  rsi
  # Move execution point to the very first instruction
  rc
  ````
