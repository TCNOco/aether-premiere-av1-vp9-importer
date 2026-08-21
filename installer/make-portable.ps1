# Портативная сборка: та же папка плагина, но без установщика.
#
#   powershell -File installer\make-portable.ps1
#
# Зачем она есть. Установщик кладёт файлы в общую папку Adobe и требует прав
# администратора, а это ровно тот момент, когда незнакомый человек закрывает
# вкладку: неподписанный .exe, лезущий в Program Files, доверия не вызывает —
# и правильно делает. Здесь та же папка, которую можно посмотреть, проверить
# и скопировать руками.
#
# Версия берётся из самого собранного плагина — оттуда же, откуда её берёт и
# установщик. Раньше она вычитывалась из Aether.iss, но там её больше нет:
# держать номер в трёх местах значит однажды выпустить архив с чужим именем.

param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = "Stop"

$release = Join-Path $Root "build\Release"
$prm     = Join-Path $release "Aether.prm"
if (-not (Test-Path $prm)) { throw "сначала соберите плагин: build.bat" }

$version = (Get-Item $prm).VersionInfo.ProductVersion
if (-not $version) { throw "в $prm нет ресурса версии — пересоберите плагин" }

$staging = Join-Path $Root "build\portable"
$folder  = Join-Path $staging "Aether"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force $folder | Out-Null

# Ровно то же, что кладёт установщик, файл в файл
Copy-Item $prm                                        $folder
Copy-Item (Join-Path $release "AetherSettings.exe")   $folder
Copy-Item (Join-Path $PSScriptRoot "aether.ico")      $folder

foreach ($dll in "avcodec-62.dll", "avformat-62.dll", "avutil-60.dll",
                 "swresample-6.dll", "swscale-9.dll") {
    Copy-Item (Join-Path $Root "ffmpeg\bin\$dll") $folder
}

# Лицензии обязаны лежать рядом с файлами: раздача ffmpeg под LGPL требует
# приложить текст к самим библиотекам, а не только к репозиторию
Copy-Item (Join-Path $Root "LICENSE")                (Join-Path $folder "LICENSE.txt")
Copy-Item (Join-Path $Root "NOTICE")                 (Join-Path $folder "NOTICE.txt")
Copy-Item (Join-Path $Root "THIRD-PARTY-NOTICES.md") $folder
Copy-Item (Join-Path $Root "ffmpeg\LICENSE.txt")     (Join-Path $folder "LICENSE-ffmpeg.txt")

# Указания кладём РЯДОМ с папкой, а не внутрь: внутри они уехали бы в MediaCore
# вместе с плагином и остались бы там навсегда
$instructions = @"
Aether $version - AV1 / VP9 importer for Adobe Premiere Pro, After Effects
and Media Encoder.

https://github.com/neoHaDe/aether-premiere-av1-vp9-importer


HOW TO INSTALL BY HAND

1. Close Premiere Pro, After Effects and Media Encoder.

2. If you have an older version installed, delete its folder first. Two
   importers claiming the same codec will fight over every file:

       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\AV1 Importer
       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Aether

3. Copy the whole "Aether" folder from this archive into:

       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\

   Windows will ask for administrator rights - that folder belongs to Adobe
   and is shared by all of its applications. That is also why one copy is
   enough for Premiere, After Effects and Media Encoder together.

4. Start Premiere and drag an AV1 or VP9 file onto the timeline.

The files must stay together. The plug-in loads the FFmpeg libraries from its
own folder by full path; on their own they are not found.

TO REMOVE IT, delete the folder.

DECODING ON THE CPU OR THE GPU is switched by AetherSettings.exe inside the
folder. The default is the CPU, and that is a measurement rather than a
preference - see the README in the repository.

IF SOMETHING GOES WRONG, the log is here:

       %LOCALAPPDATA%\Aether\log.txt

It records every request Premiere sends, including the ones the plug-in
refuses. Attach it to a bug report.


КАК УСТАНОВИТЬ РУКАМИ

1. Закройте Premiere Pro, After Effects и Media Encoder.

2. Если стояла прежняя версия, сначала удалите её папку. Два импортёра на один
   и тот же кодек будут драться за каждый файл:

       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\AV1 Importer
       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Aether

3. Скопируйте папку "Aether" из этого архива целиком в:

       C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\

   Windows спросит права администратора - эта папка принадлежит Adobe и общая
   для всех её приложений. Поэтому же одной копии хватает сразу на Premiere,
   After Effects и Media Encoder.

4. Запустите Premiere и перетащите файл AV1 или VP9 на таймлайн.

Файлы должны лежать вместе. Плагин загружает библиотеки ffmpeg из своей папки
по полному пути; по отдельности они не находятся.

ЧТОБЫ УДАЛИТЬ, удалите папку.

РАСПАКОВКА ПРОЦЕССОРОМ ИЛИ ВИДЕОКАРТОЙ переключается программой
AetherSettings.exe из этой же папки. По умолчанию процессором, и это по
замеру, а не по вкусу - подробности в README репозитория.

ЕСЛИ ЧТО-ТО НЕ ТАК, журнал лежит здесь:

       %LOCALAPPDATA%\Aether\log.txt

В него пишутся все запросы Premiere, включая те, которые плагин отверг.
Приложите его к сообщению об ошибке.
"@

# UTF-8 с меткой: без неё Блокнот на русской Windows покажет кракозябры
$instructionsPath = Join-Path $staging "INSTALL.txt"
[System.IO.File]::WriteAllText($instructionsPath, $instructions,
                               (New-Object System.Text.UTF8Encoding $true))

# Время правки INSTALL.txt приравниваем к времени самого плагина.
#
# Zip хранит время изменения каждого файла, а INSTALL.txt создаётся заново на
# каждый прогон — из-за одного этого две сборки одного и того же кода давали
# РАЗНЫЕ контрольные суммы. Само по себе не беда, но именно так и получается
# «в описании релиза одна сумма, в приложенном файле другая»: пересобрал,
# забыл пересчитать. Теперь одинаковый вход даёт одинаковый архив.
$stamp = (Get-Item $prm).LastWriteTimeUtc
foreach ($file in Get-ChildItem $staging -Recurse -File) {
    $file.LastWriteTimeUtc = $stamp
}

$dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "Aether-$version-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }

Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip -CompressionLevel Optimal

$size = (Get-Item $zip).Length
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()

Write-Host ""
Write-Host "portable: $zip"
Write-Host ("  {0:N0} bytes" -f $size)
Write-Host "  SHA256  $hash"
