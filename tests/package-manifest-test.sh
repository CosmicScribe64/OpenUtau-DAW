#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dotnet run --project "$root/bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj" \
  --configuration Release --no-restore -- self-test
