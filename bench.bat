@echo off
rem SR2VR - mesure les methodes de capture d image sur la carte graphique ; resultat dans build\bench.log
rem (quelques secondes, carte graphique peu sollicitee)
setlocal
cd /d "%~dp0"
echo === SR2VR : mesures du %DATE% %TIME% === > build\bench.log
build\Release\capture_bench.exe 1920 1080 12 >> build\bench.log 2>&1
echo BENCH FINI >> build\bench.log
exit /b 0
