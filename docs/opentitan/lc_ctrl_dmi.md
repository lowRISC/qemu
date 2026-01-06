# LifeCycle Controller over DTM

## Communicating with the Life Cycle Controller through a JTAG connection

In QEMU, a bridge between the Debug Transport Module (DTM) and the Life Cycle Controller is
implemented as a Debug Module bridge.

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

In Darjeeling, a top-level debug crossbar multiplexes debug access to several devices, including
the Life Cycle Controller. Therefore, there is a single DTM and Debug Module bridge. In the QEMU
implementation of Darjeeling we thus have a single JTAG server, reachable over the `taprbb`
character device. For that machine, QEMU should be started with an option such as:
`-chardev socket,id=taprbb,host=localhost,port=3335,server=on,wait=off` so that the JTAG server is
instantiated and listens for incoming connections on TCP port 3335.

In Earlgrey, we have multiple JTAG TAPs, including one for the Life Cycle Controller. The Life Cycle
Controller instantiates its own DTM and Debug Module bridge. Therefore, in the QEMU implementation
of Earlgrey we use a separate character device for that JTAG server, named instead `taprbb-lc-ctrl`.

## Communicating with JTAG server and Life Cycle controller using Python

OpenTitan implementation provides JTAG/DTM/DMI/Mailbox stack available as Python modules:

* jtag/tap module is available from `python/qemu/jtag` directory
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

### Troubleshooting

See the [Troubleshooting](jtag-dm.md#troubleshooting) section for details.
