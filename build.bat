@echo off
REM Сборка плагина AV1Importer.prm
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
msbuild src\AV1Importer.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo

REM AV1ImporterSettings.exe — окно переключения декодера
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MD /DUNICODE /D_UNICODE ^
   tools\settings_app.cpp src\AV1Settings.cpp ^
   /Fe:build\Release\AV1ImporterSettings.exe "/Fo:build\obj\\" ^
   /link /SUBSYSTEM:WINDOWS shell32.lib ole32.lib user32.lib gdi32.lib
