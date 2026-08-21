# Материал для живой проверки HDR в Premiere.
#
# Отличается от make-test-media.ps1 замыслом. Там файлы для автоматических
# проверок — маленькие и уродливые, лишь бы декодировались. Здесь наоборот:
# на них надо СМОТРЕТЬ, и вывод делается глазами.
#
#   powershell -File tools\make-hdr-check.ps1
#
# Устройство набора. Картинка во всех файлах одна и та же, меняется только
# то, как она записана. Поэтому сравнивать можно прямо на таймлайне, положив
# клипы друг за другом.
#
#   01-sdr-bt709.mkv     обычный SDR — это эталон, «как должно выглядеть»
#   02-pq-100nit.mkv     та же картинка, честно переведённая в BT.2020 PQ
#   03-pq-1000nit.mkv    то же, но яркостью до 1000 нит — тут нужен тон-маппинг
#   04-hlg.mkv           то же в BT.2020 HLG — вторая кривая HDR
#
# Главный тут второй файл. Перевод в PQ сделан так, что белый SDR оказался
# ровно на 100 нитах, то есть картинка по смыслу не изменилась вовсе. Если
# хост понимает, что ему отдали, файл 02 выглядит НЕОТЛИЧИМО от файла 01.
# Если не понимает — выйдет молочно-серое и плоское, мимо не пройдёшь.
#
# HDR-монитор для этого не нужен: на секвенции Rec.709 хост сводит HDR к SDR
# сам, и смотреть надо именно на результат этого сведения.

param(
    [string]$OutDir = "build\media\hdr-check",
    [string]$FFmpeg = "ffmpeg\bin\ffmpeg.exe",
    [string]$Font   = "C:/Windows/Fonts/arialbd.ttf"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $FFmpeg)) {
    $fallback = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
    if (-not $fallback) { throw "ffmpeg not found: neither $FFmpeg nor one on PATH" }
    $FFmpeg = $fallback
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

$Dur = 6
$Size = "1280x720"

# Двоеточие в пути шрифта приходится прятать: в графе фильтров оно разделяет
# параметры. И только прямые слэши — обратный там значит совсем другое.
# Кавычки вокруг пути тоже обязательны: без них экранирование не срабатывает и
# ffmpeg спотыкается о букву диска. По имени шрифт не запросить — fontconfig
# в этой сборке без файла настроек.
$FontEsc = $Font -replace ':', '\:'

# Надпись держим наверху, поверх цветных полос. Внизу кадра лежит поле чёрного
# и лестница серого — то самое, по чему и судят о верности цвета; закрывать их
# подписью значит выкинуть половину смысла.
function Label($text) {
    "drawtext=fontfile='$FontEsc'" +
    ":text='$text':fontcolor=white:fontsize=34:borderw=3:bordercolor=black" +
    ":x=(w-text_w)/2:y=28"
}

# Одна и та же исходная картинка для всех четырёх файлов: цветные полосы SMPTE.
# В них сразу видно и насыщенность, и серую лестницу, и чёрное поле снизу —
# то есть ровно то, что первым портится при неверно понятом цвете.
function Source($label, $chain) {
    # Ярлык и перевод в HDR идут ОДНОЙ цепочкой. Двумя ключами -vf нельзя:
    # ffmpeg молча оставит последний, и надпись пропадёт.
    $vf = Label $label
    if ($chain) { $vf = "$chain,$vf" }
    @("-f","lavfi","-i","smptehdbars=size=${Size}:rate=30:duration=$Dur",
      "-f","lavfi","-i","sine=frequency=440:duration=$Dur",
      "-map","0:v","-map","1:a","-vf",$vf)
}

function Run($name, $ffargs) {
    $path = Join-Path $OutDir $name
    Write-Host "  make  $name"
    & $FFmpeg -y -hide_banner -loglevel error @ffargs $path
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed on $name" }
}

# Перевод SDR в HDR по-честному: разворачиваем гамму в линейный свет, объявляем,
# на сколько нит приходится белый, и оттуда уже кодируем нужной кривой с
# первичными цветами BT.2020. Просто переставить теги было бы подлогом — файл
# заявлял бы одно, а внутри лежало бы другое, и проверять стало бы нечего.
# Число для графа фильтров: точка, а не запятая.
#
# Правило скучное, а ошибка от него дорогая. В русской локали 3.32 печатается
# как «3,32», запятая в графе фильтров разделяет фильтры, и ffmpeg спотыкается
# на ровном месте. Хуже другое: «1,02» он читает как «1» и молча продолжает —
# получается файл, который выглядит убедительно и лжёт.
function Num($v) { ([double]$v).ToString("0.######", [cultureinfo]::InvariantCulture) }

# Перевод SDR в PQ с ЗАДАННОЙ яркостью белого.
#
# Ключ npl у zscale для этого не годится, хотя название обещает ровно это:
# zimg применяет его только к HLG, а для PQ пропускает. Мимо этого легко
# пройти — файлы собираются, теги проставляются, и только измерение кода
# показывает, что все они вышли одинаковыми. Так и случилось: «1000 нит»
# ничем не отличался от «100 нит», пока не сверили с формулой SMPTE 2084.
#
# Работает вместо него простое усиление в линейном свете. Проверено по той же
# формуле: белый на 100 / 203 / 400 нит даёт код 509 / 573 / 639, и фильтр
# выдаёт ровно их.
function ToPQ($nits) {
    # Фильтр берёт не больше трёх ступеней за раз, а до тысячи нит их 3.32,
    # поэтому усиление разбивается на несколько подряд. Свет линейный, так что
    # два умножения подряд — это то же самое умножение.
    $left  = [math]::Log($nits / 100.0, 2)
    $gain  = @()
    while ([math]::Abs($left) -gt 3.0) {
        $step  = 3.0 * [math]::Sign($left)
        $gain += "exposure=exposure=" + (Num $step)
        $left -= $step
    }
    if ([math]::Abs($left) -gt 0.0001) { $gain += "exposure=exposure=" + (Num $left) }
    $boost = if ($gain) { ($gain -join ",") + "," } else { "" }

    "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv," +
    "zscale=t=linear,format=gbrpf32le,$boost" +
    "zscale=p=bt2020:t=smpte2084:m=bt2020nc:r=tv,format=yuv420p10le"
}

# HLG считает яркость не в нитах, а долей от возможностей экрана, поэтому
# задавать ей нит нечего: белый и так встаёт на своё штатное место.
function ToHLG() {
    "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv," +
    "zscale=t=linear,format=gbrpf32le," +
    "zscale=p=bt2020:t=arib-std-b67:m=bt2020nc:r=tv,format=yuv420p10le"
}

$AV1_8  = @("-pix_fmt","yuv420p","-c:v","libsvtav1","-preset","6","-crf","28","-g","30","-c:a","aac")
$AV1_10 = @("-c:v","libsvtav1","-preset","6","-crf","24","-g","30","-c:a","aac")

Write-Host "HDR check material in $OutDir"

# 01 — эталон. Ничего особенного, обычный BT.709.
# В Matroska, как и остальные три: этот ffmpeg при записи в MP4 сохраняет
# матрицу, но теряет первичные цвета и кривую. Эталон с неполным описанием
# сравнивать не с чем.
# Описание цвета вешаем фильтром, а не одними ключами: ключи -color_* правят
# заголовок контейнера, но кадры уходят в кодировщик неописанными, и часть
# полей теряется по дороге. Проверено — без setparams выходит файл с матрицей,
# но без первичных цветов и кривой.
Run "01-sdr-bt709.mkv" ((Source "01  SDR  BT.709  -  reference" `
    "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv") + $AV1_8 + @(
    "-color_primaries","bt709","-color_trc","bt709","-colorspace","bt709","-color_range","tv"))

# 02 — та же картинка в PQ, белый на 100 нитах. Должна совпасть с эталоном.
Run "02-pq-100nit.mkv" ((Source "02  HDR  BT.2020 PQ  100 nit  -  must match 01" (ToPQ 100)) + $AV1_10 + @(
    "-color_primaries","bt2020","-color_trc","smpte2084","-colorspace","bt2020nc","-color_range","tv"))

# 03 — настоящая яркость. Здесь без тон-маппинга не обойтись.
Run "03-pq-1000nit.mkv" ((Source "03  HDR  BT.2020 PQ  1000 nit  -  needs tone mapping" (ToPQ 1000)) + $AV1_10 + @(
    "-color_primaries","bt2020","-color_trc","smpte2084","-colorspace","bt2020nc","-color_range","tv"))

# 04 — вторая кривая HDR, вещательная.
Run "04-hlg.mkv" ((Source "04  HDR  BT.2020 HLG  -  must match 01" (ToHLG)) + $AV1_10 + @(
    "-color_primaries","bt2020","-color_trc","arib-std-b67","-colorspace","bt2020nc","-color_range","tv"))

# 05 - лесенка яркости, и она отвечает на вопрос, на который остальные файлы
# только намекают.
#
# PQ задаёт яркость В АБСОЛЮТНЫХ НИТАХ, и потому «перевести SDR в PQ» - это не
# одно преобразование, а целое семейство: надо решить, скольким нитам равен
# белый. Файл 02 сделан по самому простому допущению - сто нит, как у
# студийного монитора SDR. Но отраслевой стандарт (ITU BT.2408) кладёт белый
# лист бумаги в HDR на 203 нита, и тон-маппер, считающий так, наши сто нит
# честно нарисует вдвое тусклее.
#
# Гадать, какое допущение у хоста, незачем. В одном файле лежат четыре отрезка
# с одной и той же картинкой, переведённой по-разному. Какой отрезок совпадёт
# с эталоном - то допущение хост и держит.
$Ladder = @(100, 203, 400, 1000)
$parts  = @()
$inputs = @()
$i = 0
foreach ($nits in $Ladder) {
    $inputs += @("-f","lavfi","-i","smptehdbars=size=${Size}:rate=30:duration=3")
    $parts  += "[${i}:v]" + (Label "05  PQ  $nits nit  -  which one matches 01") +
               "," + (ToPQ $nits) + "[v$i]"
    $i++
}
$tags  = (0..($Ladder.Count-1) | ForEach-Object { "[v$_]" }) -join ""
$graph = ($parts -join ";") + ";" + $tags + "concat=n=$($Ladder.Count):v=1[out]"

Run "05-pq-ladder.mkv" ($inputs + @(
    "-filter_complex",$graph,"-map","[out]") + $AV1_10 + @(
    "-color_primaries","bt2020","-color_trc","smpte2084","-colorspace","bt2020nc","-color_range","tv"))

# Две картинки на память: как должно быть и как не должно. Сверяться с ними
# удобнее, чем вспоминать, какими полосы были минуту назад.
Run "expected.png" @("-i",(Join-Path $OutDir "01-sdr-bt709.mkv"),
                     "-vf","select=eq(n\,60)","-frames:v","1")

# А это тот же кадр PQ, вытащенный БЕЗ пересчёта кривой — ровно то, что видно,
# когда объявление цветового пространства пропустили мимо ушей.
Run "if-ignored.png" @("-i",(Join-Path $OutDir "02-pq-100nit.mkv"),
                       "-vf","select=eq(n\,60),format=yuv420p","-frames:v","1")

# Замер первый: та ли яркость записалась, которую просили.
#
# Проверка появилась не от любви к порядку. Первая сборка набора пользовалась
# ключом npl, тот для PQ не работает, и все файлы вышли одинаковыми — при
# верных тегах, правдоподобном виде и молчаливом согласии всех остальных
# проверок. «1000 нит» отличался от «100 нит» ничем. Показать это могло только
# измерение самого кода.
#
# Ждём мы не «примерно похоже», а конкретные числа из формулы SMPTE 2084:
# белому на 100 / 203 / 400 / 1000 нит отвечают коды 509 / 573 / 639 / 720.
# Надпись из кадра вырезаем — она рисуется после перевода и всегда белая,
# так что затмила бы собой то, что меряем.
function Ymax($file, $extra) {
    $vf = "crop=1280:620:0:100,signalstats,metadata=print:key=lavfi.signalstats.YMAX"
    if ($extra) { $vf = "$extra,$vf" }
    $keep = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $FFmpeg -hide_banner -loglevel info -i $file -vf $vf -frames:v 1 -f null - 2>&1
    } finally { $ErrorActionPreference = $keep }
    $line = $out | Select-String "YMAX="
    if ("$line" -match 'YMAX=([0-9.]+)') { return [int][double]$matches[1] }
    return -1
}

Write-Host ""
Write-Host "did the asked-for brightness actually land (PQ code of white):"
$expect = @{ 100 = 509; 203 = 573; 400 = 639; 1000 = 720 }
$frame  = 45
foreach ($nits in $Ladder) {
    $got = Ymax (Join-Path $OutDir "05-pq-ladder.mkv") "select=eq(n\,$frame)"
    $ok  = if ([math]::Abs($got - $expect[$nits]) -le 30) { "ok" } else { "WRONG" }
    Write-Host ("  {0,5} nit   want ~{1,4}   got {2,4}   {3}" -f $nits, $expect[$nits], $got, $ok)
    $frame += 90
}
Write-Host "  a constant offset of about +20 is the label border and encoder overshoot"

# Замер второй: правда ли, что файл 02 обязан совпасть с эталоном.
#
# Весь набор держится на этом обещании, и проверять его надо счётом, а не
# верой. Гоняем обратное преобразование — из PQ и HLG назад в BT.709 — и
# сравниваем с эталоном. Рядом меряем тот же файл БЕЗ преобразования: так
# он выглядит, если объявление проигнорировали.
#
# Надпись при этом отрезаем: она у файлов разная, и без обрезки она одна
# роняет цифру на пятнадцать децибел, хотя к цвету отношения не имеет.
function Psnr($file, $chain) {
    $crop = "crop=1280:620:0:100"
    $ref  = Join-Path $OutDir "01-sdr-bt709.mkv"
    # PSNR ffmpeg печатает в поток ошибок, и его приходится ловить. А ловля
    # через 2>&1 в PowerShell 5.1 сама по себе считается сбоем: каждая строка
    # приезжает завёрнутой в ошибку, и при ErrorActionPreference=Stop скрипт
    # падает на ровном месте. Поэтому на время замера отпускаем.
    $keep = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $FFmpeg -hide_banner -loglevel info -i $file -i $ref `
               -lavfi "[0:v]$chain,$crop[x];[1:v]$crop[y];[x][y]psnr" -f null - 2>&1
    } finally { $ErrorActionPreference = $keep }
    $line = $out | Select-String "PSNR y:"
    if (-not $line) { return "??" }
    if ("$line" -match 'average:([0-9.]+)') { return [math]::Round([double]$matches[1], 1) }
    return "??"
}

$Back = "zscale=t=linear:npl=100,format=gbrpf32le," +
        "zscale=p=bt709:t=bt709:m=bt709:r=tv,format=yuv420p"

Write-Host ""
Write-Host "does the promise hold (PSNR against 01, dB):"
foreach ($n in @("02-pq-100nit.mkv","04-hlg.mkv")) {
    $f  = Join-Path $OutDir $n
    $ok = Psnr $f $Back
    $no = Psnr $f "format=yuv420p"
    Write-Host ("  {0,-18} understood {1,5}   ignored {2,5}" -f $n, $ok, $no)
}
Write-Host "  around 38 is 'the eye sees no difference', around 16 is 'washed out'"

# Что записалось на самом деле — иначе весь набор держится на честном слове.
Write-Host ""
Write-Host "what actually ended up in the files:"
foreach ($f in Get-ChildItem $OutDir -Include *.mp4,*.mkv -Recurse | Sort-Object Name) {
    $line = & (Join-Path (Split-Path $FFmpeg) "ffprobe.exe") -v error `
        -select_streams v:0 -show_entries stream=color_primaries,color_transfer,color_space,pix_fmt `
        -of default=noprint_wrappers=1:nokey=0 $f.FullName
    Write-Host ("  " + $f.Name)
    $line | ForEach-Object { Write-Host ("      " + $_) }
}
