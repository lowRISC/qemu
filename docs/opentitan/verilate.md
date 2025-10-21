# `verilate.py`

`verilate.py` is a Verilator wrapper tool to run unit tests on Verilator simulation environment. It
is designed as a companion tool to help writing QEMU OpenTitan devices, running the very same
binaries as QEMU and comparing the outcome of each simulation environment.

## Usage

````text
usage: verilate.py [-h] [-V VERILATOR] [-R FILE] [-M FILE] [-F FILE] [-O VMEM]
                   [-K] [-D TMP_DIR] [-c CFG] [-a PREFIX] [-C CYCLES] [-I]
                   [-k SECONDS] [-l] [-P FILE] [-w] [-x] [-v] [-d] [-G]
                   [ELF ...]

Verilator wrapper.

options:
  -h, --help            show this help message and exit

Files:
  ELF                   application file, may be repeated
  -V, --verilator VERILATOR
                        Verilator executable
  -R, --rom FILE        ROM file, may be repeated
  -M, --ram FILE        RAM file, may be repeated
  -F, --flash FILE      eFlash file, may be repeated
  -O, --otp VMEM        OTP file
  -K, --keep-tmp        do not automatically remove temporary files and dirs
                        on exit
  -D, --tmp-dir TMP_DIR
                        store the temporary files in the specified directory
  -c, --config CFG      input QEMU OT config file (required w/ non scrambled
                        ROM images)

Verilator:
  -a, --artifact-name PREFIX
                        set an alternative artifact name (default is derived
                        from the application name)
  -C, --cycles CYCLES   exit after the specified cycles
  -I, --show-init       show initializable devices
  -k, --timeout SECONDS
                        exit after the specified seconds (default: 600.0 secs)
  -l, --link-exec-log   create a symlink to the execution log file
  -P, --profile FILE    enable/manage profile file
  -w, --wave-gen        generate output wave file
  -x, --save-exec-log   save execution log

Extras:
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
  -G, --log-time        show local time in log messages
````

### Arguments

* `ELF` one or more application files to load in Verilator emulated memory devices before starting
  the simulation environment. It can be repeated. ELF metadata are used to discover in which device
  to load each ELF application: ROM, RAM or eFlash devices are supported. If another file input
  format (such as `VMEM`, `HEX`, `BIN`, ...) needs to be used, please refer to options `-R`, `-M`
  and `-F`.

* `-a` / `--artifact` all artifact files (see `-l`, `-x` and `-w`) are named after the application
  name. This option specifies an alternative file prefix for all those artifacts.

* `-C` / `--cycles` abort Verilator execution after the specified count of cycles. See also the `-k`
  option.

* `-c` / `--config` specify a QEMU [configuration file](otcfg.md) from which to read all the
  cryptographic constants. See [`cfggen.py`](cfggen.md) tool to generate such a file. It is required
  to scramble any non-pre scrambled ROM input file.

* `-d` / `--debug` only useful to debug the script, reports any Python traceback to the standard
  error stream.

* `-D` / `--tmp-dir` specify a custom directory for temporary files.

* `-F` specify a file to load into the embedded flash device. This option can be repeated if the
  simulated platform supports multiple embedded flash partitions, in which case the specified files
  are loaded in partition order. See option `-I` for a list of supported devices.

* `-G` / `--log-time` show local time before each logged message

* `-K` / `--keep-tmp` do not automatically remove temporary files and directories on exit. The user
  is in charge of discarding any generated files and directories after execution. The paths to the
  generated items are emitted as warning messages.

* `-k` / `--timeout` abort Verilator execution after the specified time. See also the `-c` option.

* `-I` / `--show-init` show a list of initializable (loadable) devices for the specified Verilator
  environment (see option `-V`).

* `-l` / `--link-log` create a shortcut (a symbolic link) to the Verilator trace log file while
  Verilator is running. This gives a known file name to track while Verilator is executing, so it is
  easy to follow the execution status (such as with `tail -F` command). See also the `-x` option to
  save the resulting trace log file after completion. See also `-a`.

* `-M` / `--ram` specify a file to load into a RAM. This option can be repeated if the simulated
  platform supports multiple RAM devices, in which case the specified files are loaded in RAM order.
  Note that the "order" of RAM depends on each platform. See option `-I` for a list of supported
  devices.

* `-O` / `--otp` specify the image file to load into the OTP controller. This option only supports
  VMEM plain text file with 24-bit data chunks (6 bit ECC + 16 bit data), as produced by the
  OpenTitan tools.

* `-P` / `--profile` create/use a Verilator profile file. Only useful for profile-guided Verilator
  optimization.

* `-R` / `--rom` specify a file to load into a ROM. This option can be repeated if the simulated
  platform supports multiple ROM devices, in which case the specified files are loaded in ROM order.
  See option `-I` for a list of supported devices.

* `-V` / `--verilator` where to find the Verilator executable. This option expects a directory. The
  file structure tree is traversed till a `Vchip_sim_tb` application is found. It is primarily
  designed to locate this application from a Bazel binary tree, such the one produced in OpenTitan
  repository.

* `-v` / `--verbose` can be repeated to increase verbosity of the script, mostly for debug purpose.

* `-x` / `--execution-log` save the generated Verilator trace log file after execution. The file is
  saved even if Verilator is aborted or killed. See also option `-l` for a live view of execution
  traces. See also `-a`.

* `-w` / `--wave` generate a GtkWave-compatible FST wave file, named after the main application.
  See also `-a`.

### Examples

* Show a list of initializable devices:
    ````sh
    scripts/opentitan/verilate.py -V bazel-bin -I
    ````
    output:
    ````
    flash0  0x20000000   512 KiB
    flash1  0x20080000   512 KiB
    otp     0x40000000    16 KiB
    ram     0x10000000   128 KiB
    rom     0x00008000    32 KiB
    ````

* Run a Verilator session with an OpenTitan unit test, using a pre-scrambled ROM file and an
  application in flash:
    ````sh
    scripts/opentitan/verilate.py -V ../ot/bazel-bin -x -O img_rma.24.vmem \
      -R test_rom_sim_verilator.39.scr.vmem -F test_rom_sim_verilator.39.scr.vmem -vv
    ````

* Run a Verilator session with a baremetal test and generate a GtkWave file:
    ````sh
    scripts/opentitan/verilate.py -V ../ot/bazel-bin -c docs/config/opentitan/earlgrey.cfg \
      -x exec.log -O img_rma.24.vmem -vv rom0.elf helloworld -w
    ````
