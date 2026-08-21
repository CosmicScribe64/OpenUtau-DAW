#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
expected="29e0e16d1623cda79ba7c3724614d6129ba3b9d5"
patch_file="$root/patches/openutau-vst.patch"
temporary="$(mktemp "$root/patches/.openutau-vst.patch.XXXXXX")"
declare -a untracked=()
intent_to_add=0

cleanup() {
  if [[ "$intent_to_add" == 1 ]]; then
    git -C "$root/upstream" reset --quiet -- "${untracked[@]}"
  fi
  rm -f "$temporary"
}
trap cleanup EXIT

actual="$(git -C "$root/upstream" rev-parse HEAD)"
if [[ "$actual" != "$expected" ]]; then
  echo "OpenUtau baseline mismatch: expected $expected, found $actual" >&2
  exit 1
fi
if ! git -C "$root/upstream" diff --cached --quiet; then
  echo "Unstage the OpenUtau changes before refreshing the patch." >&2
  exit 1
fi

while IFS= read -r -d '' path; do
  untracked+=("$path")
done < <(git -C "$root/upstream" ls-files --others --exclude-standard -z)
if (( ${#untracked[@]} > 0 )); then
  git -C "$root/upstream" add --intent-to-add -- "${untracked[@]}"
  intent_to_add=1
fi

git -C "$root/upstream" diff --binary --no-ext-diff > "$temporary"
if ! grep -q '^diff --git ' "$temporary"; then
  echo "There are no OpenUtau changes to record." >&2
  exit 1
fi

if [[ "$intent_to_add" == 1 ]]; then
  git -C "$root/upstream" reset --quiet -- "${untracked[@]}"
  intent_to_add=0
fi
mv "$temporary" "$patch_file"
trap - EXIT

"$root/scripts/verify-upstream-patch.sh"
echo "Updated patches/openutau-vst.patch from the OpenUtau working tree."
