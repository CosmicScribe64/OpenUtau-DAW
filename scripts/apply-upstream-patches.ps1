$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$expected = "29e0e16d1623cda79ba7c3724614d6129ba3b9d5"
$actual = git -C (Join-Path $root "upstream") rev-parse HEAD
if ($actual -ne $expected) {
    throw "OpenUtau baseline mismatch: expected $expected, found $actual"
}
$patch = Join-Path $root "patches/openutau-vst.patch"
git -C (Join-Path $root "upstream") apply --unidiff-zero --reverse --check $patch 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "OpenUtau VST adapter patch is already applied."
    exit 0
}
git -C (Join-Path $root "upstream") apply --unidiff-zero --check $patch
if ($LASTEXITCODE -ne 0) {
    throw "OpenUtau working tree is neither clean nor correctly patched."
}
git -C (Join-Path $root "upstream") apply --unidiff-zero $patch
Write-Host "Applied OpenUtau VST adapter patch."
