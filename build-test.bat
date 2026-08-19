@echo off
REM Сборка проверочных программ: слой декодирования и загрузка готового .prm.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

REM decoder_test — ядро декодирования, работает с файлом напрямую
cl /nologo /std:c++17 /EHsc /O2 /MD ^
   /D__STDC_CONSTANT_MACROS /D__STDC_LIMIT_MACROS ^
   /I"ffmpeg\include" ^
   src\AV1Decoder.cpp tools\decoder_test.cpp ^
   /Fe:build\decoder_test.exe /Fo:build\obj\ ^
   /link /LIBPATH:"ffmpeg\lib" avcodec.lib avformat.lib avutil.lib swscale.lib
if errorlevel 1 exit /b 1

REM plugin_test — загружает собранный .prm так же, как это сделает Premiere
cl /nologo /std:c++17 /EHsc /O2 /MD ^
   /DPRWIN_ENV ^
   /I"sdk\Premiere Pro 26.0 C++ SDK\Examples\Headers" ^
   tools\plugin_test.cpp ^
   /Fe:build\plugin_test.exe "/Fo:build\obj\\"
