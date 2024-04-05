# Darjeeling LifeCycle Controller over DTM

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
| | JTAG server |---->| DTM |---->| ot_dm_tl |====D====| LC Ctrl |====P====| Hart |       |
| +-------------+     +-----+     +----------+         +---------+         +------+       |
|                                                                                     QEMU|
+-----------------------------------------------------------------------------------------+
```

where:
  `P` is the private OT bus
  `D` is the debug bus

QEMU should be started with an option such as `-jtag tcp::3335` so that the JTAG server is
instantiated and listen for incoming connection on TCP port 3335.

#### macOS

If you want to avoid the boring pop-up window from macOS
```
Do you want the application “qemu-system-riscv32” to accept incoming network connections?
```
restrict the listening interfaces to the localhost with `-jtag tcp:localhost:3335` as QEMU defaults
to listening on all interfaces, _i.e._ 0.0.0.0

## Communicating with JTAG server and Life Cycle controller using Python

OpenTitan implementation provides JTAG/DTM/DMI/Mailbox stack available as Python modules:

* jtag/tap module is available from `scripts/jtag` directory
* dtm/dmi and jtag mailbox modules are available from `scripts/opentitan` directory

Python snippet to create a communication channel with the VM JTAG mailbox:

````py
from socket import create_connection
from jtag.bitbang import JtagBitbangController
from jtag.jtag import JtagEngine
from ot.dtm import DebugTransportModule
from ot.lc_ctrl.lcdmi import LifeCycleController

sock = create_connection(('localhost', 3335), timeout=1.0)
ctrl = JtagBitbangController(sock)
eng = JtagEngine(ctrl)
ctrl.tap_reset(True)
dtm = DebugTransportModule(eng, 5)  # IR length depends on the actual machine
version, abits = dtm['dtmcs'].dmi_version, dtm['dtmcs'].abits
print(f'DTM: v{version[0]}.{version[1]}, {abits} bits')
dtm['dtmcs'].dmireset()
lc_ctrl = LifeCycleController(dtm, 0x3000 >> 2)

# See LifeCycleController for LC controller communication API
````
