# Darjeeling JTAG Mailbox

The JTAG mailbox host side (responder) is connected to the private OT address space, while the
system side (requester) is connected to a Debug Tile Link bus.

## Communicating with the JTAG Mailbox through a JTAG connection

In QEMU, a bridge between the Debug Transport Module (DTM) and the JTAG Mailbox is implemented
as Debug Module bridge.

```
+----------------+
| Host (OpenOCD) |
+----------------+
      |
      | TCP connection ("bitbang mode")
      |
+-----|-----------------------------------------------------------------------------------+
|     v                                                                                   |
| +-------------+     +-----+     +----------+         +---------+         +------+       |
| | JTAG server |---->| DTM |---->| ot_dm_tl |====D====|S> MBX <H|====P====| Hart |       |
| +-------------+     +-----+     +----------+         +---------+         +------+       |
|                                                                                     QEMU|
+-----------------------------------------------------------------------------------------+
```

where:
  `P` is the private OT bus
  `D` is the debug bus

QEMU should be started with an option such as:
`-chardev socket,id=taprbb,host=localhost,port=3335,server=on,wait=off` so that the JTAG server is
instantiated and listens for incoming connection on TCP port 3335.

## Communicating with JTAG server and JTAG MailBox using Python

OpenTitan implementation provides JTAG/DTM/DMI/Mailbox stack available as Python modules:

* jtag/tap module is available from `python/qemu/jtag` directory
* dtm/dmi and jtag mailbox modules are available from `scripts/opentitan` directory

Python snippet to create a communication channel with the VM JTAG mailbox:

````py
from socket import create_connection
from jtag.bitbang import JtagBitbangController
from jtag.jtag import JtagEngine
from ot.dtm import DebugTransportModule
from ot.mailbox.jtag import JtagMbox

sock = create_connection(('localhost', 3335), timeout=1.0)
ctrl = JtagBitbangController(sock)
eng = JtagEngine(ctrl)
ctrl.tap_reset(True)
dtm = DebugTransportModule(eng, 5)  # IR length depends on the actual machine
version, abits = dtm['dtmcs'].dmi_version, dtm['dtmcs'].abits
print(f'DTM: v{version[0]}.{version[1]}, {abits} bits')
dtm['dtmcs'].dmireset()
mbj = JtagMbox(dtm, 0x2200 >> 2)

# See ot.mailbox.sysmbox.SysMbox for mailbox communication API
````

## Communicating with JTAG server using OpenOCD

It is possible from OpenOCD running a host to connect to the embedded JTAG server using the
`remote_bitbang` protocol, using a configuration script such as

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

From here, it is possible reach the JTAG dmi through the DMI device:

```tcl
# Mailbox system register addresses
set MBX_SYS_BASE 0x200

set SYS_INTR_MSG_ADDR $($MBX_SYS_BASE + 0x0)
set SYS_INTR_MSG_DATA $($MBX_SYS_BASE + 0x1)
set SYS_CONTROL       $($MBX_SYS_BASE + 0x2)
set SYS_STATUS        $($MBX_SYS_BASE + 0x3)
set SYS_WRITE_DATA    $($MBX_SYS_BASE + 0x4)
set SYS_READ_DATA     $($MBX_SYS_BASE + 0x5)

# Mailbox system register bits
set SYS_CONTROL_ABORT      0x1
set SYS_CONTROL_SYS_INT_EN 0x2
set SYS_CONTROL_GO         0x80000000

set SYS_STATUS_BUSY        0x1
set SYS_STATUS_INT         0x2
set SYS_STATUS_ERROR       0x4
set SYS_STATUS_READY       0x80000000

# read the STATUS register
dmi_read $SYS_STATUS

# write the GO bit of the control register
dmi_write $SYS_CONTROL $SYS_CONTROL_GO
```

Unfortunalely the current version of OpenOCD (v0.12+) does not support a JTAG-DMI communication if
no RISC-V DM module is connected to the DMI device, _i.e._

```
+----------------+
| Host (OpenOCD) |
+----------------+
      |
      | TCP connection ("bitbang mode")
      |
+-----|-----------------------------------------------------------------------------------+
|     v                                                                                   |
| +-------------+     +-----+     +----------+         +---------+         +------+       |
| | JTAG server |---->| DTM |-+-->| ot_dm_tl |====D====|S> MBX <H|===++=P==| Hart |       |
| +-------------+     +-----+ |   +----------+         +---------+   ||    +------+       |
|                             |                                      ||                   |
|                             |   +----+     +-------- +             ||                   |
|                              -->| DM |---->| PulpDM  |==============                    |
|                                 +----+     +---------+                                  |
|                                            | ROM/RAM |                                  |
|                                            +---------+                                  |
|                                                                                         |
|                                                                                     QEMU|
+-----------------------------------------------------------------------------------------+
```

If no RISC-V compatible DM is detected at `$DM_BASE`, OpenOCD enters an infinite loop seeking a
valid DM module.

DM / PulpDM should be part of an upcoming delivery. Meanwhile it is required to tweak OpenOCD so
that it accepts staying connected to the DMI even if it cannot locate a valud DM:

```diff
diff --git a/src/target/riscv/riscv-013.c b/src/target/riscv/riscv-013.c
index 2ab28deac..0e84418d9 100644
--- a/src/target/riscv/riscv-013.c
+++ b/src/target/riscv/riscv-013.c
@@ -1882,6 +1882,8 @@ static int examine(struct target *target)
 		return ERROR_FAIL;
 	}
 
+	target->state = TARGET_UNAVAILABLE;
+	return ERROR_OK;
 	/* Reset the Debug Module. */
 	dm013_info_t *dm = get_dm(target);
 	if (!dm)
diff --git a/src/target/riscv/riscv.c b/src/target/riscv/riscv.c
index 5bae01d5f..786f2520a 100644
--- a/src/target/riscv/riscv.c
+++ b/src/target/riscv/riscv.c
@@ -3167,6 +3167,8 @@ int riscv_openocd_poll(struct target *target)
 {
 	LOG_TARGET_DEBUG_IO(target, "Polling all harts.");
 
+	return ERROR_OK;
+
 	struct list_head *targets;
 
 	LIST_HEAD(single_target_list);
```

## Communicating with the JTAG Mailbox through a DevProxy connection

DevProxy is a communication tool that enables to control QEMU devices from a remote host, over a
TCP connection.

It can access QEMU `SysBusDevice` through their `MemoryRegion` API, intercepts or generate IRQs on
those devices, intercepts accesses to plain RAM region and read or modify their content. See the
[devproxy.md](devproxy.md) document for information about the DevProxy communication protocol and
supported features.

The `devproxy.py` Python module implements the DevProxy protocol and can be used on a host to
remotely control selected devices in the QEMU machine. It includes support for the Mailbox devices
for both the Host and System interfaces.

```
+--------------------+
| Host (Python, ...) |
+--------------------+
      |
      | TCP connection
      |
+-----|-----------------------------------------+
|     v                                         |
| +-----------------+                           |
| | DevProxy server |------                     |
| +-----------------+      |                    |
|      |                   |                    |
|     CTN               Private                 |
|      |                   |                    |
|      |    +---------+    |                    |
|      +--->|S> MBX <H|<---+                    |
|      |    +---------+    |    +------+        |
|      |                   +---<| Hart |        |
|      |    +---------+    |    +------+        |
|      +--->|S> MBX <H|<---+                    |
|      |    +---------+    |    +---------+     |
|      |                   +--->| MBX RAM |     |
|      |    +---------+    |    +---------+     |
|      +--->|S> MBX <H|<---+                    |
|      |    +---------+    |    +-----+         |
|      |                   +--->| RAM |         |
|      |                   |    +-----+         |
|     ...                  |                    |
|      |    +---------+    |                    |
|      +--->|   IPI   |    |                    |
|      |    +---------+    |                    |
|     ...                 ...                   |
|                                           QEMU|
+-----------------------------------------------+
```

QEMU should be started with a option pair to enable communication with the DevProxy server:
* `-chardev socket,id=devproxy,host=localhost,port=8003,server=on,wait=off
* `-global ot-dev_proxy.chardev=devproxy`

Subsequently, a Python script importing the `devproxy.py` module can be used to communicate with
the JTAG mailbox.

Note: `devproxy.py` needs to be found within the Python path, using for example
```sh
exprot PYTHONPATH=${QEMU_SOURCE_PATH}/scripts/opentitan
```

### Troubleshooting

See the [Troubleshooting](jtag-dm.md#troubleshooting) section for details.
