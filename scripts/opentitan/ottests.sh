#!/bin/sh

# Copyright (c) 2023 Rivos, Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

# Die with an error message
die() {
    echo >&2 "$*"
    exit 1
}

# Show usage information
usage() {
    NAME=$(basename $0)
cat <<EOT
$NAME [options] <ot_dir> <pattern> [-- [QEMU_options ...]]

    Execute OpenTitan test applications and reports results

        -h         Print this help message
        --         All following options are forwarded to QEMU

        ot_dir     OT top directory
        pattern    Regular expression pattern to select the tests to execute
EOT
}

# note: this script use recent CLI tools such as 'fd' and 'rg'
# it could easily be adapted to use 'find' and 'grep'
which rg >/dev/null || die "rg not found"
which fd >/dev/null || die "fd not found"

OT_SCRIPTS=$(dirname $0)
OT_DIR=""
TPATTERN=""
OTBN_TIMEOUT=10
DEFAULT_TIMEOUT=2

# Parse options
while [ $# -gt 0 ]; do
    case "$1" in
        -h)
           usage
           exit 0
           ;;
        --)
           shift
           break
           ;;
        -*)
           die "Unknown option $1"
           ;;
        *)
           if [ -z "${OT_DIR}" ]; then
               OT_DIR="$1"
               [ -d "${OT_DIR}" ] || die "Invalid OT directory: ${OT_DIR}"
           elif [ -z "${TPATTERN}" ]; then
               TPATTERN="$1"
           else
                die "Unknown argument: $1"
           fi
           ;;
    esac
    shift
done

if [ -z "${OT_DIR}" ]; then
    usage
    exit 0
fi

success=0
fail=0
fatal=0
timeout=0
total=0

if [ ! -s spi.raw ]; then
    # assume that if the flash.raw backend file already exist, it has been
    # properly generated (otherwise, remove the file and restart this script)
    (cd build && ninja qemu-img all)
    ./build/qemu-img create -f raw spi.raw 16M
fi

QEMU_LOG="build/qemu.log"

echo "Test name,Result" > qemu_ottest_res.csv
#for tfile in $(fd -e elf . ${OT_DIR} | rg "${TPATTERN}"); do
for tfile in $(fd -e signed.bin . ${OT_DIR} | rg "${TPATTERN}"); do
    echo ""
    test_name=$(basename ${tfile%%.*})
    echo "${test_name}"
    test_radix=$(echo ${test_name} | sed -E 's/^(.*)_prog_.*$/\1/') || \
        die "Cannot extract test radix"
    echo "${test_radix}" | rg -q '^manuf'
    if [ $? -eq 0 ]; then
        echo "Skipping ${test_radix}"
    fi
    rm -f ${QEMU_LOG}
    echo "${test_radix}" | rg -q otbn
    if [ $? -eq 0 ]; then
        # OTBN simulator is slow, so timeout should be increased to avoid
        # kill the test while it has not completed - but not stalled
        TIMEOUT=${OTBN_TIMEOUT}
    else
        TIMEOUT=${DEFAULT_TIMEOUT}
    fi
    echo "${test_radix}" | rg -q '_wycheproof'
    if [ $? -eq 0 ]; then
        # disable vCPU slow down for them, since they are usually not time
        # sensitive
        ICOUNT=0
        # we really need a inline parser rather than relying on timeout values
        # but shell scripts are not a proper solution for this
        TIMEOUT=40
    else
        # vCPU clock < 15MHz
        ICOUNT=6
        echo "${test_radix}" | rg -q 'edn_concurrency'
        # some tests may take quite some time
        if [ $? -eq 0 ]; then
            TIMEOUT=30
        fi
    fi
    EXTRA_OPTS=""
    echo "${test_radix}" | rg -q '^aes_'
    if [ $? -eq 0 ]; then
        # Some AES tests need better scheduling accuracy
        EXTRA_OPTS="-global ot-aes.fast-mode=false"
    fi
    ${OT_SCRIPTS}/otrun.sh -t ${TIMEOUT} -f ${OT_DIR} ${test_radix} \
        -- -d unimp,guest_errors -icount ${ICOUNT} ${EXTRA_OPTS} > ${QEMU_LOG}
    TRES=$?
    RESULT=$(tail -1 ${QEMU_LOG})
    total=$(( total + 1 ))
    echo ${RESULT} | rg -q 'PASS!'
    if [ $? -eq 0 ]; then
    	echo "--- Ok"
	    success=$(( success + 1 ))
        echo "${test_radix},Ok" >> qemu_ottest_res.csv
    	continue
    fi
    cat ${QEMU_LOG}
    rm -f ${QEMU_LOG}
    echo ${RESULT} | rg -q 'FAIL!'
	if [ $? -eq 0 ]; then
    	echo "--- Error"
   	    fail=$(( fail + 1 ))
        echo "${test_radix},Error" >> qemu_ottest_res.csv
    	continue
    fi
    if [ ${TRES} -eq 124 ]; then
    	echo "--- Timeout"
   		timeout=$(( timeout + 1 ))
        echo "${test_radix},Timeout" >> qemu_ottest_res.csv
   	else
    	echo "--- Crash"
		fatal=$(( fatal + 1 ))
        echo "${test_radix},Crash" >> qemu_ottest_res.csv
	fi
done

rm -f ${QEMU_LOG}
echo "TOTAL $total, PASS $success, FAIL $fail, TIMEOUT $timeout, CRASH $fatal"
echo ""
cat qemu_ottest_res.csv
