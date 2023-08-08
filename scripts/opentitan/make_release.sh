#!/bin/bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Create a tarball containing the binaries and scripts necessary for opentitan.
set -e

if [ $# -ne 3 ]; then
    echo "Usage: $0 /path/to/output/tarball /path/to/build/dir /path/to/src/dir" >&2
    exit 1
fi

OUT_TARBALL="$1"
QEMU_BUILD_DIR="$2"
QEMU_SRC_DIR="$3"
# Create a temporary directory that we will tar.
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT
# Copy some binaries
cp "$QEMU_BUILD_DIR/qemu-system-riscv32" "$TMP_DIR/"
cp "$QEMU_BUILD_DIR/qemu-img" "$TMP_DIR/"
cp "$QEMU_SRC_DIR/scripts/opentitan/otpconv.py" "$TMP_DIR/"
cp "$QEMU_SRC_DIR/scripts/opentitan/flashgen.py" "$TMP_DIR/"
# Create archive.
tar --create --auto-compress --verbose --file="$OUT_TARBALL" --directory "$TMP_DIR/" .
