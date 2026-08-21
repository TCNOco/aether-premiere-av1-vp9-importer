@echo off
REM Builds the test programs: the decoding core, and loading a finished .prm.
REM ASCII-only on purpose - see the note at the top of build.bat.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

REM decoder_test - the decoding core, works on a file directly
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\decoder_test.cpp ^
   /Fe:build\decoder_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib
if errorlevel 1 exit /b 1

REM host_test - drives the .prm from a fake host that can pretend to be older
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD ^
   /DPRWIN_ENV ^
   /I"sdk\Premiere Pro 26.0 C++ SDK\Examples\Headers" ^
   tools\host_test.cpp ^
   /Fe:build\host_test.exe "/Fo:build\obj\\"
if errorlevel 1 exit /b 1

REM plugin_test - loads the built .prm the same way Premiere will
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD ^
   /DPRWIN_ENV ^
   /I"sdk\Premiere Pro 26.0 C++ SDK\Examples\Headers" ^
   tools\plugin_test.cpp ^
   /Fe:build\plugin_test.exe "/Fo:build\obj\\"
