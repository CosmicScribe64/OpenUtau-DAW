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

if ! grep -Fq 'shared/Microsoft.WindowsDesktop.App/$runtimeVersion' scripts/package-windows.ps1; then
  echo "Windows package does not bundle the desktop runtime required by the embedded editor." >&2
  exit 1
fi
if ! grep -Fq 'WindowsBase.dll' scripts/package-windows.ps1; then
  echo "Windows package does not verify its bundled desktop runtime." >&2
  exit 1
fi
if ! grep -Fq 'Package manifest tool restore' scripts/package-windows.ps1; then
  echo "Windows packaging is not independent of the full CI restore step." >&2
  exit 1
fi

for workflow in .github/workflows/build-and-validate.yml .github/workflows/release-candidate.yml; do
  if ! grep -Fq './scripts/smoke-windows-package.ps1' "$workflow"; then
    echo "$workflow does not run the diagnostic packaged-editor smoke test." >&2
    exit 1
  fi
done

if ! grep -Fq 'windows-vst3-package-smoke:' .github/workflows/build-and-validate.yml; then
  echo "Windows packaging smoke must run independently from the exhaustive test job." >&2
  exit 1
fi
if ! grep -Fq '0x$hexExitCode' scripts/smoke-windows-package.ps1; then
  echo "Windows package smoke does not report the native crash exit code." >&2
  exit 1
fi
if ! grep -Fq 'COREHOST_TRACE' scripts/smoke-windows-package.ps1; then
  echo "Windows package smoke does not capture managed-host diagnostics." >&2
  exit 1
fi

license_eol="$(git check-attr eol -- third_party/licenses/Ignore-0.1.50-MIT.txt)"
if [[ "$license_eol" != *': eol: lf' ]]; then
  echo "Reviewed licence files must retain LF endings on Windows: $license_eol" >&2
  exit 1
fi

echo "Windows build policy tests passed."
