param(
    [string]$Configuration = "Release",
    [string]$BuildDirectory = ".build/windows",
    [string]$OutputDirectory = "artifacts/windows-x64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory
$output = Join-Path $root $OutputDirectory
$publish = Join-Path $build "publish"

& (Join-Path $root "scripts/apply-upstream-patches.ps1")

cmake -S (Join-Path $root "plugin") -B $build -A x64
cmake --build $build --config $Configuration --target OpenUtauVst_VST3

dotnet publish (Join-Path $root "bridge/OpenUtau.Vst.Engine.Host/OpenUtau.Vst.Engine.Host.csproj") `
    -c $Configuration -r win-x64 --self-contained false `
    -p:UseAppHost=false -p:PublishReadyToRun=false `
    -o (Join-Path $publish "Engine")
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

# Copy only the .NET 8 runtime needed by the two managed components. This
# avoids installing .NET on an FL Studio user's machine and avoids shipping the
# much larger SDK that is present on the build runner.
$dotnetExe = (Get-Command dotnet).Source
$dotnetRoot = Split-Path -Parent $dotnetExe
$runtimeVersions = @(& $dotnetExe --list-runtimes | ForEach-Object {
    if ($_ -match '^Microsoft\.NETCore\.App (8\.0\.\d+) ') {
        [Version]$Matches[1]
    }
})
if ($runtimeVersions.Count -eq 0) {
    throw "A Microsoft.NETCore.App 8.0 runtime is required for packaging."
}
$runtimeVersion = ($runtimeVersions | Sort-Object -Descending | Select-Object -First 1).ToString()
$runtimeSource = Join-Path $dotnetRoot "shared/Microsoft.NETCore.App/$runtimeVersion"
$fxrSource = Join-Path $dotnetRoot "host/fxr/$runtimeVersion"
if (!(Test-Path $runtimeSource) -or !(Test-Path $fxrSource)) {
    throw "Could not locate the selected private .NET runtime $runtimeVersion."
}
$privateRuntime = Join-Path $embeddedPublish "dotnet"
New-Item (Join-Path $privateRuntime "host/fxr/$runtimeVersion") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $privateRuntime "shared/Microsoft.NETCore.App/$runtimeVersion") -ItemType Directory -Force | Out-Null
Copy-Item $dotnetExe (Join-Path $privateRuntime "dotnet.exe")
Copy-Item (Join-Path $fxrSource "*") (Join-Path $privateRuntime "host/fxr/$runtimeVersion") -Recurse
Copy-Item (Join-Path $runtimeSource "*") `
    (Join-Path $privateRuntime "shared/Microsoft.NETCore.App/$runtimeVersion") -Recurse
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
if ($LASTEXITCODE -ne 0) { throw "Could not generate third-party runtime notices." }

& dotnet run --project $noticeTool `
    --configuration $Configuration --no-restore -- create $output
if ($LASTEXITCODE -ne 0) { throw "Could not create package manifest." }

$archive = Join-Path (Split-Path $output -Parent) "OpenUtau-DAW-windows-x64.zip"
if (Test-Path $archive) { Remove-Item $archive -Force }
Compress-Archive -Path (Join-Path $output "*") -DestinationPath $archive
& dotnet run --project $noticeTool `
    --configuration $Configuration --no-restore -- verify $archive
if ($LASTEXITCODE -ne 0) { throw "Packaged archive failed manifest verification." }
(Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant() + "  " + (Split-Path -Leaf $archive) |
    Set-Content -NoNewline "$archive.sha256"
Write-Host "Created $archive"
