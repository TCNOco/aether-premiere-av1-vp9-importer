# Confirms the FFmpeg DLLs next to the plug-in are the pinned build.
#
# GitHub CI already hashes the zip. This catches a local swap of ffmpeg\bin
# that would otherwise sail through release-check.bat.

param(
    [string]$BinDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
if (-not $BinDir) { $BinDir = Join-Path $Root "ffmpeg\bin" }
$Pin  = Join-Path $PSScriptRoot "ffmpeg-dll.sha256"

if (-not (Test-Path $Pin))  { throw "missing pin file: $Pin" }
if (-not (Test-Path $BinDir)) { throw "missing FFmpeg folder: $BinDir" }

$expected = @{}
Get-Content -LiteralPath $Pin | ForEach-Object {
    $line = $_.Trim()
    if ($line -match '^([0-9a-fA-F]{64})\s+(\S+)$') {
        $expected[$matches[2]] = $matches[1].ToLowerInvariant()
    }
}

if ($expected.Count -lt 5) {
    throw "pin file $Pin has $($expected.Count) hashes, want at least 5"
}

$failed = 0
foreach ($name in $expected.Keys) {
    $path = Join-Path $BinDir $name
    if (-not (Test-Path $path)) {
        Write-Host "FAIL  $name  missing"
        $failed++
        continue
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected[$name]) {
        Write-Host "FAIL  $name"
        Write-Host "      want $($expected[$name])"
        Write-Host "      got  $actual"
        $failed++
    } else {
        Write-Host "OK    $name"
    }
}

if ($failed -gt 0) {
    Write-Host "$failed FFmpeg DLL(s) do not match tools\ffmpeg-dll.sha256"
    exit 1
}
Write-Host "FFmpeg DLLs match the pin"
exit 0
