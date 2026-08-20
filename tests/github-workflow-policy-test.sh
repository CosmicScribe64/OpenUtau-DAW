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

for safe_directory in \
  /workspace \
  /workspace/upstream \
  /workspace/.git/modules/upstream; do
  if ! grep -Fq "safe.directory $safe_directory" docker/dev.Dockerfile; then
    echo "Docker image does not trust expected Git path: $safe_directory" >&2
    exit 1
  fi
done

for required_release_text in \
  'runs-on: macos-15-intel' \
  'OpenUtau-DAW-$RELEASE_TAG-macos-x64.zip' \
  'OpenUtau-DAW-$RELEASE_TAG-linux-x64.zip'; do
  if ! grep -Fq "$required_release_text" .github/workflows/release-candidate.yml; then
    echo "Release workflow is missing platform policy: $required_release_text" >&2
    exit 1
  fi
done

for amd64_policy in \
  'image: open-utau-vst-dev:amd64' \
  'platform: linux/amd64' \
  'name: open-utau-vst-nuget-cache-amd64' \
  'name: open-utau-vst-build-cache-amd64'; do
  if ! grep -Fq "$amd64_policy" docker/compose.amd64.yml; then
    echo "Local amd64 Docker policy is missing: $amd64_policy" >&2
    exit 1
  fi
done
if ! grep -Fq 'docker/compose.amd64.yml' scripts/package-linux-amd64-docker.sh; then
  echo "Linux x64 Docker helper does not use the isolated amd64 override." >&2
  exit 1
fi
if ! grep -Fq 'BUILD_DIRECTORY=/workspace/.build/linux-x64' \
    scripts/package-linux-amd64-docker.sh; then
  echo "Linux x64 Docker helper does not pin its CMake cache path." >&2
  exit 1
fi
if ! grep -Fq 'version="${1:?Expected the release version tag.}"' \
    scripts/package-source.sh; then
  echo "Source packaging must require an explicit release version." >&2
  exit 1
fi

echo "GitHub workflow pinning policy tests passed."
