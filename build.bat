@echo off
REM Builds AV1Importer.prm.
REM
REM Comments here are English on purpose. cmd.exe walks a batch file by byte
REM offset and re-seeks after every line; when the file is UTF-8 but the console
REM code page is not, those offsets drift and a later command gets cut in half
REM ("vcxproj" arriving as "xproj"). ASCII-only keeps the file immune to that.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
msbuild src\AV1Importer.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if errorlevel 1 exit /b 1

REM AV1ImporterSettings.exe - the decoder switch window
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD /DUNICODE /D_UNICODE ^
   tools\settings_app.cpp src\AV1Settings.cpp ^
   /Fe:build\Release\AV1ImporterSettings.exe "/Fo:build\obj\\" ^
   /link /SUBSYSTEM:WINDOWS shell32.lib ole32.lib user32.lib gdi32.lib
