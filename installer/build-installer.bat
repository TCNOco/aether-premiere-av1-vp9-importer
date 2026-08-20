@echo off
REM Builds the installer. Needs Inno Setup 6 (winget install JRSoftware.InnoSetup).
REM Build the plug-in first: build.bat
REM ASCII-only on purpose - see the note at the top of build.bat.

setlocal
set ISCC=

if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"      set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe"           set ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe

if "%ISCC%"=="" (
    echo Inno Setup 6 not found. Install it: winget install JRSoftware.InnoSetup
    exit /b 1
)

if not exist "%~dp0..\build\Release\AV1Importer.prm" (
    echo Build the plug-in first: build.bat
    exit /b 1
)

"%ISCC%" "%~dp0AV1Importer.iss"
