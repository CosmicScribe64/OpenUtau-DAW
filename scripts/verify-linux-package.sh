#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
architecture="${ARCHITECTURE:-x64}"
archive="${1:-$root/artifacts/OpenUtau-DAW-linux-$architecture.zip}"
package="${2:-$root/artifacts/linux-$architecture-package}"
bundle="$package/OpenUtau DAW.vst3"

case "$architecture" in
  x64) bundle_architecture=x86_64-linux; machine_pattern='x86-64|x86_64' ;;
  arm64) bundle_architecture=aarch64-linux; machine_pattern='aarch64|ARM aarch64' ;;
  *) echo "ARCHITECTURE must be x64 or arm64." >&2; exit 1 ;;
esac
module="$bundle/Contents/$bundle_architecture/OpenUtau DAW.so"
worldline="$bundle/Contents/Resources/Engine/libworldline.so"
runtime="$bundle/Contents/Resources/EmbeddedEditor/dotnet/dotnet"
editor="$bundle/Contents/Resources/Editor/OpenUtau"

for required in "$archive" "$archive.sha256" "$module" "$worldline" "$runtime" \
  "$editor" "$package/PACKAGE-MANIFEST.sha256" \
  "$package/THIRD-PARTY-NOTICES.md"; do
  [[ -e "$required" ]] || { echo "Missing required package path: $required" >&2; exit 1; }
done
[[ -x "$editor" ]] || { echo "Linux editor launcher is not executable." >&2; exit 1; }

(cd "$(dirname "$archive")" && sha256sum -c "$(basename "$archive").sha256")
unzip -tq "$archive" >/dev/null
file "$module" | grep -Eq "$machine_pattern" || {
  echo "VST3 module has the wrong architecture." >&2; exit 1;
}
file "$worldline" | grep -Eq "$machine_pattern" || {
  echo "Worldline runtime has the wrong architecture." >&2; exit 1;
}
for binary in "$module" "$worldline" "$runtime"; do
  if ldd "$binary" 2>&1 | grep -q 'not found'; then
    echo "Package binary has a missing shared-library dependency: $binary" >&2
    ldd "$binary" >&2 || true
    exit 1
  fi
  if ldd "$binary" 2>&1 | grep -Eq '/workspace/|/[.]build/'; then
    echo "Package binary references a build-local dependency: $binary" >&2
    exit 1
  fi
done

dotnet run --project "$root/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj" \
  --configuration Release -- verify "$archive"
echo "Linux package verification passed."
