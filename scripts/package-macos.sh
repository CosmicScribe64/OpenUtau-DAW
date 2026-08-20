#!/usr/bin/env bash
set -euo pipefail

configuration="${CONFIGURATION:-Release}"
architecture="${ARCHITECTURE:-arm64}"
macos_deployment_target="${MACOS_DEPLOYMENT_TARGET:-14.0}"
runtime_version="${DOTNET_RUNTIME_VERSION:-8.0.30}"
cmake_bin="${CMAKE_BIN:-cmake}"
juce_source_override="${JUCE_SOURCE_DIR:-}"
juce_commit="91ad83ae34a81e0833b1a2b0866f54846370ae53"
managed_publish_mode="${MANAGED_PUBLISH_MODE:-docker}"
package_tool_mode="${PACKAGE_TOOL_MODE:-$managed_publish_mode}"
codesign_identity="${CODESIGN_IDENTITY:--}"
package_version="${PACKAGE_VERSION:-}"
notarize="${NOTARIZE:-0}"
notary_key_path="${NOTARY_KEY_PATH:-}"
notary_key_id="${NOTARY_KEY_ID:-}"
notary_issuer_id="${NOTARY_ISSUER_ID:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
build_directory="${BUILD_DIRECTORY:-$root/.build/macos-$architecture}"
output_directory="${OUTPUT_DIRECTORY:-$root/artifacts/macos-$architecture-package}"
publish_directory="$root/artifacts/.macos-$architecture-publish"
package_relative="${output_directory#"$root"/}"
bundle_name="OpenUtau DAW.vst3"
source_bundle="$build_directory/OpenUtauVst_artefacts/$configuration/VST3/$bundle_name"
bundle="$output_directory/$bundle_name"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "macOS packaging must run on macOS." >&2
  exit 1
fi
if [[ "$architecture" != "arm64" && "$architecture" != "x64" ]]; then
  echo "ARCHITECTURE must be arm64 or x64." >&2
  exit 1
fi
if [[ "$managed_publish_mode" != "docker" && "$managed_publish_mode" != "host" ]]; then
  echo "MANAGED_PUBLISH_MODE must be docker or host." >&2
  exit 1
fi
if [[ "$package_tool_mode" != "docker" && "$package_tool_mode" != "host" ]]; then
  echo "PACKAGE_TOOL_MODE must be docker or host." >&2
  exit 1
fi
if [[ "$notarize" != "0" && "$notarize" != "1" ]]; then
  echo "NOTARIZE must be 0 or 1." >&2
  exit 1
fi
if [[ -n "$package_version" && "$package_version" == *[!A-Za-z0-9._-]* ]]; then
  echo "PACKAGE_VERSION contains unsupported archive-name characters." >&2
  exit 1
fi
if [[ "$notarize" == "1" ]] && { [[ "$codesign_identity" == "-" ]] ||
    [[ -z "$notary_key_path" ]] || [[ -z "$notary_key_id" ]] || [[ -z "$notary_issuer_id" ]]; }; then
  echo "Notarization requires a Developer ID identity and App Store Connect API key settings." >&2
  exit 1
fi
if { [[ "$managed_publish_mode" == "docker" ]] || [[ "$package_tool_mode" == "docker" ]]; } &&
    ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required for the managed publish." >&2
  exit 1
fi
if { [[ "$managed_publish_mode" == "host" ]] || [[ "$package_tool_mode" == "host" ]]; } &&
    ! command -v dotnet >/dev/null 2>&1; then
  echo ".NET 8 is required when a packaging stage runs in host mode." >&2
  exit 1
fi
if [[ "${SKIP_NATIVE_BUILD:-0}" != "1" ]]; then
  if [[ ! -x "$cmake_bin" ]] && ! command -v "$cmake_bin" >/dev/null 2>&1; then
    echo "CMake was not found. Set CMAKE_BIN to a CMake executable." >&2
    exit 1
  fi
elif [[ ! -d "$source_bundle" ]]; then
  echo "SKIP_NATIVE_BUILD=1 requires an existing native VST3 at: $source_bundle" >&2
  exit 1
fi

cd "$root"
./scripts/apply-upstream-patches.sh

# The normal developer path publishes in Docker. Host mode exists for an
# ephemeral GitHub-hosted macOS runner, where Docker is unavailable.
if [[ "$managed_publish_mode" == "docker" ]]; then
  docker compose run --rm dev bash -lc "
    set -euo pipefail
    publish='/workspace/artifacts/.macos-$architecture-publish'
    rm -rf \"\$publish\"
    mkdir -p \"\$publish/Engine\" \"\$publish/EmbeddedEditor/app\"
    dotnet publish bridge/OpenUtau.Vst.Engine.Host/OpenUtau.Vst.Engine.Host.csproj \\
      -c '$configuration' -r 'osx-$architecture' --self-contained false \\
      -p:UseAppHost=false -p:PublishReadyToRun=false -o \"\$publish/Engine\"
    dotnet publish bridge/OpenUtau.Vst.EditorHost/OpenUtau.Vst.EditorHost.csproj \\
      -c '$configuration' -r 'osx-$architecture' --self-contained false \\
      -p:UseAppHost=false -p:PublishReadyToRun=false \\
      -o \"\$publish/EmbeddedEditor/app\"
    runtime_cache='/opt/openutau-macos-runtime-cache/$architecture/$runtime_version'
    if [[ ! -x \"\$runtime_cache/dotnet\" ]]; then
      runtime_temporary=\"\${runtime_cache}.tmp\"
      rm -rf \"\$runtime_temporary\"
      mkdir -p \"\$(dirname \"\$runtime_cache\")\"
      curl --fail --location --silent --show-error \\
        https://dot.net/v1/dotnet-install.sh -o /tmp/openutau-dotnet-install.sh
      bash /tmp/openutau-dotnet-install.sh --runtime dotnet \\
        --version '$runtime_version' --os osx --architecture '$architecture' \\
        --install-dir \"\$runtime_temporary\" --no-path
      mv \"\$runtime_temporary\" \"\$runtime_cache\"
    fi
    cp -a \"\$runtime_cache/.\" \"\$publish/EmbeddedEditor/dotnet/\"
  "
else
  rm -rf "$publish_directory"
  mkdir -p "$build_directory"
  mkdir -p "$publish_directory/Engine" "$publish_directory/EmbeddedEditor/app"
  dotnet publish "$root/bridge/OpenUtau.Vst.Engine.Host/OpenUtau.Vst.Engine.Host.csproj" \
    -c "$configuration" -r "osx-$architecture" --self-contained false \
    -p:UseAppHost=false -p:PublishReadyToRun=false -o "$publish_directory/Engine"
  dotnet publish "$root/bridge/OpenUtau.Vst.EditorHost/OpenUtau.Vst.EditorHost.csproj" \
    -c "$configuration" -r "osx-$architecture" --self-contained false \
    -p:UseAppHost=false -p:PublishReadyToRun=false \
    -o "$publish_directory/EmbeddedEditor/app"
  runtime_cache="$build_directory/private-dotnet-$runtime_version-$architecture"
  if [[ ! -x "$runtime_cache/dotnet" ]]; then
    runtime_temporary="${runtime_cache}.tmp"
    rm -rf "$runtime_temporary"
    curl --fail --location --silent --show-error \
      https://dot.net/v1/dotnet-install.sh -o "$build_directory/dotnet-install.sh"
    bash "$build_directory/dotnet-install.sh" --runtime dotnet \
      --version "$runtime_version" --os osx --architecture "$architecture" \
      --install-dir "$runtime_temporary" --no-path
    mv "$runtime_temporary" "$runtime_cache"
  fi
  cp -a "$runtime_cache/." "$publish_directory/EmbeddedEditor/dotnet/"
fi

if [[ "${SKIP_NATIVE_BUILD:-0}" == "1" ]]; then
  echo "Reassembling a package from the existing native VST3; native rebuild skipped."
else
  cmake_arguments=(
    -S "$root/plugin"
    -B "$build_directory"
    -DCMAKE_BUILD_TYPE="$configuration"
    -DCMAKE_OSX_ARCHITECTURES="$architecture"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$macos_deployment_target"
  )
  if [[ -n "$juce_source_override" ]]; then
    if [[ "$(git -C "$juce_source_override" rev-parse HEAD)" != "$juce_commit" ]]; then
      echo "JUCE_SOURCE_DIR is not the pinned JUCE 8.0.15 commit." >&2
      exit 1
    fi
    cmake_arguments+=("-DFETCHCONTENT_SOURCE_DIR_JUCE=$juce_source_override")
  fi
  "$cmake_bin" "${cmake_arguments[@]}"
  "$cmake_bin" --build "$build_directory" \
    --target OpenUtauVst_VST3 --config "$configuration"
fi

rm -rf "$output_directory"
mkdir -p "$output_directory"
ditto "$source_bundle" "$bundle"
mkdir -p "$bundle/Contents/Resources"
ditto "$publish_directory/Engine" "$bundle/Contents/Resources/Engine"
ditto "$publish_directory/EmbeddedEditor" \
  "$bundle/Contents/Resources/EmbeddedEditor"

required_files=(
  "$bundle/Contents/MacOS/OpenUtau DAW"
  "$bundle/Contents/Resources/Engine/OpenUtau.Vst.Engine.Host.dll"
  "$bundle/Contents/Resources/Engine/OpenUtau.Plugin.Builtin.dll"
  "$bundle/Contents/Resources/Engine/libworldline.dylib"
  "$bundle/Contents/Resources/EmbeddedEditor/app/OpenUtau.Vst.EditorHost.dll"
  "$bundle/Contents/Resources/EmbeddedEditor/app/OpenUtau.dll"
  "$bundle/Contents/Resources/EmbeddedEditor/dotnet/dotnet"
  "$bundle/Contents/Resources/EmbeddedEditor/dotnet/LICENSE.txt"
  "$bundle/Contents/Resources/EmbeddedEditor/dotnet/ThirdPartyNotices.txt"
  "$bundle/Contents/Resources/EmbeddedEditor/dotnet/host/fxr/$runtime_version/libhostfxr.dylib"
)
for required in "${required_files[@]}"; do
  if [[ ! -f "$required" ]]; then
    echo "Package is missing required file: $required" >&2
    exit 1
  fi
done

cp "$root/upstream/LICENSE.txt" "$output_directory/LICENSE-OPENUTAU-MIT.txt"
cp "$root/LICENSE" "$output_directory/LICENSE-OPENUTAU-DAW-AGPL-3.0.txt"
cp "$root/docs/licensing.md" "$output_directory/DISTRIBUTION-LICENSING.md"
cp "$root/docs/source-offer.md" "$output_directory/CORRESPONDING-SOURCE.md"
cp "$root/docs/verification.md" "$output_directory/VERIFICATION.md"
if [[ -n "$juce_source_override" ]]; then
  juce_source="$juce_source_override"
else
  juce_source="$build_directory/_deps/juce-src"
fi
cp "$juce_source/LICENSE.md" "$output_directory/LICENSE-JUCE.md"
cp "$juce_source/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt" \
  "$output_directory/LICENSE-VST3-SDK-MIT.txt"

notice_tool="/workspace/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj"
if [[ "$package_tool_mode" == "docker" ]]; then
  docker compose run --rm dev dotnet run --project "$notice_tool" \
    --configuration "$configuration" -- notices \
    /root/.nuget/packages \
    /workspace/third_party/license-overrides.json \
    "/workspace/$package_relative" \
    "/workspace/${publish_directory#"$root"/}/Engine/OpenUtau.Vst.Engine.Host.deps.json" \
    "/workspace/${publish_directory#"$root"/}/EmbeddedEditor/app/OpenUtau.Vst.EditorHost.deps.json"
else
  dotnet run --project "$root/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj" \
    --configuration "$configuration" -- notices \
    "${NUGET_PACKAGES:-$HOME/.nuget/packages}" \
    "$root/third_party/license-overrides.json" \
    "$output_directory" \
    "$publish_directory/Engine/OpenUtau.Vst.Engine.Host.deps.json" \
    "$publish_directory/EmbeddedEditor/app/OpenUtau.Vst.EditorHost.deps.json"
fi

if [[ "$codesign_identity" == "-" ]]; then
  codesign --force --deep --sign - "$bundle"
else
  codesign --force --deep --options runtime --timestamp --sign "$codesign_identity" "$bundle"
fi
codesign --verify --deep --strict --verbose=2 "$bundle"
if [[ "$notarize" == "1" ]]; then
  notary_archive="$build_directory/OpenUtau-DAW-notary-submission.zip"
  rm -f "$notary_archive"
  ditto -c -k --keepParent --norsrc "$bundle" "$notary_archive"
  xcrun notarytool submit "$notary_archive" \
    --key "$notary_key_path" \
    --key-id "$notary_key_id" \
    --issuer "$notary_issuer_id" \
    --wait
  xcrun stapler staple "$bundle"
  xcrun stapler validate "$bundle"
fi
manifest_project="$notice_tool"
if [[ "$package_tool_mode" == "docker" ]]; then
  docker compose run --rm dev dotnet run --project "$manifest_project" \
    --configuration "$configuration" -- create "/workspace/$package_relative"
else
  dotnet run --project "$root/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj" \
    --configuration "$configuration" -- create "$output_directory"
fi

if [[ -n "$package_version" ]]; then
  archive="$root/artifacts/OpenUtau-DAW-$package_version-macos-$architecture.zip"
else
  archive="$root/artifacts/OpenUtau-DAW-macos-$architecture.zip"
fi
rm -f "$archive"
# Resource forks and Finder metadata create AppleDouble `._*` ZIP entries.
# They are not distributable plug-in payload and would invalidate the
# content-addressed manifest, so omit them deliberately.
ditto -c -k --keepParent --norsrc "$output_directory" "$archive"
archive_relative="${archive#"$root"/}"
if [[ "$package_tool_mode" == "docker" ]]; then
  docker compose run --rm dev dotnet run --project "$manifest_project" \
    --configuration "$configuration" -- verify "/workspace/$archive_relative"
else
  dotnet run --project "$root/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj" \
    --configuration "$configuration" -- verify "$archive"
fi
archive_directory="$(dirname "$archive")"
(cd "$archive_directory" && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
ARCHITECTURE="$architecture" PACKAGE_TOOL_MODE="$package_tool_mode" \
  REQUIRE_NOTARIZED="$notarize" \
  "$root/scripts/verify-macos-package.sh" "$archive" "$output_directory"
echo "Created $archive"
