# OpenTitan regressions

The `run-bazel-tests.sh` script and accompanying CI workflow allow us to run
regression tests against the OpenTitan repository. Currently only Earlgrey tests
are supported.

With a checkout of OpenTitan, the script can be run like this:

```sh
./scripts/opentitan/run-bazel-tests.sh path/to/opentitan path/to/qemu \
    [execution_environment]
```

The script will execute all QEMU-compatible tests using QEMU as it was built
at the given path. The test results will be compared against a list checked
into this repository at `tests/opentitan/data/earlgrey-tests.txt`.

Each test occupies its own line, prefixed by the expected status.
Tests with a `pass` status are expected to pass - failures indicate a
regression in either QEMU or Earlgrey. The script will fail if there is a
mismatch between the tests that we expect to pass and the actual results.

Some tests may be flaky and pass or fail on different runs of the same QEMU
and OpenTitan checkouts. These tests can be prefixed with the `flaky` status
to cause the script to ignore them. This will suppress these tests from being
printed as unexpected passes when they do succeed, but will also stop their
failures from causing the script to fail.

After each test, a comment can be provided (starting with a `#`), which can
help log the reasons that tests might have become flaky.

Specifying an execution environment when running the script will restrict to
only running and comparing against tests for that execution environment.
This can be used for more granular testing to break down large test workloads.
If you do not specify an execution environment, _all_ available QEMU tests
will be executed.
