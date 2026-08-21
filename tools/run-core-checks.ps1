# Runs the decoding core against the generated media and says whether the
# result is what the plug-in promises.
#
#   powershell -File tools\run-core-checks.ps1
#
# Two kinds of expectation, and the second matters as much as the first:
#
#   accept  - the file is ours, every check inside decoder_test must pass
#   refuse  - the file belongs to Premiere's own importer and must be handed
#             back. A plug-in that grabs more than it should is worse than one
#             that grabs less: it breaks footage that used to work.
#
# The plug-in itself cannot be built here. It needs the Adobe SDK, which may not
# be redistributed, so a build server has no way to get it — see
# THIRD-PARTY-NOTICES.md. What runs is the decoding core, which is deliberately
# free of any Adobe dependency.

param(
    [string]$MediaDir = "build\media",
    [string]$Exe      = "build\decoder_test.exe"
)

# Continue, а не Stop, и это вынужденно: PowerShell 5.1 заворачивает вывод
# родной программы в stderr в объект-ошибку, и при Stop скрипт падает от
# любой болтовни ffmpeg. Свои настоящие ошибки бросаем через throw — он
# работает независимо от этой настройки.
$ErrorActionPreference = "Continue"

if (-not (Test-Path $Exe))      { throw "not built: $Exe" }
if (-not (Test-Path $MediaDir)) { throw "no media: $MediaDir - run tools\make-test-media.ps1" }

$expect = [ordered]@{
    "av1_8bit.mp4"    = "accept"
    "av1_10bit.mp4"   = "accept"
    "vp9.webm"        = "accept"
    "vp9_alpha.webm"  = "accept"
    "audio_only.mka"  = "accept"
    "vfr.mkv"         = "accept"
    "sync.mp4"        = "accept"
    "sync_offset.mp4" = "accept"
    "colour_bt709.mp4" = "accept"
    "audio_only.mp4"  = "refuse"
    "h264.mp4"        = "refuse"
}

# У файлов со вспышкой и щелчком проверка синхронности обязана отработать,
# а не пропуститься: на сломанном файле ни вспышки, ни щелчка не находится,
# и молчаливый пропуск спрятал бы ровно ту поломку, ради которой она есть.
$extraArgs = @{
    "sync.mp4"        = @("--sync")
    "sync_offset.mp4" = @("--sync")
    "colour_bt709.mp4" = @("--colour")
}

$failed = 0

foreach ($name in $expect.Keys) {
    $path = Join-Path $MediaDir $name
    if (-not (Test-Path $path)) { Write-Host ("{0,-18} MISSING" -f $name); $failed++; continue }

    # Без 2>&1: PowerShell 5.1 заворачивает stderr родной программы
    # в ошибку и при ErrorActionPreference=Stop роняет весь скрипт.
    # Всё нужное decoder_test пишет в stdout, а ffmpeg шумит в stderr.
    $more   = if ($extraArgs.ContainsKey($name)) { $extraArgs[$name] } else { @() }
    $output = & $Exe $path @more 2>$null | Out-String
    $code   = $LASTEXITCODE

    if ($expect[$name] -eq "accept") {
        $ok = ($code -eq 0) -and ($output -match "ALL CHECKS PASSED")
        $why = if ($ok) { "all checks passed" } else { "exit $code" }
    } else {
        # Отказ распознаём по сообщению, а не только по коду: ненулевой код
        # бывает и от настоящей поломки, а нам нужен именно вежливый отказ
        $ok = ($output -match "OPEN FAILED")
        $why = if ($ok) { (($output -split "`n") | Select-String "OPEN FAILED").ToString().Trim() } else { "the file was taken" }
    }

    Write-Host ("{0,-18} {1,-7} {2,-5} {3}" -f $name, $expect[$name], $(if ($ok) {"OK"} else {"FAIL"}), $why)
    if (-not $ok) {
        $failed++
        if ($output) { Write-Host ($output.Trim()) }
    }
}

Write-Host ""
if ($failed -gt 0) {
    Write-Host "$failed of $($expect.Count) files behaved wrongly"
    exit 1
}
Write-Host "all $($expect.Count) files behaved as promised"

exit 0
