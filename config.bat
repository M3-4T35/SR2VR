@echo off
rem SR2VR - ouvre les reglages (sans lancer le jeu). Un jeu en cours les applique en une seconde.
setlocal
cd /d "%~dp0"
if not exist build\Release\sr2_vr_config.exe call build.bat
if not exist build\Release\sr2_vr_config.exe exit /b 1
start "" build\Release\sr2_vr_config.exe
