cmake_minimum_required(VERSION 3.20)

# Dépendances vcpkg liées statiquement : la DLL est chargée par LoadLibrary depuis
# build/, Windows ne chercherait pas ses DLL dépendantes à côté d'elle.
set(VCPKG_TARGET_TRIPLET x86-windows-static CACHE STRING "")
project(SR2VR VERSION 0.19.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SR2_pc.exe est en 32 bits : configure avec  cmake -S . -B build -A Win32")
endif()

# Numéro de version visible dans le log, l'injecteur et l'outil de configuration
add_compile_definitions(SR2VR_VERSION="${PROJECT_VERSION}")

find_package(minhook CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

add_library(sr2_vr SHARED
    src/dllmain.cpp
    src/util/log.cpp
    src/util/hook.cpp
    src/util/config.cpp
    src/util/pattern.cpp
    src/engine/aim.cpp
    src/engine/camera_hook.cpp
    src/engine/controls.cpp
    src/engine/hands.cpp
    src/engine/hud.cpp
    src/engine/melee.cpp
    src/engine/player.cpp
    src/render/d3d9_hook.cpp
    src/render/frame_capture.cpp
    src/render/sight_overlay.cpp
    src/input/actions.cpp
    src/input/keys.cpp
    src/input/vpad.cpp
    src/xr/xr_input.cpp
    src/xr/xr_laser.cpp
    src/xr/xr_session.cpp
)
target_include_directories(sr2_vr PRIVATE src)
target_compile_options(sr2_vr PRIVATE /utf-8 /W4)
target_link_libraries(sr2_vr PRIVATE minhook::minhook OpenXR::openxr_loader version d3d11 dxgi dxguid)
target_compile_definitions(sr2_vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Runtime statique : pas besoin d'installer les redistribuables VC++ à côté du jeu
set_property(TARGET sr2_vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Injecteur CLI (32 bits comme le jeu) : lance SR2_pc.exe et y injecte sr2_vr.dll
add_executable(sr2_vr_injector injector/main.cpp)
target_compile_definitions(sr2_vr_injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(sr2_vr_injector PRIVATE /utf-8 /W4)
target_link_libraries(sr2_vr_injector PRIVATE psapi advapi32)
set_property(TARGET sr2_vr_injector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# L'injecteur cherche la DLL à côté de lui
add_dependencies(sr2_vr_injector sr2_vr)

# Outil de configuration et lanceur : édite sr2_vr.ini (fenêtre Win32 dessinée en GDI)
add_executable(sr2_vr_config WIN32 config_tool/main.cpp config_tool/settings.cpp)
target_compile_definitions(sr2_vr_config PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(sr2_vr_config PRIVATE /utf-8 /W4)
target_link_libraries(sr2_vr_config PRIVATE comctl32 gdi32 user32)
set_property(TARGET sr2_vr_config PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# L'injecteur l'ouvre avant le jeu
add_dependencies(sr2_vr_injector sr2_vr_config)

# Banc d'essai : faux jeu D3D9 32 bits qui fait tourner la capture et la session OpenXR de
# la DLL hors de Saints Row 2 (runtime 32 bits fautif, ou problème propre au jeu ?)
add_executable(xr_probe tools/xr_probe/main.cpp src/util/log.cpp src/util/config.cpp src/render/frame_capture.cpp
               src/render/sight_overlay.cpp src/input/actions.cpp src/input/keys.cpp src/input/vpad.cpp
               src/xr/xr_input.cpp src/xr/xr_laser.cpp src/xr/xr_session.cpp)
target_include_directories(xr_probe PRIVATE src)
target_compile_definitions(xr_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(xr_probe PRIVATE /utf-8 /W4)
target_link_libraries(xr_probe PRIVATE OpenXR::openxr_loader d3d9 d3d11 dxgi dxguid advapi32 user32)
set_property(TARGET xr_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
