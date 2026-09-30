#!/bin/sh
# Prints the version packaging/RapidVersion.cmake derives for the checkout at
# HEAD: a vX.Y.Z tag is the bare release version, anything else is
# <base>~dev.<commit-count>+<short-sha>. CI compares the Windows companion's
# build identity and the Pi package's version against this so the two always
# name the same build (#73).
#
# The commit count needs full history. A shallow clone counts 1, which made the
# CI companion say dev.1 while the same revision's package said dev.460, so this
# refuses to guess in a shallow clone. Run it from the repository root.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
if [ "$(git -C "$root" rev-parse --is-shallow-repository)" = true ]; then
  echo "expected-version: this checkout is shallow, so the commit count is wrong; use actions/checkout with fetch-depth: 0" >&2
  exit 1
fi
base=$(sed -n 's/^set(RAPID_VERSION_BASE "\(.*\)")$/\1/p' "$root/packaging/RapidVersion.cmake")
[ -n "$base" ] || { echo "expected-version: cannot read RAPID_VERSION_BASE from packaging/RapidVersion.cmake" >&2; exit 1; }
tag=$(git -C "$root" describe --exact-match --tags HEAD 2>/dev/null || true)
case "$tag" in
  v[0-9]*) printf '%s\n' "${tag#v}"; exit 0 ;;
esac
count=$(git -C "$root" rev-list --count HEAD)
sha=$(git -C "$root" rev-parse --short=7 HEAD)
printf '%s~dev.%s+%s\n' "$base" "$count" "$sha"
