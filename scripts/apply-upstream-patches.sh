#!/bin/sh
set -eu

expected="29e0e16d1623cda79ba7c3724614d6129ba3b9d5"
actual="$(git -C upstream rev-parse HEAD)"
if [ "$actual" != "$expected" ]; then
  echo "OpenUtau baseline mismatch: expected $expected, found $actual" >&2
  exit 1
fi

patch_file="$(pwd)/patches/openutau-vst.patch"
if git -C upstream apply --unidiff-zero --reverse --check "$patch_file" 2>/dev/null; then
  echo "OpenUtau VST adapter patch is already applied."
elif git -C upstream apply --unidiff-zero --check "$patch_file"; then
  git -C upstream apply --unidiff-zero "$patch_file"
  echo "Applied OpenUtau VST adapter patch."
else
  echo "OpenUtau working tree is neither clean nor correctly patched." >&2
  exit 1
fi
