#!/usr/bin/env bash
set -euo pipefail

configuration="${CONFIGURATION:-Release}"
architecture="${ARCHITECTURE:-x64}"
package_version="${PACKAGE_VERSION:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
build_directory="${BUILD_DIRECTORY:-$root/.build/linux-$architecture}"
output_directory="${OUTPUT_DIRECTORY:-$root/artifacts/linux-$architecture-package}"
publish_directory="$build_directory/publish"
bundle_name="OpenUtau DAW.vst3"

case "$architecture" in
  x64)
    expected_machine=x86_64
    bundle_architecture=x86_64-linux
    ;;
  arm64)
    expected_machine=aarch64
    bundle_architecture=aarch64-linux
    ;;
  *) echo "ARCHITECTURE must be x64 or arm64." >&2; exit 1 ;;
esac
if [[ "$(uname -s)" != Linux || "$(uname -m)" != "$expected_machine" ]]; then
  echo "Linux $architecture packaging must run on a native $expected_machine Linux host." >&2
  exit 1
fi
if [[ -n "$package_version" && "$package_version" == *[!A-Za-z0-9._-]* ]]; then
  echo "PACKAGE_VERSION contains unsupported archive-name characters." >&2
  exit 1
fi

cd "$root"
./scripts/apply-upstream-patches.sh

cmake -S plugin -B "$build_directory" -G Ninja -DCMAKE_BUILD_TYPE="$configuration"
cmake --build "$build_directory" --target OpenUtauVst_VST3 plugin_smoke_tests

rm -rf "$publish_directory"
mkdir -p "$publish_directory/Engine" "$publish_directory/Editor/app"
dotnet publish bridge/OpenUtau.Vst.Engine.Host/OpenUtau.Vst.Engine.Host.csproj \
  -c "$configuration" -r "linux-$architecture" --self-contained false \
  -p:UseAppHost=false -p:PublishReadyToRun=false \
  -o "$publish_directory/Engine"
dotnet publish upstream/OpenUtau/OpenUtau.csproj \
  -c "$configuration" -r "linux-$architecture" --self-contained false \
  -p:UseAppHost=false -p:PublishReadyToRun=false \
  -o "$publish_directory/Editor/app"

dotnet_executable="$(readlink -f "$(command -v dotnet)")"
dotnet_root="$(dirname "$dotnet_executable")"
runtime_version="$(dotnet --list-runtimes | awk \
  '$1 == "Microsoft.NETCore.App" && $2 ~ /^8[.]/ { print $2 }' | sort -V | tail -n 1)"
if [[ -z "$runtime_version" ]]; then
  echo "A Microsoft.NETCore.App 8 runtime is required for packaging." >&2
  exit 1
fi
private_runtime="$publish_directory/EmbeddedEditor/dotnet"
mkdir -p "$private_runtime/host/fxr/$runtime_version" \
  "$private_runtime/shared/Microsoft.NETCore.App/$runtime_version"
cp "$dotnet_executable" "$private_runtime/dotnet"
cp -a "$dotnet_root/host/fxr/$runtime_version/." \
  "$private_runtime/host/fxr/$runtime_version/"
cp -a "$dotnet_root/shared/Microsoft.NETCore.App/$runtime_version/." \
  "$private_runtime/shared/Microsoft.NETCore.App/$runtime_version/"
cp "$dotnet_root/LICENSE.txt" "$dotnet_root/ThirdPartyNotices.txt" "$private_runtime/"

rm -rf "$output_directory"
mkdir -p "$output_directory"
source_bundle="$build_directory/OpenUtauVst_artefacts/$configuration/VST3/$bundle_name"
bundle="$output_directory/$bundle_name"
cp -a "$source_bundle" "$bundle"
mkdir -p "$bundle/Contents/Resources"
cp -a "$publish_directory/Engine" "$bundle/Contents/Resources/Engine"
cp -a "$publish_directory/EmbeddedEditor" "$bundle/Contents/Resources/EmbeddedEditor"
cp -a "$publish_directory/Editor" "$bundle/Contents/Resources/Editor"
cp packaging/linux/OpenUtau "$bundle/Contents/Resources/Editor/OpenUtau"
chmod 755 "$bundle/Contents/Resources/Editor/OpenUtau"

module="$bundle/Contents/$bundle_architecture/OpenUtau DAW.so"
for required in \
  "$module" \
  "$bundle/Contents/Resources/Engine/OpenUtau.Vst.Engine.Host.dll" \
  "$bundle/Contents/Resources/Engine/OpenUtau.Plugin.Builtin.dll" \
  "$bundle/Contents/Resources/Engine/libworldline.so" \
  "$bundle/Contents/Resources/EmbeddedEditor/dotnet/dotnet" \
  "$bundle/Contents/Resources/Editor/OpenUtau" \
  "$bundle/Contents/Resources/Editor/app/OpenUtau.dll"; do
  [[ -f "$required" ]] || { echo "Package is missing: $required" >&2; exit 1; }
done

cp upstream/LICENSE.txt "$output_directory/LICENSE-OPENUTAU-MIT.txt"
cp LICENSE "$output_directory/LICENSE-OPENUTAU-DAW-AGPL-3.0.txt"
cp docs/licensing.md "$output_directory/DISTRIBUTION-LICENSING.md"
cp docs/source-offer.md "$output_directory/CORRESPONDING-SOURCE.md"
cp docs/verification.md "$output_directory/VERIFICATION.md"
juce_source="$build_directory/_deps/juce-src"
cp "$juce_source/LICENSE.md" "$output_directory/LICENSE-JUCE.md"
cp "$juce_source/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt" \
  "$output_directory/LICENSE-VST3-SDK-MIT.txt"

manifest_project=bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj
dotnet run --project "$manifest_project" --configuration "$configuration" -- notices \
  "${NUGET_PACKAGES:-/root/.nuget/packages}" \
  third_party/license-overrides.json "$output_directory" \
  "$publish_directory/Engine/OpenUtau.Vst.Engine.Host.deps.json" \
  "$publish_directory/Editor/app/OpenUtau.deps.json"
dotnet run --project "$manifest_project" --configuration "$configuration" -- \
  create "$output_directory"

archive_name="OpenUtau-DAW${package_version:+-$package_version}-linux-$architecture.zip"
archive="$root/artifacts/$archive_name"
rm -f "$archive"
(cd "$(dirname "$output_directory")" && zip -qrX "$archive" "$(basename "$output_directory")")
dotnet run --project "$manifest_project" --configuration "$configuration" -- verify "$archive"
(cd "$(dirname "$archive")" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")

"$build_directory/plugin_smoke_tests_artefacts/$configuration/plugin_smoke_tests" "$module"
ARCHITECTURE="$architecture" "$root/scripts/verify-linux-package.sh" \
  "$archive" "$output_directory"
echo "Created $archive"
