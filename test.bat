@echo off
rem SR2VR - lance les tests (ctest) apres une compilation ; resultat dans build\test.log
setlocal
cd /d "%~dp0"
set "CTEST_EXE=ctest"
where ctest >nul 2>&1
if errorlevel 1 set "CTEST_EXE=C:\Program Files\CMake\bin\ctest.exe"
echo === SR2VR : tests du %DATE% %TIME% === > build\test.log
"%CTEST_EXE%" --test-dir build -C Release --output-on-failure >> build\test.log 2>&1
if errorlevel 1 (echo TESTS ECHEC >> build\test.log) else (echo TESTS OK >> build\test.log)
exit /b 0
