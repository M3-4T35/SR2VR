@echo off
rem SR2VR - configure et compile en Release 32 bits (MSVC + vcpkg).
rem Le detail va dans build\build.log, qui se termine par BUILD OK ou BUILD ECHEC.
rem vcpkg : variable SR2VR_VCPKG si elle existe, sinon le dossier ci-dessous.
setlocal
cd /d "%~dp0"

if not defined SR2VR_VCPKG set "SR2VR_VCPKG=C:\Users\mehme\Documents\vcpkg"
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if errorlevel 1 set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"

if not exist build mkdir build
set "LOG=build\build.log"
echo === SR2VR : compilation du %DATE% %TIME% === > "%LOG%"

echo [1/2] Configuration (CMake, Win32, vcpkg x86-windows-static)...
echo === configuration === >> "%LOG%"
"%CMAKE_EXE%" -S . -B build -A Win32 "-DCMAKE_TOOLCHAIN_FILE=%SR2VR_VCPKG%\scripts\buildsystems\vcpkg.cmake" >> "%LOG%" 2>&1
if errorlevel 1 goto fail

echo [2/2] Compilation (Release)...
echo === compilation === >> "%LOG%"
"%CMAKE_EXE%" --build build --config Release >> "%LOG%" 2>&1
if errorlevel 1 goto fail

echo BUILD OK >> "%LOG%"
echo Compilation reussie : build\Release\sr2_vr.dll, sr2_vr_injector.exe, sr2_vr_config.exe
exit /b 0

:fail
echo BUILD ECHEC >> "%LOG%"
echo ECHEC : voir build\build.log
pause
exit /b 1
