# OpenTitan SoC Debug controller

SoC debug controller manages SoC debug policies based on external signals - such as GPIO, Power
Manager states and LifeCycle states, the later being defined from the OTP content. If no OTP image
is provided, or a RAW (blank) image is provided, or if the OTP image defines a LifeCycle in any of
TEST* or RMA states, a Darjeeling machine that features a SoC Debug controller may enter the DFT
("Debug For Test") execution mode, where the Ibex core may not resume execution till a JTAG debugger
triggers it.

To force QEMU to execute an application despite this feature, bypassing the DFT mode, use
`-global ot-socdbg_ctrl.dft-ignore=on` QEMU option.
