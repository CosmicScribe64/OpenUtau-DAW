param(
    [Parameter(Mandatory = $true)][string]$Actual,
    [Parameter(Mandatory = $true)][string]$Expected,
    [double]$PeakTolerance = 0.0001,
    [double]$RmsTolerance = 0.00001
)

$ErrorActionPreference = "Stop"

function Read-Wave([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $Path))
    if ($bytes.Length -lt 44 `
        -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne "RIFF" `
        -or [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -ne "WAVE") {
        throw "Not a RIFF/WAVE file: $Path"
    }
    $format = $null
    $channels = $null
    $sampleRate = $null
    $bits = $null
    $dataOffset = $null
    $dataLength = $null
    for ($offset = 12; $offset + 8 -le $bytes.Length;) {
        $id = [Text.Encoding]::ASCII.GetString($bytes, $offset, 4)
        $size = [BitConverter]::ToUInt32($bytes, $offset + 4)
        $payload = $offset + 8
        if ($payload + $size -gt $bytes.Length) { throw "Invalid WAV chunk in $Path" }
        if ($id -eq "fmt ") {
            $format = [BitConverter]::ToUInt16($bytes, $payload)
            $channels = [BitConverter]::ToUInt16($bytes, $payload + 2)
            $sampleRate = [BitConverter]::ToUInt32($bytes, $payload + 4)
            $bits = [BitConverter]::ToUInt16($bytes, $payload + 14)
        } elseif ($id -eq "data") {
            $dataOffset = $payload
            $dataLength = [int]$size
        }
        $offset = $payload + $size + ($size % 2)
    }
    if ($null -eq $format -or $null -eq $dataOffset) { throw "WAV is missing fmt or data: $Path" }
    $bytesPerSample = [int]($bits / 8)
    if ($bytesPerSample -lt 2 -or $dataLength % $bytesPerSample -ne 0) {
        throw "Unsupported WAV sample width: $bits bits"
    }
    $samples = [double[]]::new($dataLength / $bytesPerSample)
    for ($i = 0; $i -lt $samples.Length; $i++) {
        $p = $dataOffset + $i * $bytesPerSample
        if ($format -eq 3 -and $bits -eq 32) {
            $samples[$i] = [BitConverter]::ToSingle($bytes, $p)
        } elseif ($format -eq 1 -and $bits -eq 16) {
            $samples[$i] = [BitConverter]::ToInt16($bytes, $p) / 32768.0
        } elseif ($format -eq 1 -and $bits -eq 24) {
            $value = $bytes[$p] -bor ($bytes[$p + 1] -shl 8) -bor ($bytes[$p + 2] -shl 16)
            if (($value -band 0x800000) -ne 0) { $value = $value -bor -16777216 }
            $samples[$i] = $value / 8388608.0
        } elseif ($format -eq 1 -and $bits -eq 32) {
            $samples[$i] = [BitConverter]::ToInt32($bytes, $p) / 2147483648.0
        } else {
            throw "Unsupported WAV encoding: format $format, $bits bits"
        }
    }
    return [pscustomobject]@{
        Channels = $channels; SampleRate = $sampleRate; Samples = $samples
    }
}

$actualWave = Read-Wave $Actual
$expectedWave = Read-Wave $Expected
if ($actualWave.Channels -ne $expectedWave.Channels `
    -or $actualWave.SampleRate -ne $expectedWave.SampleRate `
    -or $actualWave.Samples.Length -ne $expectedWave.Samples.Length) {
    throw "WAV layout mismatch (channels, sample rate, or sample count)."
}
$peak = 0.0
$sumSquares = 0.0
for ($i = 0; $i -lt $actualWave.Samples.Length; $i++) {
    $difference = [Math]::Abs($actualWave.Samples[$i] - $expectedWave.Samples[$i])
    $peak = [Math]::Max($peak, $difference)
    $sumSquares += $difference * $difference
}
$rms = [Math]::Sqrt($sumSquares / [Math]::Max(1, $actualWave.Samples.Length))
if ($peak -gt $PeakTolerance -or $rms -gt $RmsTolerance) {
    throw "WAV mismatch: peak=$peak (limit $PeakTolerance), RMS=$rms (limit $RmsTolerance)."
}
Write-Host "WAV reference match passed: peak=$peak, RMS=$rms."
