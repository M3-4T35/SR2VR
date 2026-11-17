@echo off
rem SR2VR - essais sans casque : le jeu est lance (solo) puis ferme par le script, trois fois (5 minutes).
rem Resultat : build\smoke.log, qui finit par SMOKE FINI. Journaux et captures de chaque essai : build\smoke\.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
set "LOG=build\smoke.log"
set "PS=powershell -NoProfile -ExecutionPolicy Bypass -File tools\smoke_test.ps1"
echo === SR2VR : essais sans casque du %DATE% %TIME% === > "%LOG%"
%PS% -Seconds 45 -Name demarrage >> "%LOG%" 2>&1
%PS% -Vr -Seconds 70 -Script tests\scripts\menu.txt -Name menu >> "%LOG%" 2>&1
%PS% -Vr -Seconds 160 -Script tests\scripts\partie.txt -ExpectStereo -Name partie >> "%LOG%" 2>&1
echo SMOKE FINI >> "%LOG%"
