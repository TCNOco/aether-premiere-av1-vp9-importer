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

if not exist "%~dp0..\build\Release\Aether.prm" (
    echo Build the plug-in first: build.bat
    exit /b 1
)

"%ISCC%" "%~dp0Aether.iss"
if errorlevel 1 exit /b 1

REM The portable archive, for people who would rather copy a folder than run
REM an unsigned installer - a reasonable position when the file comes from a
REM stranger on the internet
powershell -ExecutionPolicy Bypass -File "%~dp0make-portable.ps1"
