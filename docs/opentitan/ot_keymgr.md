# OpenTitan Key Manager support

## Properties

- `-global ot-keymgr.disable-flash-seed-check=true` can be used to disable the
data validity check in the Keymgr for loaded flash secrets (the owner and
creator seed). This validity check ensures that the loaded key is not all-zero
or all-one (and thus probably uninitialized). When emulating OpenTitan, it may
be useful to be able to advance using uninitialized keys due to a lack of flash
info splicing, to bypass the need to run through an entire provisioning flow.
  - Note also that the fatal Keymgr alert caused by failing this check should
  not appear for unprovisioned flash if flash scrambling is implemented (and
  enabled). This is because the garbage unscrambled data that is read will not
  pass this check.
