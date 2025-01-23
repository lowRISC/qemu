#!/usr/bin/env sh
# Copyright lowRISC.

# This script checks that commit messages are in the correct format:
#
#   1. The title must be of the form `[ot] path/to/change: description`.
#   2. The message must end with `Signed-off-by: Name <email@address>`.
#
# Usage:
#
#   lint-commits.sh <first commit>

set -e

# Check the commit title has the correct prefixes.
lint_title() {
  commit="$1"

  title="$(git show "$commit" -s --format="format:%s")"
  short_hash="$(git show "$commit" -s --format="format:%h")"

  example() {
    echo "Got:"                                                     >&2
    echo "    ${title}"                                             >&2
    echo
    echo "Example:"                                                 >&2
    echo "    [ot] hw/opentitan: ot_hmac: fix i2c register address" >&2
  }

  if ! echo "$title" | grep -P -q '^\[ot\]'; then
    echo "::error::${short_hash}: commit titles must have the prefix '[ot]'" >&2
    example
    exit 1
  fi

  if ! echo "$title" | grep -P -q '^\[ot\]\s+[^:]+:'; then
    echo "::error::${short_hash}: commit titles must contain the path of the change" >&2
    example
    exit 1
  fi

  if ! echo "$title" | grep -P -q '^\[ot\]\s+[^:]+:\s+[^:]+:'; then
    echo "::error::${short_hash}: commit titles must state the changed component" >&2
    example
    exit 1
  fi
}

# Check the commit is signed off.
lint_signed_off_by() {
  commit="$1"

  signed_off_by="$(git show "$commit" -s --format="format:%(trailers:key=Signed-off-by)")"
  short_hash="$(git show "$commit" -s --format="format:%h")"

  if [ -z "$signed_off_by" ]; then
    echo "::error::${short_hash}: is missing a 'Signed-off-by' line" >&2
    echo "Hint:"                                                     >&2
    echo "Use \`git commit -s\` to add sign-offs"                    >&2
    exit 1
  fi
}

first_commit="$1"
if [ -z "$first_commit" ]; then
  echo "Usage: ${0} <first commit>" >&2
  exit 1
fi

exit_code=0

# Lint each commit from the merge target to now.
for commit in $(git log "${first_commit}..HEAD" --format="format:%H" --no-merges); do
  lint_title "$commit"         || exit_code=1
  lint_signed_off_by "$commit" || exit_code=1
done

exit $exit_code
