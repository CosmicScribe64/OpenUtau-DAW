param(
    [string]$Configuration = "Release",
    [string]$BuildDirectory = ".build/windows"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory

& (Join-Path $root "scripts/apply-upstream-patches.ps1")

dotnet build (Join-Path $root "bridge/OpenUtau.Vst.sln") -c $Configuration
dotnet build (Join-Path $root "upstream/OpenUtau/OpenUtau.csproj") -c $Configuration
dotnet run --project (Join-Path $root "bridge/OpenUtau.Vst.Protocol.Tests") `
    -c $Configuration --no-build
dotnet run --project (Join-Path $root "bridge/OpenUtau.Vst.Engine.Tests") `
    -c $Configuration --no-build
dotnet run --project (Join-Path $root "bridge/OpenUtau.Vst.EditorHost.Tests") `
    -c $Configuration --no-build

cmake -S (Join-Path $root "plugin") -B $build -A x64 `
    -DCMAKE_BUILD_TYPE=$Configuration
cmake --build $build --config $Configuration --target `
    audio_ring_buffer_tests engine_bridge_tests plugin_smoke_tests plugin_singing_e2e

# engine_client_tests uses the POSIX fake-editor fixture. The managed E2E test
# covers that fault-injection boundary on Windows. The audible VST3 test uses
# the real managed host, generated classic singer, and Worldline runtime.
ctest --test-dir $build -C $Configuration --output-on-failure `
    -R "^(audio_ring_buffer_tests|engine_bridge_tests|plugin_smoke_tests|plugin_singing_e2e)$"
