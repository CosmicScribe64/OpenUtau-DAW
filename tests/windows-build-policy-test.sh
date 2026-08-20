#!/usr/bin/env bash
set -euo pipefail

editor_tfm="$(dotnet msbuild \
  bridge/OpenUtau.Vst.EditorHost/OpenUtau.Vst.EditorHost.csproj \
  -nologo -getProperty:TargetFramework -p:OS=Windows_NT)"
test_tfm="$(dotnet msbuild \
  bridge/OpenUtau.Vst.EditorHost.Tests/OpenUtau.Vst.EditorHost.Tests.csproj \
  -nologo -getProperty:TargetFramework -p:OS=Windows_NT)"

if [[ "$editor_tfm" != net8.0-windows ]]; then
  echo "Windows editor host resolved unexpected TFM: $editor_tfm" >&2
  exit 1
fi
if [[ "$test_tfm" != net8.0-windows ]]; then
  echo "Windows editor host tests resolved unexpected TFM: $test_tfm" >&2
  exit 1
fi

if ! grep -Fq '/W4 /WX /wd4324' plugin/CMakeLists.txt; then
  echo "MSVC does not narrowly exempt intentional alignment padding." >&2
  exit 1
fi

for script in scripts/ci-windows.ps1 scripts/package-windows.ps1; do
  if ! grep -Fq 'Assert-NativeSuccess' "$script"; then
    echo "$script does not fail fast after native tool failures." >&2
    exit 1
  fi
done

echo "Windows build policy tests passed."
