param(
    [Parameter(Mandatory = $true)][string]$FlStudioExe,
    [Parameter(Mandatory = $true)][string]$FixtureFlp,
    [Parameter(Mandatory = $true)][string]$ExpectedWav,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$PluginBundle,
    [int]$TimeoutSeconds = 900,
    [double]$PeakTolerance = 0.0001,
    [double]$RmsTolerance = 0.00001
)

$ErrorActionPreference = "Stop"
if (!(Test-Path $FlStudioExe)) { throw "FL Studio executable not found: $FlStudioExe" }
if (!(Test-Path $FixtureFlp)) { throw "FL Studio fixture not found: $FixtureFlp" }
if (!(Test-Path $ExpectedWav)) { throw "Approved WAV fixture not found: $ExpectedWav" }
if ($PluginBundle -and !(Test-Path $PluginBundle)) {
    throw "Packaged plugin bundle not found: $PluginBundle"
}
if ($TimeoutSeconds -lt 1) { throw "TimeoutSeconds must be positive." }
New-Item $OutputDirectory -ItemType Directory -Force | Out-Null

$startedAt = [DateTime]::UtcNow
$process = Start-Process -FilePath $FlStudioExe -ArgumentList @(
    "/D", "/R", "/Ewav", "/O`"$OutputDirectory`"", "`"$FixtureFlp`""
) -PassThru
if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill($true)
    throw "FL Studio export exceeded the $TimeoutSeconds second timeout."
}
if ($process.ExitCode -ne 0) { throw "FL Studio export exited with $($process.ExitCode)." }

$waves = @(Get-ChildItem $OutputDirectory -Filter *.wav | Where-Object {
    $_.LastWriteTimeUtc -ge $startedAt
})
if ($waves.Count -ne 1 -or $waves[0].Length -le 44) {
    throw "FL Studio must produce exactly one new, valid WAV file; found $($waves.Count)."
}
$wave = $waves[0]
& (Join-Path $PSScriptRoot "compare-wav.ps1") -Actual $wave.FullName `
    -Expected $ExpectedWav -PeakTolerance $PeakTolerance -RmsTolerance $RmsTolerance

$evidence = [ordered]@{
    completedAtUtc = [DateTime]::UtcNow.ToString("o")
    flStudio = (Resolve-Path $FlStudioExe).Path
    flStudioSha256 = (Get-FileHash $FlStudioExe -Algorithm SHA256).Hash
    fixtureFlpSha256 = (Get-FileHash $FixtureFlp -Algorithm SHA256).Hash
    expectedWavSha256 = (Get-FileHash $ExpectedWav -Algorithm SHA256).Hash
    actualWav = $wave.Name
    actualWavSha256 = (Get-FileHash $wave.FullName -Algorithm SHA256).Hash
    pluginBundle = if ($PluginBundle) { (Resolve-Path $PluginBundle).Path } else { $null }
    pluginFilesSha256 = if ($PluginBundle) {
        @(Get-ChildItem $PluginBundle -File -Recurse | Sort-Object FullName | ForEach-Object {
            [ordered]@{ path = $_.FullName.Substring((Resolve-Path $PluginBundle).Path.Length); sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
        })
    } else { @() }
    peakTolerance = $PeakTolerance
    rmsTolerance = $RmsTolerance
    timeoutSeconds = $TimeoutSeconds
}
$evidence | ConvertTo-Json -Depth 5 | Set-Content `
    (Join-Path $OutputDirectory "evidence.json") -Encoding utf8
Write-Host "FL Studio E2E render produced $($wave.FullName) ($($wave.Length) bytes)."
