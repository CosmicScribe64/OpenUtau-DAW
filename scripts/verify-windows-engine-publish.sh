#!/bin/sh
set -eu

output="$(mktemp -d)"
dotnet publish bridge/OpenUtau.Vst.Engine.Host/OpenUtau.Vst.Engine.Host.csproj \
  --configuration Release --runtime win-x64 --self-contained true \
  -p:PublishSingleFile=false -p:WarningLevel=0 --output "$output" >/dev/null

for required in \
  OpenUtau.Vst.Engine.Host.exe \
  OpenUtau.Plugin.Builtin.dll \
  worldline.dll
do
  test -f "$output/$required"
done

echo "Windows engine publish contains executable, phonemizers, and Worldline runtime."
