@echo off
rem SR2VR - mesures sans casque : la meme scene en partie avec plusieurs reglages (12 minutes).
rem Resultat : build\mesures.log, qui finit par MESURES FINIES. Journaux et captures de chaque essai : build\smoke\.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
set "LOG=build\mesures.log"
set "PS=powershell -NoProfile -ExecutionPolicy Bypass -File tools\smoke_test.ps1"
echo === SR2VR : mesures sans casque du %DATE% %TIME% === > "%LOG%"
%PS% -Vr -Seconds 160 -Script tests\scripts\partie.txt -ExpectStereo -Set "Display.frame_cap=0" -Name mesure_frein_du_jeu >> "%LOG%" 2>&1
%PS% -Vr -Seconds 160 -Script tests\scripts\partie.txt -ExpectStereo -Set "Display.frame_cap=100" -Name mesure_defaut >> "%LOG%" 2>&1
%PS% -Vr -Seconds 160 -Script tests\scripts\partie.txt -ExpectStereo -Set "Display.async_capture=2" -Name mesure_lecture_autre_thread >> "%LOG%" 2>&1
%PS% -Vr -Seconds 160 -Script tests\scripts\partie.txt -ExpectStereo -Set "Display.async_capture=2,Display.frame_cap=240" -Name mesure_autre_thread_plafond_240 >> "%LOG%" 2>&1
echo MESURES FINIES >> "%LOG%"
