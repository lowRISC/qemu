#!/usr/bin/env sh
# Copyright (c) 2025 lowRISC contributors.

set -e

# This script will run all QEMU tests in the provided OpenTitan repository,
# comparing the results with a list of expected passes.
#
# USAGE: run-bazel-tests.sh path/to/opentitan/repo path/to/qemu/repo \
#            [execution environment]
#
# There is a companion file `tests/opentitan/data/earlgrey-tests.txt` that
# is read by this script.
# 
# The idea is to keep the list of passing tests up to date as QEMU changes.
#
# * If a test starts passing, add it with a `pass` status to the test
#   list to catch regressions.
# * When a test starts failing, investigate why and fix it, or remove it
#   from the test list if necessary.
#
# Tests which are marked `flaky` and may pass/fail randomly will still be run,
# but can be marked as such to prevent them from causing warnings or failures.

# CI-only job summary feature - write to `/dev/null` when run locally.
GITHUB_STEP_SUMMARY="${GITHUB_STEP_SUMMARY:-/dev/null}"

# CLI arguments:
opentitan_path="$1"
qemu_path="$2"
qemu_exec_env="$3"
if [ ! -d "$opentitan_path" ] || [ ! -d "$qemu_path" ]; then
  echo "USAGE: ${0} <OPENTITAN REPO> <QEMU REPO> [<EXEC ENV>]"
  exit 1
fi
opentitan_path="$(realpath "$opentitan_path")"
qemu_path="$(realpath "$qemu_path")"
if [ ! "$qemu_exec_env" ]; then
  # catch-all tag for all QEMU exec envs
  qemu_exec_env="qemu"
  echo "Using default 'qemu' tag to test all exec envs"
fi

# Lists of passing and flaky OpenTitan tests:
tests_path="${qemu_path}/tests/opentitan/data/earlgrey-tests.txt"

# Ensure QEMU has already been built in `./build`:
if [ ! -x "${qemu_path}/build/qemu-system-riscv32" ]; then
  echo >&2 "ERROR: expected QEMU binary at '${qemu_path}/build/qemu-system-riscv32'"
  exit 1
fi

# Check if needed Bazel repository override files exist
prev_bazel_files=false
if [ -f "${qemu_path}/REPO.bazel" ]; then
  prev_bazel_files=true
else
  # Add temporary Bazel `REPO.bazel` file
  touch "${qemu_path}/REPO.bazel"
fi

if [ -f "${qemu_path}/BUILD" ]; then
  prev_bazel_files=true
else
  # Add temporary Bazel 'BUILD' file
  ln -s                                                             \
    "${opentitan_path}/third_party/qemu/BUILD.qemu_opentitan.bazel" \
    "${qemu_path}/BUILD"
fi


# Temporary files used by this script:
results="$(mktemp)"
flaky="$(mktemp)"
expected="$(mktemp)"
all_passed="$(mktemp)"
passed="$(mktemp)"
cleanup() {
  if [ "$prev_bazel_files" != "true" ]; then
    rm -f "${qemu_path}/REPO.bazel" "${qemu_path}/BUILD"
  fi
  rm -f "$results" "$flaky" "$expected" "$all_passed" "$passed" 
}
trap "cleanup" EXIT

## RUN BAZEL TESTS
cd "$opentitan_path" >/dev/null

./bazelisk.sh test //...                                    \
  --test_tag_filters="$qemu_exec_env"                       \
  --test_summary="short"                                    \
  --test_output=all                                         \
  --override_repository="+qemu+qemu_opentitan=${qemu_path}" \
  --build_tests_only                                        \
  | tee "$results"

## COMPARE RESULTS

# Ensure the flaky tests are sorted with the current locale.
grep "flaky:.*$qemu_exec_env" "$tests_path" | cut -d" " -f2 | sort -u > "$flaky"

# Load the list of passing tests
grep -E "pass(ing)?:.*$qemu_exec_env" "$tests_path" | cut -d" " -f2 | sort -u > "$expected"

# Find all the tests which passed in Bazel:
grep "PASSED[^:]" "$results" | cut -d' ' -f1 | sort > "$all_passed"

# Filter out the flaky tests:
comm -23 "$all_passed" "$flaky" > "$passed"

## REPORT MISMATCHES

unexpected_failures="$(comm -13 "$passed" "$expected")"
unexpected_passes="$(comm -23 "$passed" "$expected")"

status=0

if [ -n "$unexpected_failures" ]; then
  echo                                                      | tee -a "$GITHUB_STEP_SUMMARY" >&2
  echo "Tests that we expected to pass which did NOT pass:" | tee -a "$GITHUB_STEP_SUMMARY" >&2
  echo "$unexpected_failures" | awk '$0="- `"$0"`"'         | tee -a "$GITHUB_STEP_SUMMARY" >&2
  status=1

  echo >&2 "::error::There were some unexpected test failures"
fi

if [ -n "$unexpected_passes" ]; then
  echo                                                          | tee -a "$GITHUB_STEP_SUMMARY" >&2
  echo "Tests which passed but we did NOT expect them to pass:" | tee -a "$GITHUB_STEP_SUMMARY" >&2
  echo "$unexpected_passes" | awk '$0="- `"$0"`"'               | tee -a "$GITHUB_STEP_SUMMARY" >&2

  echo >&2 "::warning::Some tests passed which we did not expect"
fi

exit $status
