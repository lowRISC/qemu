# `checkregs.py`

`checkregs.py` checks whether QEMU register definitions for OpenTitan match the generated OpenTitan
`*_regs.h` files. This enables to spot major differences whenever OpenTitan definitions are updated.

Note that only register addresses are checked for now, the bit assignement in each register is not
validated.

## Usage

````text
usage: checkregs.py [-h] [-q dir] [-a] [-k] [-M] [-v] [-d] file [file ...]

Verify register definitions.

positional arguments:
  file                register header file

options:
  -h, --help          show this help message and exit
  -q dir, --qemu dir  QEMU directory to seek
  -a, --all           list all discrepancies
  -k, --keep          keep verifying if QEMU impl. is not found
  -M, --no-map        do not convert regs into QEMU impl. path
  -v, --verbose       increase verbosity
  -d, --debug         enable debug mode
````

### Arguments

* `-a` list all discrepancies rather than only reporting their count.

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-k` keep searching for discrepancies if some error is detected.

* `-M` do not try to map radix of input register files into their QEMU counter part

* `-q dir` the directory to look for OpenTitan devices to check.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.

### Examples

* All Earlgrey register files have been copied into `opentitan/regs-251rc1`

  ````sh
  scripts/opentitan/checkregs.py -k -q hw/opentitan ~/opentitan/regs-251rc1/*.h
  ````
  reports
  ````
  ERROR ot.regs    ot_hmac.c: 1 discrepancies
  ERROR ot.regs    ot_otbn.c: 2 discrepancies
  ERROR ot.regs    ot_otp.c: 69 discrepancies
  ERROR ot.regs    ot_pinmux.c: 585 discrepancies
  ERROR ot.regs    ot_ibex_wrapper.c: 1 discrepancies
  658 differences
  ````
  `pinmux` and `otp` registers in QEMU OT are not implemented as a flat list of registers, which
  explain the large amount of discrepancies.

  ````sh
  scripts/opentitan/checkregs.py -a -k -q hw/opentitan ~/opentitan/regs-251rc1/rv_core_ibex_regs.h -v
  ````
  reports
  ````
  WARNING ot.regs    QEMU ot_ibex_wrapper.c is missing 1 defs
  WARNING ot.regs    .. DV_SIM_WINDOW (0x80)
    ERROR ot.regs    ot_ibex_wrapper.c: 1 discrepancies
  1 differences
  ````
  which is Ok as DV_SIM_WINDOW is not supported on QEMU.
