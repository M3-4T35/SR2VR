@echo off
rem SR2VR - compile, puis un essai sans casque dont les arguments sont dans build\essai_args.txt (ceux de
rem tools\smoke_test.ps1, sur une ligne). Resultat : build\essai.log, qui finit par ESSAI FINI.
setlocal
cd /d "%~dp0"
if not exist build\essai_args.txt (
    echo Ecrire les arguments de tools\smoke_test.ps1 dans build\essai_args.txt
    pause
    exit /b 1
)
set /p ARGS=<build\essai_args.txt
call build.bat >nul
if errorlevel 1 (
    echo COMPILATION EN ECHEC : voir build\build.log > build\essai.log
    echo ESSAI FINI >> build\essai.log
    exit /b 1
)
echo === SR2VR : essai du %DATE% %TIME% : %ARGS% === > build\essai.log
powershell -NoProfile -ExecutionPolicy Bypass -File tools\smoke_test.ps1 %ARGS% >> build\essai.log 2>&1
echo ESSAI FINI >> build\essai.log
