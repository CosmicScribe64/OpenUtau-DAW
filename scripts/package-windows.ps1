param(
    [string]$Configuration = "Release",
    [string]$BuildDirectory = ".build/windows",
    [string]$OutputDirectory = "artifacts/windows-x64",
    [string]$PackageVersion = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory
$output = Join-Path $root $OutputDirectory
$publish = Join-Path $build "publish"

function Assert-NativeSuccess([string]$Operation, [int]$ExitCode) {
    if ($ExitCode -ne 0) {
        throw "$Operation failed with exit code $ExitCode."
    }
}

if ($PackageVersion -and $PackageVersion -notmatch '^[A-Za-z0-9._-]+$') {
    throw "PackageVersion contains unsupported characters: $PackageVersion"
}

& (Join-Path $root "scripts/apply-upstream-patches.ps1")

cmake -S (Join-Path $root "plugin") -B $build -A x64
Assert-NativeSuccess "Native configure" $LASTEXITCODE
cmake --build $build --config $Configuration --target OpenUtauVst_VST3
Assert-NativeSuccess "VST3 build" $LASTEXITCODE

dotnet publish (Join-Path $root "bridge/OpenUtau.Vst.Engine.Host/OpenUtau.Vst.Engine.Host.csproj") `
    -c $Configuration -r win-x64 --self-contained false `
    -p:UseAppHost=false -p:PublishReadyToRun=false `
    -o (Join-Path $publish "Engine")
Assert-NativeSuccess "Engine host publish" $LASTEXITCODE
$enginePublish = Join-Path $publish "Engine"
foreach ($required in @("OpenUtau.Vst.Engine.Host.dll", "OpenUtau.Plugin.Builtin.dll", "worldline.dll")) {
    if (!(Test-Path (Join-Path $enginePublish $required))) {
        throw "Engine publish is missing required runtime file: $required"
    }
}

$embeddedPublish = Join-Path $publish "EmbeddedEditor"
$embeddedApp = Join-Path $embeddedPublish "app"
dotnet publish (Join-Path $root "bridge/OpenUtau.Vst.EditorHost/OpenUtau.Vst.EditorHost.csproj") `
    -c $Configuration -r win-x64 --self-contained false `
    -p:UseAppHost=false -p:PublishReadyToRun=false -o $embeddedApp
Assert-NativeSuccess "Embedded editor publish" $LASTEXITCODE
foreach ($required in @(
    "OpenUtau.Vst.EditorHost.dll",
    "OpenUtau.Vst.EditorHost.runtimeconfig.json",
    "OpenUtau.dll",
    "Avalonia.Native.dll"
)) {
    if (!(Test-Path (Join-Path $embeddedApp $required))) {
        throw "Embedded editor publish is missing required file: $required"
    }
}

# Copy only the .NET 8 runtimes needed by the two managed components. The
# engine uses Microsoft.NETCore.App while the net8.0-windows editor also needs
# Microsoft.WindowsDesktop.App. This avoids requiring users to install .NET
# and avoids shipping the much larger SDK that is present on the build runner.
$dotnetExe = (Get-Command dotnet).Source
$dotnetRoot = Split-Path -Parent $dotnetExe
$runtimeInventory = @(& $dotnetExe --list-runtimes)
Assert-NativeSuccess ".NET runtime inventory" $LASTEXITCODE
$coreRuntimeVersions = @($runtimeInventory | ForEach-Object {
    if ($_ -match '^Microsoft\.NETCore\.App (8\.0\.\d+) ') {
        [Version]$Matches[1]
    }
})
$desktopRuntimeVersions = @($runtimeInventory | ForEach-Object {
    if ($_ -match '^Microsoft\.WindowsDesktop\.App (8\.0\.\d+) ') {
        [Version]$Matches[1]
    }
})
$sharedRuntimeVersions = @($coreRuntimeVersions | Where-Object {
    $desktopRuntimeVersions -contains $_
})
if ($sharedRuntimeVersions.Count -eq 0) {
    throw "Matching Microsoft.NETCore.App and Microsoft.WindowsDesktop.App 8.0 runtimes are required for packaging."
}
$runtimeVersion = ($sharedRuntimeVersions | Sort-Object -Descending | Select-Object -First 1).ToString()
$coreRuntimeSource = Join-Path $dotnetRoot "shared/Microsoft.NETCore.App/$runtimeVersion"
$desktopRuntimeSource = Join-Path $dotnetRoot "shared/Microsoft.WindowsDesktop.App/$runtimeVersion"
$fxrSource = Join-Path $dotnetRoot "host/fxr/$runtimeVersion"
if (!(Test-Path $coreRuntimeSource) -or
    !(Test-Path $desktopRuntimeSource) -or
    !(Test-Path $fxrSource)) {
    throw "Could not locate the selected private .NET runtime $runtimeVersion."
}
$privateRuntime = Join-Path $embeddedPublish "dotnet"
New-Item (Join-Path $privateRuntime "host/fxr/$runtimeVersion") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $privateRuntime "shared/Microsoft.NETCore.App/$runtimeVersion") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $privateRuntime "shared/Microsoft.WindowsDesktop.App/$runtimeVersion") -ItemType Directory -Force | Out-Null
Copy-Item $dotnetExe (Join-Path $privateRuntime "dotnet.exe")
Copy-Item (Join-Path $fxrSource "*") (Join-Path $privateRuntime "host/fxr/$runtimeVersion") -Recurse
Copy-Item (Join-Path $coreRuntimeSource "*") `
    (Join-Path $privateRuntime "shared/Microsoft.NETCore.App/$runtimeVersion") -Recurse
Copy-Item (Join-Path $desktopRuntimeSource "*") `
    (Join-Path $privateRuntime "shared/Microsoft.WindowsDesktop.App/$runtimeVersion") -Recurse
if (!(Test-Path (Join-Path $privateRuntime "shared/Microsoft.WindowsDesktop.App/$runtimeVersion/WindowsBase.dll"))) {
    throw "Private .NET runtime is missing the Windows desktop framework."
}
foreach ($notice in @("LICENSE.txt", "ThirdPartyNotices.txt")) {
    $noticePath = Join-Path $dotnetRoot $notice
    if (!(Test-Path $noticePath)) { throw "Private .NET runtime notice is missing: $noticePath" }
    Copy-Item $noticePath $privateRuntime
}

if (Test-Path $output) { Remove-Item $output -Recurse -Force }
New-Item $output -ItemType Directory | Out-Null
$sourceBundle = Join-Path $build "OpenUtauVst_artefacts/$Configuration/VST3/OpenUtau DAW.vst3"
$bundle = Join-Path $output "OpenUtau DAW.vst3"
Copy-Item $sourceBundle $bundle -Recurse
$resources = Join-Path $bundle "Contents/Resources"
New-Item $resources -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $publish "Engine") (Join-Path $resources "Engine") -Recurse
Copy-Item $embeddedPublish (Join-Path $resources "EmbeddedEditor") -Recurse
Copy-Item (Join-Path $root "upstream/LICENSE.txt") (Join-Path $output "LICENSE-OPENUTAU-MIT.txt")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $output "LICENSE-OPENUTAU-DAW-AGPL-3.0.txt")
$juceSource = Join-Path $build "_deps/juce-src"
$juceLicense = Join-Path $juceSource "LICENSE.md"
$vst3License = Join-Path $juceSource "modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"
if (!(Test-Path $juceLicense)) { throw "Pinned JUCE licence notice was not found." }
if (!(Test-Path $vst3License)) { throw "Embedded VST3 SDK licence notice was not found." }
Copy-Item $juceLicense (Join-Path $output "LICENSE-JUCE.md")
Copy-Item $vst3License (Join-Path $output "LICENSE-VST3-SDK-MIT.txt")
Copy-Item (Join-Path $root "docs/licensing.md") (Join-Path $output "DISTRIBUTION-LICENSING.md")
Copy-Item (Join-Path $root "docs/source-offer.md") (Join-Path $output "CORRESPONDING-SOURCE.md")
Copy-Item (Join-Path $root "docs/verification.md") (Join-Path $output "VERIFICATION.md")

$nugetCache = if ($env:NUGET_PACKAGES) {
    $env:NUGET_PACKAGES
} else {
    Join-Path ([Environment]::GetFolderPath("UserProfile")) ".nuget/packages"
}
$noticeTool = Join-Path $root "bridge/OpenUtau.Vst.PackageManifest/OpenUtau.Vst.PackageManifest.csproj"
& dotnet run --project $noticeTool --configuration $Configuration --no-restore -- notices `
    $nugetCache `
    (Join-Path $root "third_party/license-overrides.json") `
    $output `
    (Join-Path $enginePublish "OpenUtau.Vst.Engine.Host.deps.json") `
    (Join-Path $embeddedApp "OpenUtau.Vst.EditorHost.deps.json")
Assert-NativeSuccess "Third-party runtime notice generation" $LASTEXITCODE

& dotnet run --project $noticeTool `
    --configuration $Configuration --no-restore -- create $output
Assert-NativeSuccess "Package manifest creation" $LASTEXITCODE

$archiveName = if ($PackageVersion) {
    "OpenUtau-DAW-$PackageVersion-windows-x64.zip"
} else {
    "OpenUtau-DAW-windows-x64.zip"
}
$archive = Join-Path (Split-Path $output -Parent) $archiveName
if (Test-Path $archive) { Remove-Item $archive -Force }
Compress-Archive -Path (Join-Path $output "*") -DestinationPath $archive
& dotnet run --project $noticeTool `
    --configuration $Configuration --no-restore -- verify $archive
Assert-NativeSuccess "Packaged archive manifest verification" $LASTEXITCODE
(Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant() + "  " + (Split-Path -Leaf $archive) |
    Set-Content -NoNewline "$archive.sha256"
Write-Host "Created $archive"
