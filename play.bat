@echo off
rem SR2VR - compile puis lance Saints Row 2 avec le mod (arguments transmis a l'injecteur).
setlocal
cd /d "%~dp0"
call build.bat
if errorlevel 1 exit /b 1
echo Lancement de Saints Row 2 avec SR2VR...
build\Release\sr2_vr_injector.exe %*
if errorlevel 1 (
    echo ECHEC de l'injection : voir build\Release\sr2_vr_injector.log
    pause
)
