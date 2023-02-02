# IbexDemo on Arty A7

Ibex Demo is a very lightweight platform that demonstrate the Ibex core features.

* Hello Word application
````sh
qemu-system-riscv32 -M ibexdemo -nographic -kernel .../hello_world/demo
````

* Mandelbrot demo
````sh
qemu-system-riscv32 -M ibexdemo -display <gtk|cocoa> -kernel .../demo/lcd_st7735/lcd_st7735
````

## Useful execution options

#### vCPU

* `-icount N` reduces the execution speed of the vCPU (Ibex core) to 1GHz >> N

* `-cpu lowrisc-ibex,x-zbr=true` can be used to force enable (IbexDemo platform) the Zbr
  experimental-and-deprecated RISC-V bitmap extension for CRC32 extension.
