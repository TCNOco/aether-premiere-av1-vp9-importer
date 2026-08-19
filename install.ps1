# Установка плагина в Premiere Pro.
#
# Плагин кладётся отдельной папкой внутри MediaCore — так же, как это делают
# остальные сторонние плагины. Отдельная папка здесь не для порядка, а по делу:
# рядом с .prm лежат библиотеки ffmpeg, и плагин ищет их именно в своей папке.
#
# Запускать от имени администратора, Premiere при этом должен быть закрыт.

param(
    [string]$Source = (Join-Path $PSScriptRoot "build\plugin"),
    [string]$MediaCore = "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$target = Join-Path $MediaCore "AV1 Importer"

$admin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Error "Нужны права администратора: папка Program Files защищена от записи."
}

# Premiere держит загруженные плагины открытыми — файл заменить не выйдет
$running = Get-Process -Name "Adobe Premiere Pro", "Adobe Media Encoder" -ErrorAction SilentlyContinue
if ($running) {
    Write-Error "Сначала закройте: $($running.ProcessName -join ', ')"
}

if ($Uninstall) {
    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
        Write-Host "Удалено: $target"
    } else {
        Write-Host "Плагин не установлен."
    }
    return
}

if (-not (Test-Path (Join-Path $Source "AV1Importer.prm"))) {
    Write-Error "Не найден AV1Importer.prm в $Source — сначала соберите проект (build.bat)."
}

New-Item -ItemType Directory -Force $target | Out-Null
Copy-Item (Join-Path $Source "*") $target -Force

Write-Host "Установлено в: $target"
Get-ChildItem $target | ForEach-Object {
    Write-Host ("  {0,-20} {1,6:N1} МБ" -f $_.Name, ($_.Length / 1MB))
}
Write-Host ""
Write-Host "Запустите Premiere и перетащите файл AV1 на таймлайн."
