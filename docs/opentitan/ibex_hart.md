# IBEX Hart

## Supported features

* `RV32I` v2.1 Base Integer Instruction Set with `B` (`Zba`, `Zbb`, `Zbc` and `Zbs`) `C`, `M`
  extensions
* `Zicsr` and `Zifencei` extensions
* `Smepmp` extension (ePMP)
* `Zbr` v0.93 unratified ISA extension (crc32 instructions)

## Unsupported features

* Extensions not defined in RISC-V standard and/or not ratified are not supported (except `Zbr`)
  * `Zbe`, `Zbf`, `Zbp` and `Zbt`
* Ibex NMI
* custom CSRs: `cpuctrlsts`, `secureseed`
* `mret` extension (double fault management)

## Useful options

* `-icount 6` reduces the execution speed of the vCPU (Ibex core) to 1GHz >> 6, _i.e._ ~15MHz,
  which should roughly match the expected speed of the Ibex core running on the CW310 FPGA, which
  is set to 10 MHz. This option is very useful/mandatory to run many OpenTitan tests that rely on
  time or CPU cycle to validate features. Using `-icount` option slows down execution speed though,
  so it is not recommended to use it when the main goal is to develop SW to run on the virtual
  machine. An alternative is to use `-icount shift=auto`, which offers fastest emulation execution,
  while preserving an accurate ratio between the vCPU clock and the virtual devices.

* `-cpu lowrisc-ibex,x-zbr=false` can be used to force disable the Zbr experimental-and-deprecated
  RISC-V bitmap extension for CRC32 extension.
