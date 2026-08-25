@echo off
REM Builds the test programs.
REM ASCII-only on purpose - see the note at the top of build.bat.
REM
REM decoder_test needs nothing but FFmpeg, so it builds anywhere - including a
REM build server. plugin_test and host_test need the Adobe SDK, which may not be
REM redistributed; without it they are skipped rather than failing the build.
REM Set AETHER_ASAN=1 to instrument the test programs with MSVC AddressSanitizer.

setlocal
cd /d "%~dp0"

set VCVARS=
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
if not defined VCVARS set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo Visual Studio with the C++ workload was not found.
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

REM cl does not create the intermediate directory itself: on a fresh checkout
REM /Fo:build\obj\ fails with C1083 and the build stops. Found by the build
REM server, which is the only machine that ever starts from an empty tree.
if not exist build mkdir build
if not exist build\obj mkdir build\obj

set "EXTRA_CFLAGS="
if /I "%AETHER_ASAN%"=="1" set "EXTRA_CFLAGS=/fsanitize=address /Zi"

REM settings_test - pure INI update logic, no FFmpeg or Adobe SDK needed
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   tools\settings_test.cpp ^
   /Fe:build\settings_test.exe /Fo:build\obj\
if errorlevel 1 exit /b 1

REM importer_math_test - overflow boundaries in Adobe's 32-bit duration field
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   tools\importer_math_test.cpp ^
   /Fe:build\importer_math_test.exe /Fo:build\obj\
if errorlevel 1 exit /b 1

REM decoder_test - the decoding core, works on a file directly
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\decoder_test.cpp ^
   /Fe:build\decoder_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib
if errorlevel 1 exit /b 1

REM cache_probe - how much memory the frame caches take with many clips open.
REM The budget used to be per clip with no overall ceiling: ten 1440p clips
REM reached 4.4 GB. Measured, not assumed - this is the instrument.
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\cache_probe.cpp ^
   /Fe:build\cache_probe.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib psapi.lib
if errorlevel 1 exit /b 1

REM conform_probe - reads one audio track end to end, the way conforming does.
REM Written while chasing "an unspecified error occurred while performing a
REM conform action": the decoder had to be cleared before looking further up.
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\conform_probe.cpp ^
   /Fe:build\conform_probe.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib
if errorlevel 1 exit /b 1

REM fuzz_test - the same core, fed deliberately broken files
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\fuzz_test.cpp ^
   /Fe:build\fuzz_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib
if errorlevel 1 exit /b 1

set SDK=sdk\Premiere Pro 26.0 C++ SDK\Examples\Headers
if not exist "%SDK%" (
    echo.
    echo Adobe SDK not found in sdk\ - plugin_test and host_test skipped.
    echo That is expected on a machine that only builds the core.
    goto after_sdk_tests
)

REM host_test - drives the .prm from a fake host that can pretend to be older
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /DPRWIN_ENV ^
   /I"%SDK%" ^
   tools\host_test.cpp ^
   /Fe:build\host_test.exe "/Fo:build\obj\\"
if errorlevel 1 exit /b 1

REM plugin_test - loads the built .prm the same way Premiere will
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /DPRWIN_ENV ^
   /I"%SDK%" ^
   tools\plugin_test.cpp ^
   /Fe:build\plugin_test.exe "/Fo:build\obj\\"
if errorlevel 1 exit /b 1

:after_sdk_tests
REM Sanitizer runtimes are added to PATH by vcvars64 inside this setlocal.
REM Run checks here when requested; after the batch exits that PATH is gone
REM and an ASan-instrumented executable cannot even start.
if /I "%AETHER_RUN_CORE_CHECKS%"=="1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-core-checks.ps1
    if errorlevel 1 exit /b 1
)
exit /b 0
