cmake_minimum_required(VERSION 3.20)
project(NukeTilemapEditor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# The vcpkg MSBuild integration picks debug vs release vcpkg libs from <UseDebugLibraries>;
# CMake omits it by default and the integration then links RELEASE boost into a Debug DLL
# (mixed CRTs — bfs::path dtor crash). Standalone builds need it here; the superbuild sets
# it globally too.
set(CMAKE_VS_USE_DEBUG_LIBRARIES "$<CONFIG:Debug>")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")   # VCPKG_ROOT env var, not a hardcoded path

add_library(NukeTilemapEditor SHARED
    src/NukeTilemapEditor.cpp
)

# EDITOR TOOLING module: imports NukeImGui (the editor's shared ImGui context) — packaging
# auto-detects that import and never ships this DLL with a game. Links NukeTilemap for the
# InvalidateTileSet hot-reload seam.
target_compile_definitions(NukeTilemapEditor PRIVATE
    WIN32 _WINDOWS NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    GLM_ENABLE_EXPERIMENTAL
    BOOST_ALL_DYN_LINK
    IMGUI_API=__declspec\(dllimport\)   # ImGui comes from the shared NukeImGui.dll
)

target_include_directories(NukeTilemapEditor PRIVATE
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/include
    ${NUKE_ROOT}/NukeEngine/deps
    ${NUKE_ROOT}/NukeEngine/deps/glm
    ${NUKE_ROOT}/NukeTilemap/include
    ${NUKE_ROOT}/NukeImGui
    ${NUKE_ROOT}/NukeImGui/src                       # nukeui.h (DocWindow: detachable doc windows)
    ${NUKE_ROOT}/NukeImGui/imgui
    ${VCPKG_INSTALLED}/include
)

target_link_directories(NukeTilemapEditor PRIVATE
    ${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>            # NukeEngine.lib (import lib)
    ${NUKE_ROOT}/NukeImGui/build/$<CONFIG>           # NukeImGui.lib
    ${NUKE_ROOT}/build/NukeTilemap/$<CONFIG>         # NukeTilemap.lib
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib  # boost (auto-linked)
)

target_link_libraries(NukeTilemapEditor PRIVATE
    NukeEngine
    NukeImGui
    NukeTilemap
)

# Deploy the plugin DLL into the host's modules/ folder.
set(MODULES_DIR "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/modules")
add_custom_command(TARGET NukeTilemapEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukeTilemapEditor>" "${MODULES_DIR}/"
    COMMENT "Deploying NukeTilemapEditor.dll to modules/"
)
