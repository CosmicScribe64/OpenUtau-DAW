#!/usr/bin/env bash
set -euo pipefail

mapfile -t action_uses < <(
  grep -HnE '^[[:space:]]*-?[[:space:]]*uses:' .github/workflows/*.yml
)
if [[ "${#action_uses[@]}" -eq 0 ]]; then
  echo "No GitHub Action uses were found." >&2
  exit 1
fi

for action_use in "${action_uses[@]}"; do
  if [[ ! "$action_use" =~ uses:[[:space:]]*[^@[:space:]]+@([0-9a-f]{40})([[:space:]]|$) ]]; then
    echo "GitHub Action is not pinned to a full commit SHA: $action_use" >&2
    exit 1
  fi
done

if [[ "$(dotnet --version)" != 8.0.424 ]]; then
  echo "global.json did not select .NET SDK 8.0.424." >&2
  exit 1
fi
if ! grep -Eq 'dotnet-version:[[:space:]]+8\.0\.424$' .github/workflows/*.yml; then
  echo "GitHub workflows do not install .NET SDK 8.0.424." >&2
  exit 1
fi

if grep -REq 'git clone .*steinbergmedia/vst3sdk.*--depth|git clone --depth .*steinbergmedia/vst3sdk' \
    .github/workflows; then
  echo "Steinberg validator checkout uses a moving shallow branch." >&2
  exit 1
fi
if ! grep -REq 'git -C .*vst3sdk checkout [0-9a-f]{40}$' .github/workflows; then
  echo "Steinberg validator source is not pinned to a full commit SHA." >&2
  exit 1
fi

echo "GitHub workflow pinning policy tests passed."
