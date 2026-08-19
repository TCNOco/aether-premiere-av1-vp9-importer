@echo off
REM Zapusk ustanovki plagina AV1 s pravami administratora.
REM Zapuskat' iz provodnika: dvoynoy klik ili pravaya knopka -> Zapusk ot imeni administratora.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','F:\premiere-av1-importer\_apply.ps1'"