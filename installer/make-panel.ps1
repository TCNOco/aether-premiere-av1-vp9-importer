# Сборка и подпись панели для Premiere.
#
#   powershell -File installer\make-panel.ps1
#
# Зачем подпись. Premiere не показывает неподписанные расширения — молча,
# без единого сообщения. Обойти это можно только режимом разработчика
# (PlayerDebugMode), которого у обычного человека нет и быть не должно.
#
# ⚠ На машине разработчика этот режим обычно включён, и там панель появится
# ДАЖЕ неподписанной. То есть проверить подпись у себя нельзя: своя машина
# врёт в утешительную сторону. Ровно поэтому здесь стоит -verify — единственная
# проверка, которая не зависит от настроек этой машины.
#
# Что на выходе. Подписанный .zxp — это обычный zip, внутри которого к файлам
# добавлена папка META-INF с подписью. Мы его тут же распаковываем обратно:
# нашему установщику остаётся положить готовую папку в расширения Adobe,
# и человеку не нужен ни менеджер расширений, ни что-либо ещё.

param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = "Stop"

$src     = Join-Path $Root "cep"
$tool    = Join-Path $Root "tools\signing\ZXPSignCmd.exe"
$p12     = Join-Path $PSScriptRoot "signing\aether.p12"
$pwFile  = Join-Path $PSScriptRoot "signing\password.txt"
$out     = Join-Path $Root "build\panel"
$zxp     = Join-Path $out "Aether.zxp"
$folder  = Join-Path $out "com.nehade.aether"

foreach ($needed in @($tool, $p12, $pwFile)) {
    if (-not (Test-Path $needed)) {
        throw "не найден $needed — запустите installer\make-cert.ps1"
    }
}

$pw = (Get-Content $pwFile -Raw).Trim()

New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item $zxp -Force -ErrorAction SilentlyContinue
if (Test-Path $folder) { Remove-Item $folder -Recurse -Force }

# ----------------------------------------------------- номер версии в панель
#
# В самом манифесте стоит 0.0.0: номер один на весь проект и живёт в
# собранном плагине. Подставляем его в КОПИЮ, а не в исходник — иначе после
# каждой сборки в репозитории оказывалась бы правка, которую никто не делал.
$prm = Join-Path $Root "build\Release\Aether.prm"
if (-not (Test-Path $prm)) { throw "сначала соберите плагин: build.bat" }
$version = (Get-Item $prm).VersionInfo.ProductVersion
if (-not $version) { throw "в $prm нет ресурса версии" }

$staged = Join-Path $out "src"
if (Test-Path $staged) { Remove-Item $staged -Recurse -Force }
Copy-Item $src $staged -Recurse

$manifest = Join-Path $staged "CSXS\manifest.xml"
(Get-Content $manifest -Raw -Encoding UTF8).Replace('"0.0.0"', '"' + $version + '"') |
    Set-Content $manifest -Encoding UTF8 -NoNewline

Write-Host "версия панели: $version"
$src = $staged

# ------------------------------------------------------------------ подпись
#
# Метка времени обязательна. Без неё подпись умирает вместе с сертификатом,
# и в один день у ВСЕХ пользователей панель молча перестанет появляться —
# без обновления, без причины, без единого сообщения. С меткой подпись
# переживает истечение сертификата.
Write-Host "подписываю панель..."
& $tool -sign $src $zxp $p12 $pw -tsa "http://timestamp.digicert.com"
if ($LASTEXITCODE -ne 0) { throw "подпись не удалась" }

# ------------------------------------------------------------------ проверка
Write-Host ""
Write-Host "проверяю подпись:"
& $tool -verify $zxp -certInfo -skipOnlineRevocationChecks
if ($LASTEXITCODE -ne 0) { throw "подпись не прошла проверку" }

# ------------------------------------------------------- готовая папка
# .zxp — обычный zip. Распаковываем, чтобы установщик клал папку как есть.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($zxp, $folder)

Write-Host ""
Write-Host "готово:"
Write-Host "  $zxp"
Write-Host "  $folder"
$signed = Join-Path $folder "META-INF"
if (-not (Test-Path $signed)) { throw "в распакованной панели нет META-INF — подпись не легла" }
Write-Host "  подпись на месте: META-INF"
