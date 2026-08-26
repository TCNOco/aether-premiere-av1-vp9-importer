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

REM AETHER_ASAN: set at all means on, unless it plainly says otherwise.
REM
REM Not "==1", and that is the whole point. Exact comparison against "1" loses
REM to a trailing space - "set AETHER_ASAN=1 & call build-test.bat" stores "1 " -
REM and then BOTH the flags below and the confirmation further down are skipped
REM together. Asking for the sanitizer and quietly not getting it is the one
REM outcome worth engineering against; erring the other way only costs a slower
REM build. Caught for real while chasing a crash: the run meant to test the core
REM under ASan produced an uninstrumented binary, and its clean result was very
REM nearly taken as evidence.
set "EXTRA_CFLAGS="
set "ASAN_WANTED="
if defined AETHER_ASAN set "ASAN_WANTED=1"
if /I "%AETHER_ASAN%"=="0"     set "ASAN_WANTED="
if /I "%AETHER_ASAN%"=="off"   set "ASAN_WANTED="
if /I "%AETHER_ASAN%"=="no"    set "ASAN_WANTED="
if /I "%AETHER_ASAN%"=="false" set "ASAN_WANTED="
if defined ASAN_WANTED set "EXTRA_CFLAGS=/fsanitize=address /Zi"

REM settings_test - INI preservation plus the shared settings model
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   tools\settings_test.cpp src\AV1Settings.cpp ^
   /Fe:build\settings_test.exe /Fo:build\obj\ ^
   /link shell32.lib ole32.lib
if errorlevel 1 exit /b 1

REM preview_cache_test - strict AEPV records, keys, corruption and strides
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   tools\preview_cache_test.cpp src\AV1Settings.cpp src\PreviewCache.cpp src\AV1Log.cpp ^
   /Fe:build\preview_cache_test.exe /Fo:build\obj\ ^
   /link shell32.lib ole32.lib bcrypt.lib
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
   src\AV1Settings.cpp src\PreviewCache.cpp src\AV1Log.cpp src\AV1Decoder.cpp tools\decoder_test.cpp ^
   /Fe:build\decoder_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib bcrypt.lib shell32.lib ole32.lib
if errorlevel 1 exit /b 1

REM Second half of the same guard: the flags were passed, now prove they landed.
REM
REM The reading of the variable above can no longer swallow the request, but a
REM compiler that ignored the flag, a stale object file, or a link that dropped
REM the runtime would all still produce a binary with no sanitizer in it and a
REM build log full of success. A green run that proves less than it appears to
REM is exactly what this project does not keep.
REM
REM decoder_test stands in for the rest: they all share EXTRA_CFLAGS, so either
REM every one of them is instrumented or none is.
if defined ASAN_WANTED (
    dumpbin /nologo /dependents build\decoder_test.exe | findstr /i "clang_rt.asan" >nul
    if errorlevel 1 (
        echo.
        echo FAIL: AETHER_ASAN was set, but decoder_test.exe carries no sanitizer runtime.
        echo       Check how the variable was set - a trailing space is enough to lose it.
        exit /b 1
    )
    echo AddressSanitizer: instrumentation confirmed
)

REM cache_probe - how much memory the frame caches take with many clips open.
REM The budget used to be per clip with no overall ceiling: ten 1440p clips
REM reached 4.4 GB. Measured, not assumed - this is the instrument.
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Settings.cpp src\PreviewCache.cpp src\AV1Log.cpp src\AV1Decoder.cpp tools\cache_probe.cpp ^
   /Fe:build\cache_probe.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib psapi.lib bcrypt.lib shell32.lib ole32.lib
if errorlevel 1 exit /b 1

REM conform_probe - reads one audio track end to end, the way conforming does.
REM Written while chasing "an unspecified error occurred while performing a
REM conform action": the decoder had to be cleared before looking further up.
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Settings.cpp src\PreviewCache.cpp src\AV1Log.cpp src\AV1Decoder.cpp tools\conform_probe.cpp ^
   /Fe:build\conform_probe.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib bcrypt.lib shell32.lib ole32.lib
if errorlevel 1 exit /b 1

REM fuzz_test - the same core, fed deliberately broken files
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD %EXTRA_CFLAGS% ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Settings.cpp src\PreviewCache.cpp src\AV1Log.cpp src\AV1Decoder.cpp tools\fuzz_test.cpp ^
   /Fe:build\fuzz_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib bcrypt.lib shell32.lib ole32.lib
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
REM
REM Read the same forgiving way as AETHER_ASAN, and for the same reason: an
REM exact "==1" loses to a trailing space, and losing it here means the ASan
REM step in CI quietly becomes a rebuild that runs nothing at all - while
REM reporting success.
set "RUN_CHECKS="
if defined AETHER_RUN_CORE_CHECKS set "RUN_CHECKS=1"
if /I "%AETHER_RUN_CORE_CHECKS%"=="0"     set "RUN_CHECKS="
if /I "%AETHER_RUN_CORE_CHECKS%"=="off"   set "RUN_CHECKS="
if /I "%AETHER_RUN_CORE_CHECKS%"=="no"    set "RUN_CHECKS="
if /I "%AETHER_RUN_CORE_CHECKS%"=="false" set "RUN_CHECKS="
if defined RUN_CHECKS (
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-core-checks.ps1
    if errorlevel 1 exit /b 1
)
exit /b 0
