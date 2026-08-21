@echo off
REM Builds the test programs.
REM ASCII-only on purpose - see the note at the top of build.bat.
REM
REM decoder_test needs nothing but FFmpeg, so it builds anywhere - including a
REM build server. plugin_test and host_test need the Adobe SDK, which may not be
REM redistributed; without it they are skipped rather than failing the build.

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

if not exist build mkdir build

REM decoder_test - the decoding core, works on a file directly
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\decoder_test.cpp ^
   /Fe:build\decoder_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib
if errorlevel 1 exit /b 1

set SDK=sdk\Premiere Pro 26.0 C++ SDK\Examples\Headers
if not exist "%SDK%" (
    echo.
    echo Adobe SDK not found in sdk\ - plugin_test and host_test skipped.
    echo That is expected on a machine that only builds the core.
    exit /b 0
)

REM host_test - drives the .prm from a fake host that can pretend to be older
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD ^
   /DPRWIN_ENV ^
   /I"%SDK%" ^
   tools\host_test.cpp ^
   /Fe:build\host_test.exe "/Fo:build\obj\\"
if errorlevel 1 exit /b 1

REM plugin_test - loads the built .prm the same way Premiere will
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD ^
   /DPRWIN_ENV ^
   /I"%SDK%" ^
   tools\plugin_test.cpp ^
   /Fe:build\plugin_test.exe "/Fo:build\obj\\"
