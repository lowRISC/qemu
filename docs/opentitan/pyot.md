# `pyot.py`

`pyot.py` is a tool to run OpenTitan tests on QEMU and report results.

## Usage

````text
usage: pyot.py [-h] [-c JSON] [-w CSV] [-k SECONDS] [-v] [-d] [-q QEMU]
               [-Q OPTS] [-m MACHINE] [-p DEVICE] [-L LOG_FILE] [-M LOG]
               [-t TRACE] [-i N] [-r ELF] [-O RAW] [-o VMEM] [-f RAW] [-x file]
               [-b file]

OpenTitan test sequencer

options:
  -h, --help            show this help message and exit
  -c JSON, --config JSON
                        path to configuration file
  -w CSV, --result CSV  path to output result file
  -k SECONDS, --timeout SECONDS
                        exit after the specified seconds (default: 60 secs)
  -F TEST, --filter TEST
                        Only run tests whose filename matches any defined filter (may be repeated)
  -K, --keep-tmp        Do not automatically remove temporary files and dirs on exit
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode

Virtual machine:
  -q QEMU, --qemu QEMU  path to qemu application (default: build/qemu-system-riscv32)
  -Q OPTS, --opts OPTS  QEMU verbatim option (can be repeated)
  -m MACHINE, --machine MACHINE
                        virtual machine (default to ot-earlgrey)
  -p DEVICE, --device DEVICE
                        serial port device name (default to localhost:8000)
  -L LOG_FILE, --log_file LOG_FILE
                        log file for trace and log messages
  -M LOG, --log LOG     log message types
  -t TRACE, --trace TRACE
                        trace event definition file
  -i N, --icount N      virtual instruction counter with 2^N clock ticks per inst.
  -s, --singlestep      enable "single stepping" QEMU execution mode

Files:
  -r ELF, --rom ELF     ROM ELF file
  -O RAW, --otp-raw RAW
                        OTP image file
  -o VMEM, --otp VMEM   OTP VMEM file
  -f RAW, --flash RAW   embedded Flash image file
  -x file, --exec file  rom extension or application
  -b file, --boot file  bootloader 0 file
````

This tool may be used in two ways, which can be combined:

* From the command line, it is possible to run a QEMU test session for one application,
* Using a JSON configuration file, it is possible to run several QEMU test sessions for each
  specified test in the configuration file. This mode is enabled when a JSON config file is
  specified.

### Arguments

* `-c` / `--config` specify a (H)JSON configuration file, see the
  [Configuration](#Configurationfile) section for details.
* `-w` / `--result` specify an output CSV report file where the result of all the QEMU sessions,
  one per test, are reported.
* `-k` / `--timeout` define the maximal duration of each QEMU session. QEMU is terminated or killed
  after this delay if the executed test has not completed in time.
* `-F` / `--filter` when used, only tests whose filenames match one of the selected filter are
  considered. This option only applies to tests enumerated from the configuration file.
* `-K` / `--keep-tmp` do not automatically remove temporary files and directories on exit. The user
  is in charge of discarding any generated files and directories after execution. The paths to the
  generated items are emitted as warning messages.
* `-v` / `--verbose` can be repeated to increase verbosity of the script, mostly for debug purpose.
* `-d` / `--debug` only useful to debug the script, reports any Python traceback to the standard
  error stream.

### Virtual machine options

* `-q` / `--qemu` specify an alternative path to the QEMU application.
* `-Q` / `--opts` add a single QEMU option forwarded verbatim to QEMU (no check is performed)
   * Note that it is easier to use the special option marker `--` to append many options to QEMU:
     any argument after this marker is forwarded verbatim to QEMU.
* `-m` / `--machine` specify the kind of virtual machine to run.
* `-p` / `--device` specify an alternative TCP communication channel. This option should only be
  used if the default channel is already in use.
* `-L` / `--log_file` specify the log file for trace and log messages from QEMU.
* `-M` / `--log` specify which log message types should be logged; most useful types are:
  * `in_asm` for guest instruction disassembly,
  * `unimp` for uimplemented guest features,
  * `int` for guest interrupts and exceptions,
  * `guest_errors` for unexpected guest behavior,
  * `exec` for guest execution stream (caution: highly verbose).
* `-t` / `--trace` trace event definition file. To obtain a list of available traces, invoke QEMU
  with `-trace help` option
* `-i` / `--icount` to specify virtual instruction counter with 2^N clock ticks per instruction.
  This option if often used with two specific values:
   * `-i 0` can be used to improve time synchronisation between the virtual CPU and the virtual HW:
     as many OpenTitan tests rely on specific CPU clock counts for the HW to complete some action,
     running QEMU without this option tends to favor CPU execution speed over HW emulation. With
     this option, the vCPU is better synchronized, trying to emulate a 1GHz-clock vCPU.
   * `-i 6` can be used to slow down vCPU virtual clock to a ~10-15MHz clock pace, which better
     matches the expected FPGA-based lowRISC CPU.
  Note that this option slows down the execution of guest applications.
* `-s` / `--singlestep` enable QEMU "single stepping" mode.

### File options:

*  `-r` / `--rom` specify a ROM ELF file. Without a ROM file, it is unlikely to start up any regular
   application since the emulated lowRISC vCPU is preconfigured with a locked PMP, as the real HW.
   When no ROM is specified, test applications are executed immediately, as a replacement of the ROM
   executable.
*  `-O` / `--otp-raw` specify a RAW image file for OTP fuses, which can be generated with the
   [`otpconv.py`](otpconf.md) tool. Alternatively, see the `-o` option.
*  `-o` / ` --otp` specify an OTP VMEM file. This option is mutually exclusive with the `-O` option.
   This script takes care of calling [`otpconv.py`](otpconf.md) to generate a temporary OTP file
   that is discarded when this script exits.
*  `-f` / `--flash` specify a RAW image file that stores the embedded Flash content, which can be
   generated with the [`flashgen.py`](flashgen.md) tool. Alternatively, see the `-x` option.
*  `-x` / ` --exec` specify a ROM extension, an application or a test to execute. This option is
   mutually exclusive with the `-f` option. This script takes care of calling
   [`flashgen.py`](flashgen.md) to generate a temporary flash file that is discarded when the
   application has been run.
*  `-b` / ` --boot`  specify a bootloader 0 file that can be added to the flash image file when
   a ROM extension file is specified with the `-x` option. This option is mutually exclusive with
   the `-f` option.


## Configuration file

### Legacy JSON syntax

Sample config for running OpenTitan tests:
````json
{
    "aliases": {
        "BASEDIR": "${OT_DIR}/bazel-out/k8-fastbuild/bin"
    },

    "testdir": "${BASEDIR}/sw",

    "default": {
        "rom": "${BASEDIR}/sw/device/lib/testing/test_rom/test_rom_fpga_cw310.elf",
        "otp": "${BASEDIR}/hw/ip/otp_ctrl/data/img_rma.24.vmem",
        "timeout": 3,
        "icount": 6
    },

    "include" : [
        "**/*.fake_rsa_test_key_0.signed.bin"
    ],

    "exclude" : [
        "alert_handler_*",
        "ast_clk_out_*",
        "clkmgr_off_*",
        "i2c_*",
        "manuf_cp_*",
        "sensor_ctrl_*",
        "spi_device_*",
        "spi_passthru_*",
        "usbdev_*"
    ],

    "suffixes": [
        "_prog_fpga_cw310"
    ],

    "tests": {
        "aes_idle_test": {
            "opts": ["-global", "ot-aes.fast-mode=false"]
        },
        "alert_handler_lpg_reset_toggle_test" : {
            "timeout": 10
        },
        "boot_data_functest": {
            "icount": 1
        },
        "otbn_rsa_test": {
            "timeout": 5
        },
        "mod_exp_otbn_functest_hardcoded": {
            "icount": 0
        },
        "ecdsa_p256_functest": {
            "timeout": 5
        },
        "ecdh_p256_functest": {
            "timeout": 5
        },
        "entropy_src_csrng_test": {
            "icount": ""
        },
        "csrng_edn_concurrency_test": {
            "timeout": 15
        },
        "csrng_smoketest": {
            "timeout": 5
        }
    }
}
````

### HJSON syntax

HJSON is a more user-friendly syntax that JSON. If the `hjson` module is available on the platform,
this script uses it as a replacement for the system JSON module, hence supporting the improved
syntax for configuration. It is encouraged to install the dependency-less HJSON module using a
command such as `pip3 install hjson`.

Sample config for running OpenTitan tests:
````hjson
{
    aliases:
    {
        BASEDIR: ${OT_DIR}/bazel-out/k8-fastbuild/bin
    }
    testdir:  ${BASEDIR}/sw
    default:
    {
        rom: ${BASEDIR}/sw/device/lib/testing/test_rom/test_rom_fpga_cw310.elf
        otp: ${BASEDIR}/hw/ip/otp_ctrl/data/img_rma.24.vmem
        timeout: 3
        icount: 6
    }
    include:
    [
        **/*.fake_rsa_test_key_0.signed.bin
    ]
    #include_from:
    #[
    #    ${BASEDIR}/../test_list.txt
    #]
    exclude:
    [
        alert_handler_*
        ast_clk_out_*
        clkmgr_off_*
        i2c_*
        manuf_cp_*
        sensor_ctrl_*
        spi_device_*
        spi_passthru_*
        usbdev_*
    ]
    suffixes:
    [
        _prog_fpga_cw310
    ]
    tests:
    {
        aes_idle_test:
        {
            opts:
            [
                -global
                ot-aes.fast-mode=false
            ]
        }
        alert_handler_lpg_reset_toggle_test:
        {
            timeout: 10
        }
        boot_data_functest:
        {
            icount: 1
        }
        otbn_rsa_test:
        {
            timeout: 5
        }
        ecdsa_p256_functest:
        {
            timeout: 5
        }
        ecdh_p256_functest:
        {
            timeout: 5
        }
        entropy_src_csrng_test:
        {
            icount: ""
        }
        csrng_edn_concurrency_test:
        {
            timeout: 15
        }
        csrng_smoketest:
        {
            timeout: 5
        }
    }
}
````

Sample config for running some non-OpenTitan tests:
````hjson
{
    aliases:
    {
        basedir: ${TEST_DIR}
    }
    testdir: ${BASEDIR}/target/riscv32imc-unknown-none-elf/debug
    default:
    {
        timeout:  3
        machine:  ot-earlgrey,no_epmp_cfg=true
    }
    # It would be nice if ELF files could have an .elf extension w/ Cargo
    # Let's include everything and exclude non-ELF files
    include:
    [
        *
    ]
    exclude:
    [
        *.d
        *.rlib
        # void dummy app never completes
        void
    ]
    tests:
    {
        edn-lim-test:
        {
            timeout:  6
        }
        spihost-test:
        {
            # This test needs a specific SPI flash image
            pre:
            [
                qemu-img create -f raw @{}/spiflash.raw 16M
                dd if=${BASEDIR}/data/spihost/content.txt of=@{}/spiflash.raw conv=notrunc
            ]
            opts:
            [
                -drive if=mtd,format=raw,file=spiflash.raw
            ]
            post:
            [
                rm -r @{}/spiflash.raw
            ]
            timeout: 10
        }
        timer-test:
        {
            timeout:  15
        }
        aes-kat-test:
        {
            # This test needs to be driven by a host script
            opts:
            [
                -chardev socket,id=serial1,host=localhost,port=8001,server=on,wait=off
                -serial chardev:serial1
            ]
            with:
            [
                ${BASEDIR}/tools/katcomm.py -t ${BASEDIR}/data/aes/nist/kat/*.rsp&
            ]
            timeout: 30
        }
    }
}
````

### Configuration file sections

* `aliases`
  This section may be used to define string aliases to simplify further definitions.

  Note that the resulting aliases are always uppercased.

  To use an alias, use the `${ALIAS}` syntax. Environment variables may also be used as aliases.
  Two special variables are automatically defined:
  * `${CONFIG}` refers to the path of the configuration file itself,
  * `${TESTDIR}` refers to the default test path (see `testdir` below).

* `testdir`
  This section may be used to define the default path where to look for tests to run.

* `default`
  This section defines the default options to use with all the tests.

  In the `tests` section, it is possible to add or remove options for specific tests.

  The option names are the same ones as the script option switches, please refer to the Usage
  section for details.

* `include`
  This section contains the list of tests to be run.

  Globalisation patterns (`*` and `?`) may be used to match multiple files. Use `**/` to define a
  recursive path.

  It is possible to exclude some tests from this list with the `exclude` and `exclude_from`
  sections.

* `include_from`
  This section contains a list of files defining the tests to be run.

  Each file should be a plain text file with one test application per line.

  `#` can be used to prefix any comments that are therefore ignored. Empty lines are also ignored.

  Aliases can be used in the file paths. If, after resolution of aliases, a test file does not
  start with a leading directory separator, the test filename is assumed to be relative to the path
  of the current include file.

  Note that `include_from` (and `exclude_from`) do not support globalization patterns (`*` and `?`).

* `exclude`
  This section contains the list of tests not to be run.

  Globalisation characters (`*` and `?`) may be used to select files with glob patterns. Use `**/`
  to define a recursive path.

  This section enables the removal of specific tests from the lists that have been built using the
  `include` and `include_from` sections.

* `exclude_from`
  This section contains a list of files defining the tests to exclude.

  The syntax is identical to the `include_from` section.

* `suffixes`
  This section defines shortcut to further reduce the definition and report of test filenames.

  Any test filename ending up with one of the suffixes is automatically stripped. Further test
  configuration in the the `tests` section should omit this suffix. The generated test report also
  omit these suffixes.

* `tests`
  This section enables option customization for each test entry.

  * To add a new option, specify it as in the `default` section.
  * To remove a default option, use an empty value (`""`)

### Special test sections

Each test section may have up to three special subsections that may be used to execute arbitrary
commands:

* `pre` subsection contains commands to execute before QEMU is executed.

  * This subsection may be useful to create some test files, or to start up a server.

* `with` section contains commands to execute as soon as QEMU is started.

  * This subsection may be useful to inject some test patterns over a communication channel into
    the guest program running on the VM

* `post` section contains commands to execute once the QEMU session has completed.

  * This section may be useful to perform some cleanup or post processing analysis once the QEMU
    session is over

Regular or 'synchronous' commands are only executed if all the previous commands have been
succesful. Moreover, commands in the `post` subsection are only executed if the QEMU session has
completed successfully.

Background commands are commands that run in the background till they complete on their own, or
automatically once the QEMU session has completed. Note that `post` commands cannot be defined as
background commands.

To create a background command, use the same syntax as with a shell command, _i.e._ append a `&`
after the last argument of the command to execute.

#### Temporary directories

It is possible to use the special `@{dir_id}/` syntax, where `dir_id` should be a regular identifier.

Whenever such directory placeholder is detected, it is replaced with a temporary directory which is
created the first time the identifier is encountered, and automatically removed along all its
contents when `pyot.py` exits. It is also possible to remove a temporary directory at any time with
a regular `rm -r` command. The temporary directories are not removed between tests, so that content
of temporary directories may be shared/reused across tests.

As a special syntax, the `@{}/` (empty directory identifier) can be used to create a temporary
directory which is automatically removed once the test completes, whatever its completion status
(success or failure).

## Result file sample

````csv
Name,Result,Time,Icount,Error
aes_entropy_test,pass,0.050,6,
aes_functest,pass,0.052,6,
aes_gcm_functest,pass,0.068,6,
aes_gcm_timing_test,fail,0.054,6,CHECK-fail: AES-GCM decryption was not constant-time for different invalid tags
aes_idle_test,pass,0.046,6,
aes_masking_off_test,fail,0.042,6,"CHECK-STATUS-fail: Internal:[""CSR"";22]"
aes_smoketest,pass,0.045,6,
aes_test,pass,0.044,6,
...
````

## Return value

The script returns the error code of the most occuring error, or success (0)

## Examples

* The most typical usage requires a JSON configuration file and produces an output CSV file. `-vv`,
  that is the information log level, should be enough to track execution without getting too many
  log messages.

  ````sh
  ./scripts/opentitan/pyot.py -vv -c pyot.json -w pyot.csv
  ````

  Note that results can be live-tracked from another terminal using a command like the following:

  ````sh
  tail -f pyot.csv
  ````

* Running at single test can be done without any configuration file:

  ````sh
  export BASEDIR=$OT_DIR/bazel-out/k8-fastbuild/bin
  ./scripts/opentitan/pyot.py -vv \
    --rom $BASEDIR/sw/device/lib/testing/test_rom/test_rom_fpga_cw310.elf \
    --otp $BASEDIR/hw/ip/otp_ctrl/data/img_rma.24.vmem \
    --exec $BASEDIR/sw/device/tests/uart_smoketest_prog_fpga_cw310.fake_rsa_test_key_0.signed.bin
  ````

* The full OpenTitan boot flow with production ROM can also be run:

  ````sh
  export BASEDIR=$OT_DIR/bazel-out/k8-fastbuild/bin
  ./scripts/opentitan/pyot.py -vv \
    --rom $BASEDIR/sw/device/silicon_creator/rom/rom_with_fake_keys_fpga_cw310.elf \
    --otp $BASEDIR/hw/ip/otp_ctrl/data/img_rma.24.vmem \
    --exec $BASEDIR/sw/device/silicon_creator/rom_ext/rom_ext_slot_virtual_fpga_cw310.fake_rsa_test_key_0.signed.bin \
    --boot $BASEDIR/sw/device/silicon_owner/bare_metal/bare_metal_slot_virtual_fpga_cw310.fake_rsa_rom_ext_test_key_0.signed.bin
  ````

## Tips

* `-M int` option is quite useful to debug application startup issues.

* `-M in_asm` option only displays the first time that a RISC-V instruction block is translated to
  host code, not each time the instructions are executed. Use `-M exec` to display the actual
  executed instructions. Enabling single stepping is also helpful in this case (with `-s`).
