# JTAG and RISC-V Debug Module support for OpenTitan

RISC-V Debug Module is implemented as an alternative to the default QEMU GDB server.

It can be used when low-level features are required, such as prototyping JTAG manufacturing and
provisioning software development, or emulating JTAG Debug Authentication.

This initial implementation of the RISC-V Debug Module and Pulp DM supports the following features:

- Ibex core debugging (RV32 only); not tested with multiple harts
- system bus memory access: read and write guest memory
- guest code debugging
- single stepping
- HW breakpoints
- HW watchpoints

## Communicating with RISC-V DM through a JTAG connection

In QEMU machines for OpenTitan, any Debug Module can register on the Debug Transport Module, which
dispatches requests to Debug Module depending on the received DMI address.

See also [JTAG mailbox](jtagmbx.md) and [Life Controller](lc_ctrl_dmi.md) for other Debug Modules.

```
+----------------+
| Host (OpenOCD) |
+----------------+
      |
      | TCP connection ("bitbang mode")
      |
+-----|-----------------------------------------------------------------------------------+
|     |                                                                             QEMU  |
|     v                                              +---------+                          |
| +-------------+     +-----+     +-----------+      | PULP-DM |         +------+         |
| | JTAG server |---->| DTM |---->| RISC-V DM |<---->+ ------- +<=======>| Hart |         |
| +-------------+     +-----+     +-----------+      | ROM/RAM |    ||   +------+         |
|                                      ||            +---------+    ||                    |
|                                      ||                           ||   +--------------+ |
|                                       ================================>| I/O, RAM, ...| |
|                                                                        +--------------+ |
+-----------------------------------------------------------------------------------------+
```

### Remote bitbang protocol

The JTAG server supports the Remote Bitbang Protocol which is compatible with other tools such as
Spike and [OpenOCD](https://github.com/riscv/riscv-openocd).

QEMU should be started with an option such as `-jtag tcp::3335` so that the JTAG server is
instantiated and listens for incoming connection on TCP port 3335.

#### Remote termination feature

The remote Remote Bitbang Protocol supports a "_quit_" feature which enables the remote client to
trigger the end of execution of the QEMU VM. This feature can be useful to run automated tests for
example.

It is nevertheless possible to disable this feature and ignore the remote exit request so that
multiple JTAG sessions can be run without terminating the QEMU VM.

To disable this feature, use the `quit` option: `-jtag tcp::3335,quit=off`.

#### macOS

If you want to avoid the boring pop-up window from macOS
```
Do you want the application “qemu-system-riscv32” to accept incoming network connections?
```
restrict the listening interfaces to the localhost with `-jtag tcp:localhost:3335` as QEMU defaults
to listening on all interfaces, _i.e._ 0.0.0.0

#### Implementation

* JTAG server is implemented with `jtag/jtag_bitbang.c`
* DTM is implemented with `hw/riscv/dtm.c`
* RISC-V DM is implemented with `hw/riscv/dm.c`
* Pulp-DM is implemented with `hw/misc/pulp_rv_dm.c`

See also other supported DM modules on OpenTitan:

* [JTAG mailbox DMI](jtagmbx.md)
* [LifeCycle DMI](lc_ctrl_dmi.md)

## Communicating with JTAG server using OpenOCD

OpenOCD running on a host can connect to the embedded JTAG server, using a configuration script such
as

```tcl
adapter driver remote_bitbang
remote_bitbang host localhost
remote_bitbang port 3335

transport select jtag

set _CHIPNAME riscv
# part 1, ver 0, lowRISC; actual TAP id depends on the QEMU machine
set _CPUTAPID 0x00011cdf

jtag newtap $_CHIPNAME cpu -irlen 5 -expected-id $_CPUTAPID

# Hart debug base also depends on the QEMU machine
set DM_BASE 0x00

target create $_CHIPNAME riscv -chain-position $_TARGETNAME rtos hwthread -dbgbase $DM_BASE
```

## Connecting GDB to OpenOCD

In another terminal, `riscv{32,64}-unknown-elf-gdb` can be used to connect to OpenOCD:

```
# 64-bit version support 32-bit targets
riscv64-unknown-elf-gdb
```

A basic `$HOME/.gdbinit` as the following should connect GDB to the running OpenOCD instance:
```
target remote :3333
```
