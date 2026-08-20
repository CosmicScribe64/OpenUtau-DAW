#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
architecture="${ARCHITECTURE:-arm64}"
package_tool_mode="${PACKAGE_TOOL_MODE:-docker}"
require_notarized="${REQUIRE_NOTARIZED:-0}"
archive="${1:-$root/artifacts/OpenUtau-DAW-macos-$architecture.zip}"
package="${2:-$root/artifacts/macos-$architecture-package}"
bundle="$package/OpenUtau DAW.vst3"
module="$bundle/Contents/MacOS/OpenUtau DAW"
worldline="$bundle/Contents/Resources/Engine/libworldline.dylib"
sidecar="$archive.sha256"
notices="$package/THIRD-PARTY-NOTICES.md"
agpl="$package/LICENSE-OPENUTAU-DAW-AGPL-3.0.txt"
source_notice="$package/CORRESPONDING-SOURCE.md"
dotnet_license="$bundle/Contents/Resources/EmbeddedEditor/dotnet/LICENSE.txt"
dotnet_notices="$bundle/Contents/Resources/EmbeddedEditor/dotnet/ThirdPartyNotices.txt"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "macOS package verification must run on macOS." >&2
  exit 1
fi
case "$architecture" in
  arm64) machine_pattern='arm64' ;;
  x64) machine_pattern='x86_64' ;;
  *) echo "ARCHITECTURE must be arm64 or x64." >&2; exit 1 ;;
esac
for required in \
  "$archive" "$sidecar" "$bundle" "$module" "$worldline" \
  "$notices" "$agpl" "$source_notice" "$dotnet_license" "$dotnet_notices"
do
  [[ -e "$required" ]] || { echo "Missing required package path: $required" >&2; exit 1; }
done

if ! find "$package/THIRD-PARTY-LICENSES" -type f -print -quit | grep -q .; then
  echo "Package has no collected third-party licence files." >&2
  exit 1
fi

archive_directory="$(dirname "$archive")"
(cd "$archive_directory" && shasum -a 256 -c "$(basename "$sidecar")")
codesign --verify --deep --strict --verbose=2 "$bundle"
if [[ "$require_notarized" == "1" ]]; then
  codesign -d --verbose=4 "$bundle" 2>&1 \
    | grep -E '^Authority=Developer ID Application:' >/dev/null || {
    echo "Package is not signed with a Developer ID Application identity." >&2
    exit 1
  }
  xcrun stapler validate "$bundle"
elif [[ "$require_notarized" != "0" ]]; then
  echo "REQUIRE_NOTARIZED must be 0 or 1." >&2
  exit 1
fi
file "$module" | grep -Eq "$machine_pattern" || {
  echo "VST3 module is not $architecture." >&2; exit 1;
}
file "$worldline" | grep -Eq "$machine_pattern" || {
  echo "Worldline renderer is not $architecture." >&2; exit 1;
}

if unzip -Z1 "$archive" | grep -E '(^|/)\._|(^|/)\.DS_Store$' >/dev/null; then
  echo "Archive contains Finder/resource-fork metadata." >&2
  exit 1
fi

for binary in "$module" "$worldline"; do
  # otool headings name the inspected file (once per architecture); dependency
  # lines are indented. Check those lines only.
  if otool -L "$binary" | grep -E '^[[:space:]]' \
      | grep -E '(/Users/|/opt/homebrew/|/usr/local/)' >/dev/null; then
    echo "Package binary references a host-local dependency: $binary" >&2
    exit 1
  fi
done

archive_relative="${archive#"$root"/}"
if [[ "$package_tool_mode" == "docker" ]]; then
  docker compose run --rm dev dotnet run --project \
    /workspace/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj \
    --configuration Release -- verify "/workspace/$archive_relative"
elif [[ "$package_tool_mode" == "host" ]]; then
  dotnet run --project "$root/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj" \
    --configuration Release -- verify "$archive"
else
  echo "PACKAGE_TOOL_MODE must be docker or host." >&2
  exit 1
fi
echo "macOS package verification passed."
