# `dtm.py`

`dtm.py` checks that the JTAG/DTM/DM stack is up and running and demonstrates how to use the Debug
Module to access the Ibex core.

## Usage

````text
usage: dtm.py [-h] [-H HOST] [-P PORT] [-Q] [-t] [-l IR_LENGTH] [-b BASE] [-I]
              [-c CSR] [-C CSR_CHECK] [-x] [-X] [-a ADDRESS] [-m {read,write}]
              [-s SIZE] [-f FILE] [-e ELF] [-F] [-v] [-d]

Debug Transport Module tiny demo

options:
  -h, --help            show this help message and exit

Virtual machine:
  -H HOST, --host HOST  JTAG host (default: localhost)
  -P PORT, --port PORT  JTAG port, default: 3335
  -t, --terminate       terminate QEMU when done

DMI:
  -l IR_LENGTH, --ir-length IR_LENGTH
                        bit length of the IR register (default: 5)
  -b BASE, --base BASE  define DMI base address (default: 0x0)

Info:
  -I, --info            report JTAG ID code and DTM configuration
  -c CSR, --csr CSR     read CSR value from hart
  -C CSR_CHECK, --csr-check CSR_CHECK
                        check CSR value matches

Actions:
  -x, --execute         update the PC from a loaded ELF file
  -X, --no-exec         does not resume hart execution

Memory:
  -a ADDRESS, --address ADDRESS
                        address of the first byte to access
  -m {read,write}, --mem {read,write}
                        access memory using System Bus
  -s SIZE, --size SIZE  size in bytes of memory to access
  -f FILE, --file FILE  file to read/write data for memory access
  -e ELF, --elf ELF     load ELF file into memory
  -F, --fast-mode       do not check system bus status while transfering

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

### Arguments

* `-a` specify the memory address where data is loaded or stored. Useful with the `--mem` option.
  See also the `--size` option. Note that only 32-bit aligned addresses are supported for now.

* `-b` specify the DMI base address for the RISC-V Debug Module

* `-C` compare a CSR value to the specified value. Requires option `--csr`.

* `-c` read and report a CSR from the Ibex core.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-e` specify an ELF32 application file to upload into memory. See also the `--exec` option.

* `-F` assume System Bus can cope with received data pace. This feature increases transfer data
  rate by bypassing SB status check. However it  may cause the transfer to fail in case System Bus
  becomes busy while data are transfered.

* `-H` specify the address of the QEMU VM.

* `-I` report the JTAG ID code and the DTM configuration.

* `-l` specify the length of the TAP instruction register length.

* `-m <read|write>` specify a memory operation to perform. See also `--address`, `--size` and
  `--file` options. With `read` operation, if no `--file` is specified, the content of the selected
  memory segment is dumped to stdout, with a similar format as the output of `hexdump -C`. If a file
  is supplied, the content of the segment is written as binary data into this file. With `write`
  operation, `--file` argument is mandatory. The content of the binary file is copied into the
  memory, starting at the `--address`. See also the `--elf` option for uploading applications.

* `-P` specify the TCP port of the JTAG server in the QEMU VM, should follow the TCP setting of the
  `-chardev socket,id=taprbb,...` option for invoking QEMU.

* `-s` specify the number of bytes to read from or write to memory. Useful with the `--mem` option.
  See also the `--address` option. This option may be omitted for the `write` memory operation, in
  which case the size of the specified file is used. Note that only sizes multiple of 4-byte are
  supported for now.

* `-t` send QEMU a request for termination when this script exits.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

* `-X` do not attempt to resume normal execution of the hart once DTM operation have been completed.
  This can be useful for example when the QEMU VM is started with `-S` and no application code has
  been loaded in memory: once the DTM operations are completed, the default behavior is to resume
  the hart execution, would start execution code from the current PC and cause an immediate
  exception. The `-x` option can nevertheless be executed, as it is the last action that the script
  performs.

* `-x` execute the loaded ELF application from its entry point. Requires the `--elf` option.
  Application is executed even with `-X` defined.

### Examples

Running QEMU VM with the `-chardev socket,id=taprbb,host=localhost,port=3335,server=on,wait=off`
option:

* Retrieve JTAG/DTM/DM information and `mtvec` CSR value
  ````sh
  ./scripts/opentitan/dtm.py -I -c mtvec
  ````

* Check that the MISA CSR matches the expected Ibex core value:
  ````sh
  ./scripts/opentitan/dtm.py -c misa -C 0x401411ad
  ````

* Load (fast mode) and execute an application
  ````sh
  ./scripts/opentitan/dtm.py -e .../helloworld -x -F
  ````

* Dump a memory segment to stdout
  ````sh
  ./scripts/opentitan/dtm.py -m read -a 0x1000_0080 -s 0x100
  ````

* Upload a file into memory and leave the Ibex core halted
  ````sh
  ./scripts/opentitan/dtm.py -m write -a 0x1000_0000 -f file.dat -X

  ````

