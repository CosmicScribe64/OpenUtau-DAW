#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
package_version="${PACKAGE_VERSION:-local}"
compose=(
  docker compose
  -f "$root/compose.yaml"
  -f "$root/docker/compose.amd64.yml"
)

case "$package_version" in
  *[!A-Za-z0-9._-]*)
    echo "PACKAGE_VERSION contains unsupported archive-name characters." >&2
    exit 1
    ;;
esac

"${compose[@]}" build dev
"${compose[@]}" run --rm \
  -e ARCHITECTURE=x64 \
  -e PACKAGE_VERSION="$package_version" \
  -e BUILD_DIRECTORY=/workspace/.build/linux-x64 \
  -e OUTPUT_DIRECTORY=/workspace/artifacts/linux-x64-amd64-emulated \
  dev ./scripts/package-linux.sh

echo "Linux x64 package completed in the isolated amd64 Docker environment."
echo "To reclaim its cache after release, remove only these Docker objects:"
echo "  docker image rm open-utau-vst-dev:amd64"
echo "  docker volume rm open-utau-vst-nuget-cache-amd64 open-utau-vst-build-cache-amd64"
