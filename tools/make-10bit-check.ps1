# Делает набор для ЖИВОЙ проверки в Premiere того, что поддельным хостом
# проверить нельзя по устройству задачи.
#
#   powershell -File tools\make-10bit-check.ps1
#
# Зачем он нужен именно сейчас.
#
# Десятибитный кадр теперь уходит хосту двумя плоскостями (P010) вместо
# пересчёта в BGRA64 — 1052 мс против 159 на 120 кадрах 1440p. Но адрес второй
# плоскости плагин ВЫЧИСЛЯЕТ сам: в SDK его нет ни в одном наборе, а
# GetYUV420PlanarBuffers описан только для восьмибитных трёх плоскостей.
# Вычисление сверяется через GetSize и при несходимости отказывается, но
# подтвердить раскладку может только настоящий хост.
#
# Картинка во всех файлах ОДНА И ТА ЖЕ. Отличается только то, как она записана.
# Цветные полосы взяты не для красоты: перестановка U и V на них видна сразу
# и на любом мониторе, а на живой съёмке сошла бы за цветокоррекцию.

param(
    [string]$OutDir = "build\media\10bit-check",
    [string]$FFmpeg = "ffmpeg\bin\ffmpeg.exe",
    [int]$Seconds   = 20
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $FFmpeg)) {
    $fallback = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
    if (-not $fallback) { throw "ffmpeg not found: neither $FFmpeg nor one on PATH" }
    $FFmpeg = $fallback
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

# Содержимое кадра:
#   * цветные полосы SMPTE — ловят перестановку цветности и уехавшую матрицу;
#   * полоса плавного градиента внизу — ловит потерю разрядности: у восьми
#     бит на ней видны ступеньки, у десяти нет;
#   * бегущий белый квадрат — видно, что кадры действительно сменяются и
#     что перемотка попадает куда надо, а не возвращает один и тот же кадр.
$W = 2560
$H = 1440

$filter = "[0:v]drawbox=x='mod(t*260,iw)':y=ih-300:w=70:h=70:color=white:t=fill[a];" +
          "[1:v]scale=${W}:220[g];[a][g]overlay=0:H-220[out]"

function Make($name, $extra) {
    $path = Join-Path $OutDir $name
    if (Test-Path $path) { Write-Host "  have  $name"; return }
    Write-Host "  make  $name"

    $common = @(
        "-f","lavfi","-i","smptehdbars=size=${W}x${H}:rate=60:duration=$Seconds",
        # ${Seconds}, а не $Seconds: двоеточие сразу за именем PowerShell
        # понимает как обращение к области видимости, и "d=$Seconds:r=60"
        # разворачивается в "d==60" — ffmpeg на это отвечает невнятно
        "-f","lavfi","-i","gradients=s=256x220:c0=black:c1=white:x0=0:y0=0:x1=256:y1=0:nb_colors=2:d=${Seconds}:r=60",
        "-filter_complex",$filter,"-map","[out]",
        "-c:v","libsvtav1","-preset","10","-crf","38","-g","120"
    )
    & $FFmpeg -y -hide_banner -loglevel error @common @extra $path
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed on $name" }
}

Write-Host "check set in $OutDir"

# 1. Эталон. Восемь бит — тот путь, что работает с 1.2.5 и проверен живьём.
# По нему сверяются остальные, и он же покажет, не сломали ли его заодно.
Make "01-8bit-709.mp4" @(
    "-pix_fmt","yuv420p",
    "-color_primaries","bt709","-color_trc","bt709","-colorspace","bt709")

# 2. Десять бит в BT.709. Самый частый десятибитный материал и самая простая
# из новых веток: формат ..._10u_as16u_709
Make "02-10bit-709.mp4" @(
    "-pix_fmt","yuv420p10le",
    "-color_primaries","bt709","-color_trc","bt709","-colorspace","bt709")

# --- BT.2020: только Matroska, и это не предпочтение --------------------------
#
# Записанный прямо в MP4, файл теряет первичные цвета и кривую переноса —
# остаётся одна матрица. Проверено здесь же: ffprobe на таком файле показывает
# color_transfer=unknown, то есть файл, сделанный ради проверки PQ, никакого PQ
# не несёт и проверяет совсем другую ветку.
#
# Matroska несёт все три метки. Поэтому собираем в MP4 и перекладываем,
# заново назвав цвет, — тем же приёмом, что и hdr_pq.mkv в make-test-media.ps1.
function MakeTagged($name, $trc, $primaries, $matrix) {
    $path = Join-Path $OutDir $name
    if (Test-Path $path) { Write-Host "  have  $name"; return }

    $temp = Join-Path $OutDir "_temp_tagged.mp4"
    Make "_temp_tagged.mp4" @(
        "-pix_fmt","yuv420p10le",
        "-color_primaries",$primaries,"-color_trc",$trc,"-colorspace",$matrix,
        "-color_range","tv")

    Write-Host "  make  $name"
    & $FFmpeg -y -hide_banner -loglevel error -i $temp -c copy `
        -color_primaries $primaries -color_trc $trc -colorspace $matrix `
        -color_range tv $path
    $code = $LASTEXITCODE
    Remove-Item $temp -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "ffmpeg failed remuxing $name" }
}

# 3. BT.2020 PQ — настоящий HDR10. Формат ..._2020_HDR.
# Это главный файл набора: на нём выигрыш наибольший, и на нём же дороже
# всего ошибка.
MakeTagged "03-10bit-pq-2020.mkv" "smpte2084" "bt2020" "bt2020nc"

# 4. BT.2020 HLG. Отдельная константа у Premiere и отдельная ветка у нас:
# ..._2020_HDR_HLG. Путаница с PQ дала бы вылинявшую картинку.
MakeTagged "04-10bit-hlg-2020.mkv" "arib-std-b67" "bt2020" "bt2020nc"

# 5. BT.2020 БЕЗ HDR-кривой. Раритет, но ветка ..._2020 живая, а проверить
# её больше нечем.
MakeTagged "05-10bit-2020-sdr.mkv" "bt709" "bt2020" "bt2020nc"

# 6. Полный размах. У каждой ветки есть парная константа _FullRange, и путаница
# здесь даёт задранный или съевший контраст — на полосах заметно по чёрному.
Make "06-10bit-709-full.mp4" @(
    "-pix_fmt","yuv420p10le","-color_range","pc",
    "-color_primaries","bt709","-color_trc","bt709","-colorspace","bt709")

# 7. Поворот. Кадр плагин НЕ вертит — объявить поворот хосту в SDK импортёра
# нечем, поля такого нет. Проверяется другое: появилась ли в журнале строка
# о том, что файл просит поворот. Ролик на таймлайне будет боком — так и надо.
$rot = Join-Path $OutDir "07-10bit-rotated-90.mp4"
if (-not (Test-Path $rot)) {
    Write-Host "  make  07-10bit-rotated-90.mp4"
    & $FFmpeg -y -hide_banner -loglevel error -display_rotation 90 `
        -i (Join-Path $OutDir "02-10bit-709.mp4") -c copy -map "0:v:0" $rot
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed on the rotated file" }
} else { Write-Host "  have  07-10bit-rotated-90.mp4" }

# 8. Путь с кириллицей и пробелом. Сам файл тот же; проверяется строка журнала
# imGetInfo8, которая до этой правки обрывалась на первой же русской букве —
# и даже без перевода строки, склеиваясь со следующей.
#
# Имя собрано из кодов, а не набрано: у этого файла нет метки порядка байтов,
# и Windows PowerShell 5.1 читает такой файл в кодировке ANSI системы, то есть
# набранная кириллица превратилась бы в мусор и создала файл не с тем именем.
function Cyr([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }

$cyrDirName  = (Cyr @(0x043F,0x0430,0x043F,0x043A,0x0430)) + " " +
               (Cyr @(0x0441,0x0020,0x043F,0x0440,0x043E,0x0431,0x0435,0x043B,0x043E,0x043C))
$cyrFileName = (Cyr @(0x0432,0x0438,0x0434,0x0435,0x043E)) + " 10 " +
               (Cyr @(0x0431,0x0438,0x0442)) + ".mp4"

$cyrDir = Join-Path $OutDir $cyrDirName
New-Item -ItemType Directory -Force $cyrDir | Out-Null
$cyrFile = Join-Path $cyrDir $cyrFileName
if (-not (Test-Path $cyrFile)) {
    Write-Host "  make  $cyrDirName\$cyrFileName"
    Copy-Item (Join-Path $OutDir "02-10bit-709.mp4") $cyrFile -Force
} else { Write-Host "  have  $cyrDirName\$cyrFileName" }

Write-Host "done"
