@echo off
REM Strict pre-release gate. Unlike build-test.bat, this script never skips
REM checks that need the Adobe SDK: a release without the SDK layer tested is
REM a failed release check.
REM
REM ASCII-only for the same cmd.exe byte-offset reason documented in build.bat.

setlocal
cd /d "%~dp0"

set "SDK=sdk\Premiere Pro 26.0 C++ SDK\Examples\Headers"
set "PLUGIN=build\Release\Aether.prm"
set "MEDIA=build\media"
set "STAGE=build\release-gate\Aether"

if not exist "%SDK%\PrSDKImport.h" (
    echo FAIL: Adobe Premiere Pro 26.0 C++ SDK is required in sdk\.
    exit /b 1
)
if not exist "ffmpeg\bin\ffmpeg.exe" (
    echo FAIL: FFmpeg shared build is required in ffmpeg\.
    exit /b 1
)

echo [1/6] Building the Release plug-in and diagnostics
call build.bat
if errorlevel 1 exit /b 1

echo [2/6] Building all test programs, including SDK tests
call build-test.bat
if errorlevel 1 exit /b 1
if not exist "build\plugin_test.exe" (
    echo FAIL: plugin_test.exe was not built.
    exit /b 1
)
if not exist "build\host_test.exe" (
    echo FAIL: host_test.exe was not built.
    exit /b 1
)

echo [3/6] Generating deterministic test media
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-test-media.ps1 -OutDir "%MEDIA%"
if errorlevel 1 exit /b 1

echo [4/6] Running core correctness and damaged-file checks
set "PATH=%CD%\ffmpeg\bin;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-core-checks.ps1 -MediaDir "%MEDIA%"
if errorlevel 1 exit /b 1

echo [5/6] Staging the installed plug-in layout
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
if errorlevel 1 exit /b 1

copy /y "%PLUGIN%" "%STAGE%\" >nul
if errorlevel 1 exit /b 1
for %%D in (avutil-60.dll swresample-6.dll swscale-9.dll avcodec-62.dll avformat-62.dll) do (
    copy /y "ffmpeg\bin\%%D" "%STAGE%\" >nul
    if errorlevel 1 exit /b 1
)

build\plugin_test.exe "%STAGE%\Aether.prm"
if errorlevel 1 exit /b 1

echo [6/6] Driving the plug-in through representative host scenarios
for %%F in (
    av1_8bit.mp4
    av1_10bit.mp4
    vp9_alpha.webm
    vfr.mkv
    hdr_pq.mkv
    audio_tracks_differ.mp4
    audio_only.mka
) do (
    echo.
    echo host_test: %%F
    build\host_test.exe "%STAGE%\Aether.prm" "%MEDIA%\%%F"
    if errorlevel 1 exit /b 1
)

echo.
echo RELEASE GATE PASSED
exit /b 0
