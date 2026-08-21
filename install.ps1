# Установка плагина в Premiere Pro.
#
# Плагин кладётся отдельной папкой внутри MediaCore — так же, как это делают
# остальные сторонние плагины. Отдельная папка здесь не для порядка, а по делу:
# рядом с .prm лежат библиотеки ffmpeg, и плагин ищет их именно в своей папке.
#
# Запускать от имени администратора, Premiere при этом должен быть закрыт.

param(
    [string]$Source = (Join-Path $PSScriptRoot "build\plugin"),
    [string]$MediaCore = "",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

# Адрес общей папки плагинов Adobe записывает в реестр сама, и он один и тот же
# для всех приложений — потому плагин и подхватывается всеми сразу. Спросить
# надёжнее, чем предполагать: Adobe может стоять и не на системном диске.
function Find-MediaCore {
    $v = (Get-ItemProperty 'HKLM:\SOFTWARE\Adobe\Premiere Pro\CurrentVersion' `
          -Name 'Plug-InsDir' -ErrorAction SilentlyContinue).'Plug-InsDir'
    if ($v -and (Test-Path $v)) { return $v.TrimEnd('\') }

    foreach ($app in 'Premiere Pro', 'After Effects') {
        $key = "HKLM:\SOFTWARE\Adobe\$app"
        if (-not (Test-Path $key)) { continue }
        foreach ($ver in Get-ChildItem $key -ErrorAction SilentlyContinue) {
            $v = (Get-ItemProperty $ver.PSPath -Name 'CommonPluginInstallPath' `
                  -ErrorAction SilentlyContinue).CommonPluginInstallPath
            if ($v -and (Test-Path $v)) { return $v.TrimEnd('\') }
        }
    }
    return "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore"
}

if (-not $MediaCore) { $MediaCore = Find-MediaCore }
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
