#!/usr/bin/env sh
set -eu

expected="29e0e16d1623cda79ba7c3724614d6129ba3b9d5"
patch_file="$(pwd)/patches/openutau-vst.patch"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

git clone --quiet --shared upstream "$scratch/upstream"
actual="$(git -C "$scratch/upstream" rev-parse HEAD)"
if [ "$actual" != "$expected" ]; then
  echo "Patch replay baseline mismatch: expected $expected, found $actual" >&2
  exit 1
fi

git -C "$scratch/upstream" apply --unidiff-zero --check "$patch_file"
git -C "$scratch/upstream" apply --unidiff-zero "$patch_file"
git -C "$scratch/upstream" diff --check

if [ ! -f "$scratch/upstream/OpenUtau.Core/Render/VstRenderAdapter.cs" ]; then
  echo "Patch replay omitted the VST render adapter." >&2
  exit 1
fi
if ! grep -q 'HandleEmbeddedKeyDown' \
    "$scratch/upstream/OpenUtau/Views/MainWindow.axaml.cs"; then
  echo "Patch replay omitted embedded shortcut routing." >&2
  exit 1
fi

echo "Pinned OpenUtau patch replay check passed."
