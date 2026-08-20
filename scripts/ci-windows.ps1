param(
    [string]$Configuration = "Release",
    [string]$BuildDirectory = ".build/windows"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory

function Assert-NativeSuccess([string]$Operation, [int]$ExitCode) {
    if ($ExitCode -ne 0) {
        throw "$Operation failed with exit code $ExitCode."
    }
}

& (Join-Path $root "scripts/apply-upstream-patches.ps1")

dotnet build (Join-Path $root "bridge/OpenUtau.Vst.sln") -c $Configuration `
    --verbosity minimal -p:WarningLevel=0
Assert-NativeSuccess "Managed solution build" $LASTEXITCODE
dotnet build (Join-Path $root "upstream/OpenUtau/OpenUtau.csproj") -c $Configuration `
    --verbosity minimal -p:WarningLevel=0
Assert-NativeSuccess "OpenUtau desktop build" $LASTEXITCODE
dotnet run --project (Join-Path $root "bridge/OpenUtau.Vst.Protocol.Tests") `
    -c $Configuration --no-build
Assert-NativeSuccess "Protocol tests" $LASTEXITCODE
dotnet run --project (Join-Path $root "bridge/OpenUtau.Vst.Engine.Tests") `
    -c $Configuration --no-build
Assert-NativeSuccess "Engine tests" $LASTEXITCODE
dotnet run --project (Join-Path $root "bridge/OpenUtau.Vst.EditorHost.Tests") `
    -c $Configuration --no-build
Assert-NativeSuccess "Editor host tests" $LASTEXITCODE

cmake -S (Join-Path $root "plugin") -B $build -A x64 `
    -DCMAKE_BUILD_TYPE=$Configuration
Assert-NativeSuccess "Native configure" $LASTEXITCODE
cmake --build $build --config $Configuration --target `
    audio_ring_buffer_tests engine_bridge_tests plugin_smoke_tests plugin_singing_e2e
Assert-NativeSuccess "Native build" $LASTEXITCODE

# engine_client_tests uses the POSIX fake-editor fixture. The managed E2E test
# covers that fault-injection boundary on Windows. The audible VST3 test uses
# the real managed host, generated classic singer, and Worldline runtime.
ctest --test-dir $build -C $Configuration --output-on-failure `
    -R "^(audio_ring_buffer_tests|engine_bridge_tests|plugin_smoke_tests|plugin_singing_e2e)$"
Assert-NativeSuccess "Native and audible VST3 tests" $LASTEXITCODE
