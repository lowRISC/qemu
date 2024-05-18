#!/bin/sh

# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

EXPECTED_VERSION="18"

# find clang-format executable: either 'clang-format-17' or 'clang-format'
for ver_suffix in "-${EXPECTED_VERSION}" ""; do
    clangformat="$(which clang-format${ver_suffix} 2>/dev/null)"
    if [ -n "${clangformat}" ]; then
        break
    fi
done
if [ -z "${clangformat}" ]; then
    echo "Unable to locate clang-format" >&2
    exit 1
fi

# check clang-format version
version_full="$(${clangformat} --version | \
    grep "clang-format version" | head -1 | \
    sed -E 's/^.*clang-format version ([0-9]+\.[0-9]+\.[0-9]+).*$/\1/')"
if [ -z "${version_full}" ]; then
    echo "Unable to retrieve LLVM version" >&2
    exit 1
fi
version_major="$(echo ${version_full} | cut -d. -f1)"
if [ ${version_major} -lt ${EXPECTED_VERSION} ]; then
    echo "clang-format v${EXPECTED_VERSION} required, found ${version_full}" >&2
    exit 1
else
    if [ ${version_major} -ne ${EXPECTED_VERSION} ]; then
        echo -n "clang-format v${EXPECTED_VERSION} expected, " >&2
        echo "v${version_full} may generate unexpected results" >&2
    fi
fi

CI_MODE=0
ARGS=""

# filter arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --ci)
           CI_MODE=1
           ;;
        --style=*)
           echo "Cannot override --style option" >&2
           exit 1
           ;;
        *)
           ARGS="$ARGS $1"
           ;;
    esac
    shift
done

# config
QEMU_ROOT="$(dirname $0)"/../..
CFG="$(dirname $0)"/clang-format.yml
FILES=""

# automatic file list
if [ $CI_MODE -eq 1 ]; then
    # file list path
    FILES_D="$(dirname $0)"/clang-format.d

    for filespec in $(cat "$FILES_D"/*.lst); do
        FILES="$FILES $QEMU_ROOT/$filespec"
    done
fi

exec "${clangformat}" --style=file:"${CFG}" $ARGS $FILES
