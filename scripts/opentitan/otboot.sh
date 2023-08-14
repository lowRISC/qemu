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
$NAME [-b] [-r] [-f] <ot_dir> [-- [QEMU_options ...]]

    Execute an OpenTitan boot up sequence with ROM/ROM_EXT/BL0 binaries

        -h   Print this help message
        -b   Build QEMU
        -f   Use a flat tree (files are stored in the same dir)
        -r   Load ROM binary rather than ROM ELF file
        -U   Run flashgen.py in unsafe mode (no ELF checks)
        -v   Verbose mode, show executed commands
        --   All following options are forwarded to QEMU

        ot_dir OpenTitan top directory or flat dir with packaged file (see -F)
EOT
}

OT_DIR=""
BUILD_QEMU=0
FLASHGEN_OPTS=""
FLAT_TREE=0
LOAD_ELF=1
VERBOSE=0

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
        -r)
           LOAD_ELF=0
           ;;
        -v)
           VERBOSE=1
           ;;
        -U)
           FLASHGEN_OPTS="${FLASHGEN_OPTS} -U"
           ;;
        --)
           shift
           break
           ;;
        -*)
           die "Unknown option $1"
           ;;
        *)
           [ -z "${OT_DIR}" ] || die "ot_dir already specified"
           OT_DIR="$1"
           ;;
    esac
    shift
done

if [ -z "${OT_DIR}" ]; then
    usage
    exit 0
fi

SCRIPT_DIR="$(dirname $0)"
QEMU_DIR="$(realpath ${SCRIPT_DIR}/../..)"
QEMU_BUILD_DIR="${QEMU_DIR}/build"

if [ ${FLAT_TREE} -gt 0 ]; then
    # flat tree mode expects all OT artifacts to be packaged in a single directory
    # with no subtree. This mode is useful when OT is built on a machine and
    # run on another one (if for some reason bazel fails, which never happens...)
    if [ ! -f "${OT_DIR}/rom_with_fake_keys_fpga_cw310.bin" ]; then
        echo >&2 "Invalid ot_pack_dir ${OT_DIR}"
        exit 1
    fi
    OT_OTP_VMEM="${OT_DIR}/img_rma.24.vmem"
    OT_ROM_BIN="${OT_DIR}/rom_with_fake_keys_fpga_cw310.bin"
    OT_ROM_EXT_BIN="${OT_DIR}/rom_ext_slot_virtual_fpga_cw310.fake_rsa_test_key_0.signed.bin"
    OT_BL0_TEST_BIN="${OT_DIR}/bare_metal_slot_virtual_fpga_cw310.fake_rsa_rom_ext_test_key_0.signed.bin"
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
    # Note: no idea on how to retrieve ELF files in this case (prefer flatdir option)
    OT_OTP_VMEM=$(cd "${OT_DIR}" && \
            ./bazelisk.sh outquery //hw/ip/otp_ctrl/data:img_rma) || \
        die "Bazel in trouble"
    OT_OTP_VMEM="$(realpath ${OT_DIR}/${OT_OTP_VMEM})"
    [ -f "${OT_OTP_VMEM}" ] || \
        (cd "${OT_DIR}" && ./bazelisk.sh build //hw/ip/otp_ctrl/data:img_rma) || \
        die "Cannot build $(basename ${OT_OTP_VMEM})"
    OT_ROM_BIN=$(cd "${OT_DIR}" && \
        ./bazelisk.sh outquery \
            //sw/device/silicon_creator/rom:rom_with_fake_keys_fpga_cw310) || \
        die "Bazel in trouble"
    OT_ROM_BIN="$(realpath ${OT_DIR}/${OT_ROM_BIN})"
    [ -f "${OT_ROM_BIN}" ] || \
        (cd "${OT_DIR}" && ./bazelisk.sh build //sw/device/silicon_creator/rom:rom_with_fake_keys_fpga_cw310) || \
        die "Cannot build $(basename ${OT_ROM_BIN})"
    OT_ROM_EXT_BIN=$(cd "${OT_DIR}" && \
        ./bazelisk.sh outquery \
            //sw/device/silicon_creator/rom_ext:rom_ext_slot_virtual_fpga_cw310_bin_signed_fake_rsa_test_key_0) || \
        die "Bazel in trouble"
    OT_ROM_EXT_BIN="$(realpath ${OT_DIR}/${OT_ROM_EXT_BIN})"
    [ -f "${OT_ROM_EXT_BIN}" ] || \
        (cd "${OT_DIR}" && ./bazelisk.sh build \
            //sw/device/silicon_creator/rom_ext:rom_ext_slot_virtual_fpga_cw310_bin_signed_fake_rsa_test_key_0) || \
        die "Cannot build $(basename ${OT_ROM_EXT_BIN})"
    OT_BL0_TEST_BIN=$(cd "${OT_DIR}" && \
        ./bazelisk.sh outquery \
            //sw/device/silicon_owner/bare_metal:bare_metal_slot_virtual_fpga_cw310_bin_signed_fake_rsa_rom_ext_test_key_0) || \
        die "Bazel in trouble"
    OT_BL0_TEST_BIN="$(realpath ${OT_DIR}/${OT_BL0_TEST_BIN})"
    [ -f "${OT_BL0_TEST_BIN}" ] || \
        (cd "${OT_DIR}" && ./bazelisk.sh build \
        //sw/device/silicon_owner/bare_metal:bare_metal_slot_virtual_fpga_cw310_bin_signed_fake_rsa_rom_ext_test_key_0) || \
        die "Cannot build $(basename ${OT_BL0_TEST_BIN})"
fi

# sanity checks
[ -f "${OT_OTP_VMEM}" ] || die "Unable to find OTP image $(basename ${OT_OTP_VMEM})"
[ -f "${OT_ROM_BIN}" ] || die "Unable to find ROM binary $(basename ${OT_ROM_BIN})"
[ -f "${OT_ROM_EXT_BIN}" ] || die "Unable to find ROM_EXT binary $(basename ${OT_ROM_EXT_BIN})"
[ -f "${OT_BL0_TEST_BIN}" ] || die "Unable to find BL0 test binary $(basename ${OT_BL0_TEST_BIN})"

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
    OT_ROM_ELF="${OT_ROM_BIN%.*}.elf"
    [ -f "${OT_ROM_ELF}" ] || die "Unable to find ROM ELF file for $(basename ${OT_ROM_BIN})"
    QEMU_GUEST_OPT="-kernel ${OT_ROM_ELF}"
else
    # in this mode, ROM image is force-loaded to the expected memory location
    # and the QEMU vCPU reset vector is hardcoded, which better mimics the actual
    # HW, but provided no debug info
    QEMU_GUEST_OPT="-device loader,addr=0x8000,file=${OT_ROM_BIN}" \
    QEMU_GUEST_OPT="${QEMU_GUEST_OPT} -global driver=lowrisc-ibex-riscv-cpu,property=resetvec,value=0x8180"
fi

if [ ${VERBOSE} -gt 0 ];then
    set -x
fi

${SCRIPT_DIR}/otpconv.py -v -i "${OT_OTP_VMEM}" -o otp.raw || die "optconv.py failed"

# note: it is recommended to place the original ELF file from which the binary
# files have been generated from. If flashgen.py locates the matching ELF file,
# the ROM_EXT and BL0 symbols can be automatically loaded by QEMU, which helps
# debugging
${SCRIPT_DIR}/flashgen.py -v -D -x "${OT_ROM_EXT_BIN}" -b "${OT_BL0_TEST_BIN}" \
    flash.raw ${FLASHGEN_OPTS} || die "flashgen.py failed"

echo "Use [Ctrl-A] + x to quit QEMU"
${QEMU_BUILD_DIR}/qemu-system-riscv32 \
    -M ot-earlgrey -display none -serial mon:stdio ${QEMU_GUEST_OPT} \
    -drive if=pflash,file=otp.raw,format=raw \
    -drive if=mtd,bus=1,file=flash.raw,format=raw $*

