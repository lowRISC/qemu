#!/bin/sh

# Copyright (c) 2024 Rivos, Inc.
# SPDX-License-Identifier: Apache2

EXPECTED_VERSION="17"

# find clang-tidy executable: either 'clang-tidy-17' or 'clang-tidy'
for ver_suffix in "-${EXPECTED_VERSION}" ""; do
    clangtidy="$(which clang-tidy${ver_suffix} 2>/dev/null)"
    if [ -n "${clangtidy}" ]; then
        break
    fi
done
if [ -z "${clangtidy}" ]; then
    echo "Unable to locate clang-tidy" >&2
    exit 1
fi

# check clang-tidy version
version_full="$(${clangtidy} --version | \
    grep "LLVM version" | head -1 | \
    sed -E 's/^.*LLVM version ([0-9]+\.[0-9]+\.[0-9]+).*$/\1/')"
version_major="$(echo ${version_full} | cut -d. -f1)"
if [ ${version_major} -lt ${EXPECTED_VERSION} ]; then
    echo "clang-tidy v${EXPECTED_VERSION} required, found ${version_full}" >&2
    exit 1
else
    if [ ${version_major} -ne ${EXPECTED_VERSION} ]; then
        echo -n "clang-tidy v${EXPECTED_VERSION} expected, " >&2
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
        --config-file=*)
           echo "Cannot override --config-file option" >&2
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
CFG="$(dirname $0)"/clang-tidy.yml
FILES_D="$(dirname $0)"/clang-tidy.d
FILES=""

# automatic file list
if [ $CI_MODE -eq 1 ]; then
    for filespec in $(cat "$FILES_D"/*.lst); do
        FILES="$FILES $QEMU_ROOT/$filespec"
    done
fi

exec "${clangtidy}" --config-file="${CFG}" $ARGS $FILES
