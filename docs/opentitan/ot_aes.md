# OpenTitan AES

* `-global ot-aes.fast-mode=false` can be used to better emulate AES HW IP, as some OT tests expect
  the Ibex core to execute while the HW is performing AES rounds. Without this option, the virtual
  HW may only give back execution to the vCPU once the AES operation is complete, which make those
  OT tests to fail. Disabling fast mode better emulates the HW to the expense of higher AES latency
  and throughput.
