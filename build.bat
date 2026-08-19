@echo off
REM Сборка плагина AV1Importer.prm
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
msbuild src\AV1Importer.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
