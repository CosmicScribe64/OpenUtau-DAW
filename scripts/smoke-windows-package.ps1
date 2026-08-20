param(
    [string]$BuildDirectory = ".build/windows",
    [string]$Module = "artifacts/windows-x64/OpenUtau DAW.vst3/Contents/x86_64-win/OpenUtau DAW.vst3",
    [string]$DiagnosticsDirectory = ".build/windows-smoke-diagnostics"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory
$modulePath = Join-Path $root $Module
$diagnostics = Join-Path $root $DiagnosticsDirectory
$dataHome = Join-Path $diagnostics "OpenUtauData"
$cacheHome = Join-Path $diagnostics "OpenUtauCache"
$stdoutPath = Join-Path $diagnostics "smoke-stdout.txt"
$stderrPath = Join-Path $diagnostics "smoke-stderr.txt"
$coreHostTrace = Join-Path $diagnostics "corehost-trace.txt"
$eventLogPath = Join-Path $diagnostics "windows-application-events.txt"
$dumpDirectory = Join-Path $diagnostics "dumps"

$smoke = Get-ChildItem $build -Recurse -Filter "plugin_smoke_tests.exe" |
    Select-Object -First 1
if ($null -eq $smoke) { throw "Packaged-editor smoke host was not built." }
if (!(Test-Path $modulePath)) { throw "Packaged VST3 module was not found: $modulePath" }

if (Test-Path $diagnostics) { Remove-Item $diagnostics -Recurse -Force }
New-Item $dataHome -ItemType Directory -Force | Out-Null
New-Item $cacheHome -ItemType Directory -Force | Out-Null
New-Item $dumpDirectory -ItemType Directory -Force | Out-Null

$started = Get-Date
$werKey = "HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\plugin_smoke_tests.exe"
$werConfigured = $false
if ($env:CI -eq "true") {
    try {
        # This key is created only on the disposable hosted runner and removed
        # below. It gives us a native dump when the CLR or UI backend terminates
        # the host before C++ can report an exception.
        New-Item $werKey -Force | Out-Null
        New-ItemProperty $werKey -Name DumpFolder -PropertyType ExpandString `
            -Value $dumpDirectory -Force | Out-Null
        New-ItemProperty $werKey -Name DumpType -PropertyType DWord `
            -Value 2 -Force | Out-Null
        $werConfigured = $true
    } catch {
        Write-Warning "Could not enable CI Windows crash dumps: $_"
    }
}

$savedEnvironment = @{}
foreach ($name in @(
    "OPENUTAU_VST_TEST_CREATE_EDITOR",
    "OPENUTAU_VST_DATA_HOME",
    "OPENUTAU_VST_CACHE_HOME",
    "COREHOST_TRACE",
    "COREHOST_TRACE_VERBOSITY",
    "COREHOST_TRACEFILE"
)) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

$exitCode = 1
try {
    $env:OPENUTAU_VST_TEST_CREATE_EDITOR = "1"
    $env:OPENUTAU_VST_DATA_HOME = $dataHome
    $env:OPENUTAU_VST_CACHE_HOME = $cacheHome
    $env:COREHOST_TRACE = "1"
    $env:COREHOST_TRACE_VERBOSITY = "4"
    $env:COREHOST_TRACEFILE = $coreHostTrace

    # PowerShell 7 can promote a native non-zero exit into a terminating error.
    # Disable that behavior here so diagnostics are collected before failure.
    $savedNativePreference = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
    try {
        & $smoke.FullName $modulePath 1> $stdoutPath 2> $stderrPath
        $exitCode = $LASTEXITCODE
    } finally {
        $PSNativeCommandUseErrorActionPreference = $savedNativePreference
    }
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable(
            $entry.Key, $entry.Value, "Process")
    }
    if ($werConfigured) {
        Remove-Item $werKey -Recurse -Force -ErrorAction SilentlyContinue
    }
}

foreach ($path in @($stdoutPath, $stderrPath)) {
    if (Test-Path $path) {
        Write-Host "--- $(Split-Path -Leaf $path) ---"
        Get-Content $path
    }
}

if ($exitCode -ne 0) {
    $events = Get-WinEvent -FilterHashtable @{
        LogName = "Application"
        StartTime = $started
    } -ErrorAction SilentlyContinue | Where-Object {
        $_.LevelDisplayName -in @("Error", "Critical") -or
        $_.ProviderName -match "Application Error|\.NET Runtime|Windows Error Reporting"
    } | Select-Object -First 40 TimeCreated, ProviderName, Id, LevelDisplayName, Message
    if ($events) {
        $events | Format-List | Out-String -Width 240 | Set-Content $eventLogPath
        Write-Host "--- windows-application-events.txt ---"
        Get-Content $eventLogPath
    }
    $openUtauLogs = Get-ChildItem $dataHome -Recurse -Filter "log*.txt" `
        -ErrorAction SilentlyContinue
    foreach ($log in $openUtauLogs) {
        Write-Host "--- OpenUtau log: $($log.FullName) ---"
        Get-Content $log.FullName -Tail 300
    }
    if (Test-Path $coreHostTrace) {
        Write-Host "--- corehost-trace.txt (tail) ---"
        Get-Content $coreHostTrace -Tail 300
    }
    $unsignedExitCode = [BitConverter]::ToUInt32(
        [BitConverter]::GetBytes([int64]$exitCode), 0)
    $hexExitCode = $unsignedExitCode.ToString("X8")
    throw "Embedded Windows editor smoke test failed with exit code $exitCode (0x$hexExitCode). Diagnostics: $diagnostics"
}

Write-Host "Packaged Windows VST3 discovery, editor creation, processing, and state smoke test passed."
