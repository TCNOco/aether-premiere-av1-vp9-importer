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
    [string]$Exe      = "build\decoder_test.exe",
    [string]$Fuzz     = "build\fuzz_test.exe",
    [string]$Settings = "build\settings_test.exe",
    [string]$Preview  = "build\preview_cache_test.exe",
    [string]$Math     = "build\importer_math_test.exe",
    [switch]$NoFuzz
)

# Continue, а не Stop, и это вынужденно: PowerShell 5.1 заворачивает вывод
# родной программы в stderr в объект-ошибку, и при Stop скрипт падает от
# любой болтовни ffmpeg. Свои настоящие ошибки бросаем через throw — он
# работает независимо от этой настройки.
$ErrorActionPreference = "Continue"

if (-not (Test-Path $Exe))      { throw "not built: $Exe" }
if (-not (Test-Path $Settings)) { throw "not built: $Settings" }
if (-not (Test-Path $Preview))  { throw "not built: $Preview" }
if (-not (Test-Path $Math))     { throw "not built: $Math" }
if (-not (Test-Path $MediaDir)) { throw "no media: $MediaDir - run tools\make-test-media.ps1" }

$settingsOutput = & $Settings | Out-String
if ($LASTEXITCODE -ne 0 -or $settingsOutput -notmatch "ALL SETTINGS CHECKS PASSED") {
    Write-Host $settingsOutput.Trim()
    throw "settings checks failed"
}
Write-Host "settings.ini       preserve OK"

$previewOutput = & $Preview | Out-String
if ($LASTEXITCODE -ne 0 -or $previewOutput -notmatch "ALL PREVIEW CACHE CHECKS PASSED") {
    Write-Host $previewOutput.Trim()
    throw "preview cache checks failed"
}
Write-Host "preview cache      format/key/stride OK"

$mathOutput = & $Math | Out-String
if ($LASTEXITCODE -ne 0 -or $mathOutput -notmatch "ALL IMPORTER MATH CHECKS PASSED") {
    Write-Host $mathOutput.Trim()
    throw "importer math checks failed"
}
Write-Host "importer duration  overflow OK"

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
    "colour_unspecified.mp4" = "accept"
    "hdr_pq.mkv"      = "accept"
    "audio_tracks_differ.mp4" = "accept"
    "audio_65ch.mkv"  = "accept"
    "audio_only.mp4"  = "refuse"
    "h264.mp4"        = "refuse"
    "concat_disguise.mp4" = "refuse"
}

# У файлов со вспышкой и щелчком проверка синхронности обязана отработать,
# а не пропуститься: на сломанном файле ни вспышки, ни щелчка не находится,
# и молчаливый пропуск спрятал бы ровно ту поломку, ради которой она есть.
$extraArgs = @{
    "sync.mp4"        = @("--sync")
    "sync_offset.mp4" = @("--sync")
    "colour_bt709.mp4" = @("--colour")
    "colour_unspecified.mp4" = @("--transfer-unspecified")
    "audio_65ch.mkv"  = @("--audio-cap")
}

# A path that is not Latin, in a folder that is not Latin either, with a space
# in it. The same media as av1_8bit.mp4 - what is under test is the PATH.
#
# Every other name above is ASCII, and that gap let a real bug live: the log
# printed the path with %S, MSVC converted it through the C locale, and the
# line stopped dead at the first Cyrillic character without even a newline.
# The one line you want when a file will not open, missing for everyone whose
# footage is not in Latin folders.
#
# Built from character codes on purpose: this file has no byte-order mark, and
# Windows PowerShell 5.1 reads such a file in the system ANSI code page, so a
# typed Cyrillic literal would become mojibake and point at a file that does
# not exist. Same reasoning as in make-test-media.ps1, same helper.
function Cyr([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }

$cyrPath = (Cyr @(0x043F,0x0430,0x043F,0x043A,0x0430)) + " test\" +
           (Cyr @(0x0432,0x0438,0x0434,0x0435,0x043E)) + "_av1.mp4"
$expect[$cyrPath] = "accept"

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

# --- порченые файлы -------------------------------------------------------
#
# Всё выше кормит ядро правильными файлами. Здесь наоборот: обрывки, шум,
# перевёрнутые биты. Проверяется единственное — что процесс доживает до конца;
# отказаться открыть такой файл совершенно законно.
#
# Это не блажь: кадр без размеров однажды уже доходил до swscale, а тот на
# такое не отвечает ошибкой, а вызывает av_assert0 и заканчивает процесс —
# внутри Premiere это выглядело как «Premiere сам закрылся».
if ($NoFuzz) { exit 0 }

if (-not (Test-Path $Fuzz)) {
    Write-Host "no $Fuzz - skipping the damaged-file pass"
    exit 0
}

Write-Host ""
Write-Host "damaged files:"

# По одному файлу на каждый разбор контейнера, по два зерна на каждый.
# Больше — дольше, а на сборочном сервере это каждый пуш.
$fuzzTargets = @("av1_8bit.mp4", "vp9_alpha.webm", "vfr.mkv")
$seeds = @(1, 20260821)
$fuzzFailed = 0

foreach ($name in $fuzzTargets) {
    $path = Join-Path $MediaDir $name
    if (-not (Test-Path $path)) { continue }

    foreach ($seed in $seeds) {
        $out = & $Fuzz $path --seed $seed 2>$null | Out-String
        $code = $LASTEXITCODE

        # Падение видно по обрыву: последняя строка не дописана
        $ok = ($code -eq 0) -and ($out -match "all survived")
        $tail = ($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 1)

        Write-Host ("  {0,-18} seed {1,-9} {2,-5} {3}" -f $name, $seed,
                    $(if ($ok) {"OK"} else {"FAIL"}), $tail.Trim())
        if (-not $ok) {
            $fuzzFailed++
            Write-Host ($out.Trim())
        }
    }
}

# Порченые копии рядом с исходниками не нужны
Get-ChildItem $MediaDir -Filter "*-damaged.*" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

if ($fuzzFailed -gt 0) {
    Write-Host ""
    Write-Host "$fuzzFailed damaged-file runs did not survive"
    exit 1
}
exit 0
