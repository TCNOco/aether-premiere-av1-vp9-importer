@echo off
REM Сборка установщика. Требует Inno Setup 6 (winget install JRSoftware.InnoSetup).
REM Перед запуском собери сам плагин: build.bat

setlocal
set ISCC=

if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"      set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe"           set ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe

if "%ISCC%"=="" (
    echo Inno Setup 6 не найден. Установи: winget install JRSoftware.InnoSetup
    exit /b 1
)

if not exist "%~dp0..\build\Release\AV1Importer.prm" (
    echo Сначала собери плагин: build.bat
    exit /b 1
)

"%ISCC%" "%~dp0AV1Importer.iss"
