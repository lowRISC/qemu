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
$NAME [options] <ot_dir> <ot_test> [-- [QEMU_options ...]]

    Execute an OpenTitan boot up sequence with TEST_ROM and a Test application

        -h         Print this help message
        -b         Build QEMU
        -f         Use a flat tree (files are stored in the same dir)
        -o         Log OTBN execution traces into otbn.log
        -r         Load ROM binary rather than ROM ELF file
        -S         Redirect guest stdout to stdout.log
        -t second  Maximum time to execute, after which QEMU instance is killed
        -T trace   QEMU trace file, output traces are stored in qemu.log
        -v         Verbose mode, show executed commands
        --         All following options are forwarded to QEMU

        ot_dir     OT top directory or flat dir with packaged file (see -F)
        ot_test    OT test application to execute
EOT
}

OT_DIR=""
OT_TEST=""
BUILD_QEMU=0
FLAT_TREE=0
LOAD_ELF=1
OTBN_LOG=0
VERBOSE=0
TIMEOUT=0
GUEST_STDOUT=""
TRACE=""
LOGFILE="qemu.log"
SUFFIX="prog_fpga_cw310.rsa_fake_test_key_0.signed.bin"
BZLSFX="prog_fpga_cw310_bin_signed_rsa_fake_test_key_0"

# Parse options
while [ $# -gt 0 ]; do
    case "$1" in
        -h)
           usage
           exit 0
           ;;
        -b)
           BUILD_QEMU=1
           ;;
        -f)
           FLAT_TREE=1
           ;;
        -o)
           OTBN_LOG=1
           ;;
        -r)
           LOAD_ELF=0
           ;;
        -S)
           GUEST_STDOUT="stdout.log"
           ;;
        -T)
           shift
           TRACE="$1"
           ;;
        -t)
           shift
           TIMEOUT="$1"
           ;;
        -v)
           VERBOSE=1
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
           elif [ -z "${OT_TEST}" ]; then
               OT_TEST="$1"
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

if [ -n "${TIMEOUT}" ]; then
    QEMU_TIMEOUT="timeout --foreground ${TIMEOUT}"
fi

if [ -n "${TRACE}" ]; then
    [ -r ${TRACE} ] || die "No such QEMU trace file ${TRACE}"
   TRACEOPTS="-D ${LOGFILE} -trace events=${TRACE}"
fi

if [ -z "${GUEST_STDOUT}" ]; then
    QEMU_SERIAL0="-serial mon:stdio"
else
    rm -f ${GUEST_STDOUT}
    QEMU_SERIAL0="-chardev file,id=gstdout,path=${GUEST_STDOUT} -serial chardev:gstdout"
fi

SCRIPT_DIR="$(dirname $0)"
QEMU_DIR="$(realpath ${SCRIPT_DIR}/../..)"
QEMU_BUILD_DIR="${QEMU_DIR}/build"

if [ ${FLAT_TREE} -gt 0 ]; then
    # flat tree mode expects all OT artifacts to be packaged in a single directory
    # with no subtree. This mode is useful when OT is built on a machine and
    # run on another one (if for some reason bazel fails, which never happens...)
    if [ ! -f "${OT_DIR}/img_rma.24.vmem" ]; then
        echo >&2 "Invalid ot_pack_dir ${OT_DIR}"
        exit 1
    fi
    OT_OTP_VMEM="${OT_DIR}/img_rma.24.vmem"
    OT_TEST_ROM_BIN="${OT_DIR}/test_rom_fpga_cw310.bin"
    OT_TEST_ROM_ELF="${OT_TEST_ROM_BIN%.*}.elf"
    OT_TEST_BIN="${OT_DIR}/${OT_TEST}_${SUFFIX}"
    [ -s "${OT_TEST_BIN}" ] || die "No such app: ${OT_TEST_BIN}"
else
    [ -x "${OT_DIR}/bazelisk.sh" ] || \
        die "${OT_DIR} is not a top-level OpenTitan directory"
    GIT="$(which git 2>/dev/null)"
    if [ -n "${GIT}" ]; then
        if [ -f "${QEMU_DIR}/hw/opentitan/ot_ref.log" ]; then
            . "${QEMU_DIR}/hw/opentitan/ot_ref.log"
            if [ -n "${GIT_COMMIT}" ]; then
                OT_GIT_COMMIT="$(cd ${OT_DIR} && ${GIT} rev-parse HEAD)"
                if [ "${OT_GIT_COMMIT}" != "${GIT_COMMIT}" ]; then
                    echo >&2 "Warning: OpenTitan repo differs from QEMU supported version"
                fi
            fi
        fi
    fi
    # default mode, use bazel to retrieve the path of artifacts
    # Bazel setup is out of scope of this script, please refer to OpenTitan
    # official installation instructions first
    # Do not forget to run this script from your OT venv if you use one.
    OT_OTP_VMEM=$(cd "${OT_DIR}" && \
            ./bazelisk.sh outquery //hw/ip/otp_ctrl/data:img_rma) || \
        die "Bazel in trouble"
    OT_OTP_VMEM="$(realpath ${OT_DIR}/${OT_OTP_VMEM})"
    [ -f "${OT_OTP_VMEM}" ] || \
        (cd "${OT_DIR}" && ./bazelisk.sh build //hw/ip/otp_ctrl/data:img_rma) || \
        die "Cannot build $(basename ${OT_OTP_VMEM})"
    if [ ${LOAD_ELF} -gt 0 ]; then
        OT_TEST_ROM_ELF=$(cd "${OT_DIR}" && \
            ./bazelisk.sh outquery --config riscv32 \
                //sw/device/lib/testing/test_rom:test_rom_fpga_cw310.elf) || \
            die "Bazel in trouble"
        OT_TEST_ROM_ELF="$(realpath ${OT_DIR}/${OT_TEST_ROM_ELF})"
        [ -f "${OT_TEST_ROM_ELF}" ] || \
            (cd "${OT_DIR}" && ./bazelisk.sh build  --config riscv32 \
                //sw/device/lib/testing/test_rom:test_rom_fpga_cw310.elf) || \
            die "Cannot build $(basename ${OT_TEST_ROM_BIN})"
    else
        OT_TEST_ROM_BIN=$(cd "${OT_DIR}" && \
            ./bazelisk.sh outquery \
                //sw/device/lib/testing/test_rom:test_rom_fpga_cw310) || \
            die "Bazel in trouble"
        OT_TEST_ROM_BIN="$(realpath ${OT_DIR}/${OT_TEST_ROM_BIN})"
        [ -f "${OT_TEST_ROM_BIN}" ] || \
            (cd "${OT_DIR}" && ./bazelisk.sh build //sw/device/lib/testing/test_rom:test_rom_fpga_cw310) || \
            die "Cannot build $(basename ${OT_TEST_ROM_BIN})"
    fi
    OT_TEST_BIN=$(cd "${OT_DIR}" && \
            ./bazelisk.sh outquery \
                //sw/device/tests:${OT_TEST}_${BZLSFX} ) || \
            die "No such test application ${OT_TEST}"
    OT_TEST_BIN="$(realpath ${OT_DIR}/${OT_TEST_BIN})"
    [ -f "${OT_TEST_BIN}" ] || \
        (cd "${OT_DIR}" && ./bazelisk.sh build //sw/device/tests:${OT_TEST}_${BZLSFX}) || \
        die "Cannot build $(basename ${OT_TEST_BIN})"
fi

# sanity checks
[ -f "${OT_OTP_VMEM}" ] || die "Unable to find OTP image"
[ -f "${OT_TEST_ROM_BIN}" ] || die "Unable to find ROM binary"
[ -f "${OT_TEST_BIN}" ] || die "Unable to find test application binary"

if [ ${BUILD_QEMU} -gt 0 ]; then
    if [ ! -d ${QEMU_BUILD_DIR} ]; then
        mkdir ${QEMU_BUILD_DIR} || die "Cannot create QEMU build directory"
    fi
    if [ ! -f "${QEMU_BUILD_DIR}/build.ninja" ]; then
        if [ "$(uname -s)" = "Darwin" ]; then
            # Cocoa deprecated APIs not yet fixed...
            QEMU_BUILD_OPT="--extra-cflags=-Wno-deprecated-declarations"
        else
            QEMU_BUILD_OPT="--without-default-features --enable-tcg --enable-trace-backends=log"
        fi
        # note: Meson does not seem to cope well with symlinks
        (cd ${QEMU_BUILD_DIR} &&
            ${QEMU_DIR}/configure --target-list=riscv32-softmmu \
                 ${QEMU_BUILD_OPT} --enable-debug) || \
        die "Cannot configure QEMU"
    fi
    (cd ${QEMU_BUILD_DIR} && ninja) || die "Cannot build QEMU"
fi

[ -x "${QEMU_BUILD_DIR}/qemu-system-riscv32" ] || die "QEMU has not been build yet"

if [ ${LOAD_ELF} -gt 0 ]; then
    # in this mode, QEMU loads the ROM image using the ELF information to locate
    # and execute the ROM. This is useful to get debugging symbols
    [ -f "${OT_TEST_ROM_ELF}" ] || die "Unable to find ROM ELF file"
    QEMU_GUEST_OPT="-kernel ${OT_TEST_ROM_ELF}"
else
    # in this mode, ROM image is force-loaded to the expected memory location
    # and the QEMU vCPU reset vector is hardcoded, which better mimics the actual
    # HW, but provided no debug info
    QEMU_GUEST_OPT="-device loader,addr=0x8000,file=${OT_TEST_ROM_BIN}" \
    QEMU_GUEST_OPT="${QEMU_GUEST_OPT} -global driver=lowrisc-ibex-riscv-cpu,property=resetvec,value=0x8180"
fi

if [ ${OTBN_LOG} -gt 0 ]; then
    QEMU_GUEST_OPT="${QEMU_GUEST_OPT} -global ot-otbn.logfile=otbn.log"
fi

echo "Use [Ctrl-A] + x to quit QEMU"

if [ ${VERBOSE} -gt 0 ];then
    set -x
fi

${SCRIPT_DIR}/otpconv.py -i "${OT_OTP_VMEM}" -o otp.raw || \
    die "Cannot generate OTP image"

# note: it is recommended to place the original ELF file from which the binary
# files have been generated from. If flashgen.py locates the matching ELF file,
# the ROM_EXT and BL0 symbols can be automatically loaded by QEMU, which helps
# debugging
${SCRIPT_DIR}/flashgen.py -D -x "${OT_TEST_BIN}" flash.raw || \
    die "Cannot generate flash image"

${QEMU_TIMEOUT} ${QEMU_BUILD_DIR}/qemu-system-riscv32 \
    -M ot-earlgrey -display none ${QEMU_SERIAL0} ${QEMU_GUEST_OPT} \
    -drive if=pflash,file=otp.raw,format=raw \
    -drive if=mtd,bus=1,file=flash.raw,format=raw ${TRACEOPTS} $*
QRES=$?
if [ -n "${GUEST_STDOUT}" ]; then
    cat ${GUEST_STDOUT}
fi
if [ ${QRES} -eq 124 ]; then
    echo >&2 "Timeout reached"
fi
exit ${QRES}
