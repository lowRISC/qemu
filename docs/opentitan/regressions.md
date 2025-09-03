# OpenTitan regressions

The `run-bazel-tests.sh` script and accompanying CI workflow allow us to run
regression tests against the OpenTitan repository. Currently only Earlgrey tests
are supported.

With a checkout of OpenTitan, the script can be run like this:

```sh
./scripts/opentitan/run-bazel-tests.sh path/to/opentitan path/to/qemu
```

The script will execute all QEMU-compatible tests using QEMU as it was built
at the given path. The test results will be compared against two lists checked
into this repository:

* `scripts/opentitan/tests-passing.txt`
* `scripts/opentitan/tests-flaky.txt`

All tests in `tests-passing.txt` are expected to pass. Failures indicate a
regression in either QEMU or Earlgrey. The script will fail if there is a
mismatch between the tests that we expect to pass and the actual results.

Some tests may be flaky and pass or fail on different runs of the same QEMU
and OpenTitan checkouts. These tests can be added to the `tests-flaky.txt` list
to cause the script to ignore them.
