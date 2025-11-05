## Useful debugging options

### Device log traces

Most OpenTitan virtual devices can emit log traces. To select which traces should be logged, a plain
text file can be used along with QEMU `-trace` option.

To populate this file, the easiest way is to dump all available traces and filter them with a
pattern, for example to get all OpenTitan trace messages:

````sh
qemu-system-riscv32 -trace help | grep -E '^ot_' > ot_trace.log
qemu-system-riscv32 -trace events=ot_trace.log -D qemu.log ...
````

* It is *highly* recommended to use the `-D` option switch when any `-trace` or `-d` (see below) is
selected, to avoid saturating the standard output stream with traces and redirect them into the
specified log file.

### QEMU log traces

QEMU provides another way of logging execution of the virtual machine using the `-d` option. Those
log messages are not tied to a specific device but rather to QEMU features. `-d help` can be used
to enumerate these log features, however the most useful ones are enumerated here:

   * `unimp` reports log messages for unimplemented features, _e.g._ when the vCPU attempts to
     read from or write into a memory mapped device that has not been implemented.
   * `guest_errors` reports log messages of invalid guest software requests, _e.g._ attempts to
     perform an invalid configuration.
   * `int` reports all interruptions *and* exceptions handled by the vCPU. It may be quite verbose
     but also very useful to track down an invalid memory or I/O access for example. This is the
     first option to use if the virtual machine seems to stall on start up.
   * `in_asm` reports the decoded vCPU instructions that are translated by the QEMU TCG, _i.e._ here
     the RISC-V instructions. Note that transcoded instructions are cached and handled by blocks,
     so the flow of transcoded instruction do not exactly reflect the stream of the executed guest
     instruction, e.g. may only appear once in a loop. Use the next log option, `exec`, to get
     more detailed but also much more verbose log traces.
   * `exec` reports the vCPU execution stream.

Those options should be combined with a comma separator, _e.g._ `-d unimp,guest_errors,int`

`in_asm` option may be able to report the name of the guest executed function, as long as the guest
application symbols have been loaded. This is the case when the `-kernel` option is used to load
an ELF non-stripped file. Unfortunately, this feature is not available for guest applications that
are loaded from a raw binary file (`.bin`, `.signed.bin`, ...). However the
[`flashgen.py`](flashgen.md) script implements a workaround for this feature, please refer to this
script for more details.

Finally, a Rust demangler has been added to QEMU, which enables the QEMU integrated disassembler to
emit the demangled names of the Rust symbols for Rust-written guest applications rather than their
mangled versions as stored in the ELF file.
