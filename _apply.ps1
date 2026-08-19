# Разовый скрипт: убрать Influx и поставить наш плагин.
# Запускается с правами администратора, пишет отчёт в _apply.log.

$ErrorActionPreference = "Stop"
$log = "F:\premiere-av1-importer\_apply.log"
"" | Set-Content $log -Encoding utf8

function Say($text) { $text | Add-Content $log -Encoding utf8 }

try {
    $mediaCore = "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore"
    $influx    = Join-Path $mediaCore "Autokroma Influx"
    $removed   = "F:\premiere-av1-importer\_removed"

    # Premiere держит загруженные плагины открытыми — заменить их на ходу нельзя
    $running = Get-Process -Name "Adobe Premiere Pro", "Adobe Media Encoder" -ErrorAction SilentlyContinue
    if ($running) { throw "Запущены: $($running.ProcessName -join ', ')" }

    # Influx переносим, а не удаляем: штатного деинсталлятора у него нет,
    # и вернуть папку на место — единственный способ откатить этот шаг
    if (Test-Path $influx) {
        New-Item -ItemType Directory -Force $removed | Out-Null
        Move-Item $influx (Join-Path $removed "Autokroma Influx") -Force
        Say "Influx перенесён в: $removed\Autokroma Influx"
    } else {
        Say "Influx не найден — пропускаю"
    }

    $target = Join-Path $mediaCore "AV1 Importer"
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item "F:\premiere-av1-importer\build\plugin\*" $target -Force
    Say "Плагин установлен в: $target"
    Get-ChildItem $target | ForEach-Object {
        Say ("  {0,-20} {1,6:N1} МБ" -f $_.Name, ($_.Length / 1MB))
    }
    Say "OK"
}
catch {
    Say "ОШИБКА: $_"
}
