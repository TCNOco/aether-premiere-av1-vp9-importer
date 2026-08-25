# Ключ для подписи панели Premiere.
#
#   powershell -File installer\make-cert.ps1
#
# Панель CEP без подписи Premiere не показывает — молча, без единого
# сообщения. Обойти это можно только режимом разработчика, которого у
# обычного человека нет и быть не должно. Значит подпись обязательна.
#
# Сертификат самоподписанный, и это законный путь: Adobe его принимает,
# в отличие от Windows SmartScreen, которому самоподпись не годится.
# Стоит он ноль.
#
# Ни сертификат, ни пароль в репозиторий не попадают (см. .gitignore).
# Потеря не смертельна: пересоздать можно этим же скриптом, для CEP смена
# ключа ничем не грозит — Premiere проверяет целость подписи, а не то,
# кем именно она поставлена.

param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [int]$ValidityDays = 3650
)

$ErrorActionPreference = "Stop"

$signDir = Join-Path $PSScriptRoot "signing"
$toolDir = Join-Path $Root "tools\signing"
$tool    = Join-Path $toolDir "ZXPSignCmd.exe"
$p12     = Join-Path $signDir "aether.p12"
$pwFile  = Join-Path $signDir "password.txt"

New-Item -ItemType Directory -Force $signDir | Out-Null
New-Item -ItemType Directory -Force $toolDir | Out-Null

# ---------------------------------------------------------------- подписывалка
$toolSha256 = "ffc2223167225ce61d024eb463fc5ad1a1be16133f99ef334a646f7311916c98"
if (-not (Test-Path $tool)) {
    # Официальный репозиторий Adobe. Закреплены и версия, и commit: ветка
    # master может измениться без нашего коммита, а подписывающий EXE не должен.
    $commit = "8897cfaf9801b0ad5321951199dd831a82964583"
    $url = "https://raw.githubusercontent.com/Adobe-CEP/CEP-Resources/$commit/ZXPSignCMD/4.1.3/x64/ZXPSignCmd.exe"
    $download = $tool + ".download"
    Write-Host "качаю ZXPSignCmd 4.1.3 с github.com/Adobe-CEP"
    Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
    $downloadHash = (Get-FileHash $download -Algorithm SHA256).Hash
    if ($downloadHash -ne $toolSha256) {
        Remove-Item $download -Force -ErrorAction SilentlyContinue
        throw "SHA256 ZXPSignCmd не совпал: ожидался $toolSha256, получен $downloadHash"
    }
    Move-Item $download $tool -Force
}

# Проверяем и уже лежащий файл: одна успешная загрузка не защищает от случайной
# замены или повреждения инструмента на машине сборки.
$actualToolHash = (Get-FileHash $tool -Algorithm SHA256).Hash
if ($actualToolHash -ne $toolSha256) {
    throw "неверный SHA256 у ${tool}: ожидался $toolSha256, получен $actualToolHash"
}
Write-Host "подписывалка: $tool"

# ------------------------------------------------------------------ сертификат
if (Test-Path $p12) {
    Write-Host "сертификат уже есть: $p12"
    Write-Host "чтобы сделать новый — удалите его и запустите снова"
    exit 0
}

# Пароль случайный. Придумывать его за человека не стоит, а спрашивать —
# значит оставить в истории команд; случайный и записанный рядом честнее.
$bytes = New-Object byte[] 24
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$pw = [Convert]::ToBase64String($bytes) -replace '[+/=]', ''
Set-Content -Path $pwFile -Value $pw -Encoding ascii -NoNewline

# Пустые поля ZXPSignCmd не принимает — молча показывает справку и выходит.
& $tool -selfSignedCert RU Unspecified neoHaDe "Aether AV1 VP9 Importer" `
        $pw $p12 -validityDays $ValidityDays
if ($LASTEXITCODE -ne 0) { throw "не удалось создать сертификат" }

Write-Host ""
Write-Host "готово:"
Write-Host "  $p12"
Write-Host "  $pwFile"
Write-Host ""
Write-Host "Оба файла — вне репозитория. Сделайте копию, если не хотите"
Write-Host "пересоздавать их при переезде на другую машину."
