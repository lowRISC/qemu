#!/bin/bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Create a tarball containing the binaries and scripts necessary for opentitan.
set -e

if [ $# -ne 3 ]; then
    echo "Usage: $0 /path/to/output/tarball /path/to/src/dir build_dirname" >&2
    exit 1
fi

OUT_TARBALL="$1"
QEMU_DIR="$2"
QEMU_BUILD="$3"
# Create archive.
tar --create --auto-compress --verbose --file="$OUT_TARBALL" \
    --directory="$QEMU_DIR" \
    "$QEMU_BUILD"/qemu-{system-riscv32,img} \
    scripts/opentitan/{otptool,flashgen,cfggen}.py \
    python/qemu/ot
