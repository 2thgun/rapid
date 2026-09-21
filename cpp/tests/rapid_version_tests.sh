#!/bin/sh
# Exercises packaging/RapidVersion.cmake: the explicit override, the
# git-derived dev version (monotonic commit count + short SHA, #58) and the
# loud failure when git metadata is unavailable (#46).
set -eu
source_root=$(cd "$1" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rapid-version.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

version_cmake="$source_root/packaging/RapidVersion.cmake"
test -f "$version_cmake"

# A throwaway repository so the git-derived path is exercised without touching
# the real checkout (a source archive has no .git at all).
repo="$work/repo"
mkdir -p "$repo/packaging"
cp "$version_cmake" "$repo/packaging/"
git -C "$repo" init -q
git -C "$repo" config user.email test@example.invalid
git -C "$repo" config user.name test
i=1
while [ "$i" -le 3 ]; do
  echo "$i" > "$repo/f$i"
  git -C "$repo" add "f$i"
  git -C "$repo" commit -qm "c$i"
  i=$((i + 1))
done

run_version() {
  cmake "$@" -P "$repo/packaging/RapidVersion.cmake" > "$work/output" 2>&1
}

expect_version() {
  expected=$1
  shift
  if ! run_version "$@"; then
    cat "$work/output"
    echo "FAIL: expected version $expected" >&2
    exit 1
  fi
  if ! grep -qF "raPId package version $expected" "$work/output"; then
    cat "$work/output"
    echo "FAIL: expected version $expected" >&2
    exit 1
  fi
  echo "ok: derived $expected"
}

expect_fail() {
  if run_version; then
    cat "$work/output"
    echo "FAIL: verifier accepted $1" >&2
    exit 1
  fi
  if ! tr -s ' \n' '  ' < "$work/output" | grep -qF "$2"; then
    cat "$work/output"
    echo "FAIL: $1 was rejected for the wrong reason" >&2
    exit 1
  fi
  echo "ok: rejects $1"
}

# #58: an untagged commit derives 0.9.9~dev.<commit-count>+<short-sha>; the
# count is monotonic so a newer commit is an upgrade, not a downgrade.
count=$(git -C "$repo" rev-list --count HEAD)
sha=$(git -C "$repo" rev-parse --short=7 HEAD)
expect_version "0.9.9~dev.$count+$sha"

# A vX.Y.Z tag becomes the bare release version.
git -C "$repo" tag v0.9.9
expect_version "0.9.9"

# An explicit -DRAPID_PACKAGE_VERSION always wins (source archives, workflows).
expect_version "1.2.3" -DRAPID_PACKAGE_VERSION=1.2.3

# #46: with no git metadata and no explicit version, fail loudly instead of
# emitting an untraceable 0.9.9~dev+unknown.
archive="$work/archive"
mkdir -p "$archive/packaging"
cp "$version_cmake" "$archive/packaging/"
if cmake -P "$archive/packaging/RapidVersion.cmake" > "$work/output" 2>&1; then
  cat "$work/output"
  echo "FAIL: a source archive without git metadata was accepted" >&2
  exit 1
fi
if ! tr -s ' \n' '  ' < "$work/output" | grep -qF "Cannot derive the raPId package version"; then
  cat "$work/output"
  echo "FAIL: a source archive failed for the wrong reason" >&2
  exit 1
fi
echo "ok: rejects a source archive without git metadata"

# The explicit override still works for that same archive.
if ! cmake -DRAPID_PACKAGE_VERSION=0.9.9~dev.42+abcdef0 -P "$archive/packaging/RapidVersion.cmake" > "$work/output" 2>&1; then
  cat "$work/output"
  echo "FAIL: an explicit version did not rescue a source archive" >&2
  exit 1
fi
if ! grep -qF "raPId package version 0.9.9~dev.42+abcdef0" "$work/output"; then
  cat "$work/output"
  echo "FAIL: an explicit version was not used for a source archive" >&2
  exit 1
fi
echo "ok: an explicit version rescues a source archive"

echo "version derivation tests passed"
