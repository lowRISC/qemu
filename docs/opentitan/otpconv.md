# `otpconv.py`

`otpconv.py` generates a QEMU RAW image file which can be loaded by the OpenTitan OTP controller
virtual device.

## Usage

````text
usage: otpconv.py [-h] [-i vmem] [-c regfile] [-l svfile] [-O {lc_arrays,raw}] [-o file] [-e bits]
                  [-b] [-s] [-v] [-d]


Convert a VMEM OTP file into a RAW file.

options:
  -h, --help            show this help message and exit
  -i vmem, --input vmem
                        input VMEM file
  -c regfile, --check regfile
                        decode OTP content w/ otp_ctrl_reg file
  -l svfile, --lc svfile
                        decode OTP lifecycle w/ LC .sv file
  -O {lc_arrays,raw}, --output-type {lc_arrays,raw}
                        type of generated file, defaults to raw
  -o file, --output file
                        output file
  -e bits, --ecc bits   ECC bit count
  -b, --bswap           reverse data byte order (swap endianess)
  -s, --show            dump decoded values to stdout
  -v, --verbose         increase verbosity
  -d, --debug           enable debug mode
````

This script works in three different modes.

1. The user mode generates an OTP image file that can be loaded by the OT OTP controller
2. The debug mode can be used to decode some of the OTP content
3. The developer mode generates C constant arrays that the QEMU virtual machine needs to
   decode the encoded Life Cycle values stored in the OTP fuses. This mode is only useful when OT is
   regenerated.

### Arguments

* `-b` byte-swap in input VMEM words (should work Ok w/o this option)

* `-c` specify the register file, which is only useful to decoce OTP content (see `-s` option)

* `-d` only useful to debug the script, reports any Python traceback to the standard error stream.

* `-e` specify how many bits are used in the VMEM file to store ECC information. Note that ECC
  information is not stored in the QEMU RAW file for now.

* `-l` specify the life cycle system verilog file that defines the encoding of the life cycle
       states. This option is not required to generate a RAW image file.

* `-i` specify the input VMEM file that contains the OTP fuse content.

* `-O format` specify the execution mode. The default mode (`raw`) is used to generate a QEMU RAW
  file. The optional mode (`lc_arrays`) can be used to update QEMU OTP implementation to decode
  encoded life cycle values.

* `-o` specify the output QEMU RAW file to generate with the OTP fuse content.

* `-s` decodes some of the content of the OTP fuse values. This option requires the `-c` option.

* `-v` can be repeated to increase verbosity of the script, mostly for debug purpose.


### Examples

#### User mode

To generate a QEMU RAW image for the virtual OTP controller, here with an RMA OTP configuration:
````sh
scripts/opentitan/otpconv.py -i img_rma.24.vmem -o otp.raw
````

#### Debug mode

````sh
scripts/opentitan/otpconv.py -i img_rma.24.vmem -c otp_ctrl_regs.h -s
````
reports

````
VENDOR_TEST                                    [64] 0...
SCRATCH                                        [56] 0...
VENDOR_TEST_DIGEST                             0
CREATOR_SW_CFG                                 [800] 0000000000000000000000000000000000000000...
CREATOR_SW_CFG_AST_CFG                         [156] 0...
CREATOR_SW_CFG_AST_INIT_EN                     6
CREATOR_SW_CFG_ROM_EXT_SKU                     0
CREATOR_SW_CFG_SIGVERIFY_RSA_MOD_EXP_IBEX_EN   (decoded) True
CREATOR_SW_CFG_SIGVERIFY_RSA_KEY_EN            4b4b4b4b4ba5a5a5
CREATOR_SW_CFG_SIGVERIFY_SPX_EN                8d6c8c17
//...
LIFE_CYCLE                                     [88] e5d7d3b7612eb79df2df9dedbe7f4fffe4efffa5...
LC_TRANSITION_CNT                              [48] d00d8b8a62e8762804055dc45e689b601df2f213...
LC_STATE                                       [40] e5d7d3b7612eb79df2df9dedbe7f4fffe4efffa5...
````

If the LifeCyle definition file is provided:
````sh
scripts/opentitan/otpconv.py -i img_rma.24.vmem -c otp_ctrl_regs.h -s \
    -l hw/ip/lc_ctrl/rtl/lc_ctrl_state_pkg.sv
````

reports

````
VENDOR_TEST                                    [64] 0...
SCRATCH                                        [56] 0...
VENDOR_TEST_DIGEST                             0
CREATOR_SW_CFG                                 [800] 0000000000000000000000000000000000000000...
CREATOR_SW_CFG_AST_CFG                         [156] 0...
CREATOR_SW_CFG_AST_INIT_EN                     6
CREATOR_SW_CFG_ROM_EXT_SKU                     0
CREATOR_SW_CFG_SIGVERIFY_RSA_MOD_EXP_IBEX_EN   (decoded) True
CREATOR_SW_CFG_SIGVERIFY_RSA_KEY_EN            4b4b4b4b4ba5a5a5
CREATOR_SW_CFG_SIGVERIFY_SPX_EN                8d6c8c17
// ...
SECRET2_DIGEST                                 0
LIFE_CYCLE                                     [88] e5d7d3b7612eb79df2df9dedbe7f4fffe4efffa5...
LC_TRANSITION_CNT                              (decoded) 8
LC_STATE                                       (decoded) Rma
````

#### Developer mode

````sh
scripts/opentitan/otpconv.py -i img_rma.24.vmem -c otp_ctrl_regs.h -O lc_arrays \
    -l hw/ip/lc_ctrl/rtl/lc_ctrl_state_pkg.sv -o lc_state.c
````

generates a C definition file like

````c
/* Section auto-generated with otpconv.py script */
enum lc_state {
    LC_STATE_RAW,
    LC_STATE_TESTUNLOCKED0,
    LC_STATE_TESTLOCKED0,
    LC_STATE_TESTUNLOCKED1,
    LC_STATE_TESTLOCKED1,
    LC_STATE_TESTUNLOCKED2,
    LC_STATE_TESTLOCKED2,
    LC_STATE_TESTUNLOCKED3,
    LC_STATE_TESTLOCKED3,
    LC_STATE_TESTUNLOCKED4,
    LC_STATE_TESTLOCKED4,
    LC_STATE_TESTUNLOCKED5,
    LC_STATE_TESTLOCKED5,
    LC_STATE_TESTUNLOCKED6,
    LC_STATE_TESTLOCKED6,
    LC_STATE_TESTUNLOCKED7,
    LC_STATE_DEV,
    LC_STATE_PROD,
    LC_STATE_PRODEND,
    LC_STATE_RMA,
    LC_STATE_SCRAP,
    LC_STATE_POST_TRANSITION,
    LC_STATE_ESCALATE,
    LC_STATE_INVALID,
};

static const char lc_states[21u][40u] = {
    [LC_STATE_RAW] = {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    },
    [LC_STATE_TESTUNLOCKED0] = {
        0xf2u, 0x9fu, 0x2eu, 0xb0u, 0x11u, 0xe2u, 0x90u, 0xc9u, 0x21u, 0x0fu,
        // ...
    },
    // ...
};
````
