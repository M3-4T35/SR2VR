@echo off
rem SR2VR - compile puis lance le banc d'essai OpenXR 32 bits hors du jeu (casque connecte).
rem Resultats : build\Release\xr_probe.log, xr_probe_defaut.log, xr_probe_steamvr.log
setlocal
cd /d "%~dp0"
call build.bat
if errorlevel 1 exit /b 1
build\Release\xr_probe.exe %*
