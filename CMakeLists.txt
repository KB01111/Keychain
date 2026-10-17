cmake_minimum_required(VERSION 3.24)
project(Keychain VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KEYCHAIN_BUILD_TESTS "Build Keychain native test suite" ON)

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    add_compile_options(/W4 /MP /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif()

# Discover dxc.exe for HLSL Shader Model 6.0 compilation
find_program(DXC_EXECUTABLE
    NAMES dxc.exe
    PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.28000.0/x64"
    DOC "DirectX Shader Compiler (DXC)"
)

if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc.exe (DirectX Shader Compiler) was not found in Windows SDK.")
endif()

set(SHADER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/assets/shaders")
set(SHADER_HEADER_DIR "${CMAKE_SOURCE_DIR}/src/native/graphics")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${SHADER_HEADER_DIR}")

add_custom_command(
    OUTPUT
        "${SHADER_OUTPUT_DIR}/ring_sdf_vs.cso"
        "${SHADER_OUTPUT_DIR}/ring_sdf_ps.cso"
        "${SHADER_OUTPUT_DIR}/atlas_quad_vs.cso"
        "${SHADER_OUTPUT_DIR}/atlas_quad_ps.cso"
        "${SHADER_HEADER_DIR}/ring_sdf_vs_bytecode.h"
        "${SHADER_HEADER_DIR}/ring_sdf_ps_bytecode.h"
        "${SHADER_HEADER_DIR}/atlas_quad_vs_bytecode.h"
        "${SHADER_HEADER_DIR}/atlas_quad_ps_bytecode.h"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -WX -O3
        -Fo "${SHADER_OUTPUT_DIR}/ring_sdf_vs.cso"
        -Fh "${SHADER_HEADER_DIR}/ring_sdf_vs_bytecode.h"
        -Vn g_RingSdfVsBytecode
        "${CMAKE_SOURCE_DIR}/src/shaders/ring_sdf.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -WX -O3
        -Fo "${SHADER_OUTPUT_DIR}/ring_sdf_ps.cso"
        -Fh "${SHADER_HEADER_DIR}/ring_sdf_ps_bytecode.h"
        -Vn g_RingSdfPsBytecode
        "${CMAKE_SOURCE_DIR}/src/shaders/ring_sdf.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -WX -O3
        -Fo "${SHADER_OUTPUT_DIR}/atlas_quad_vs.cso"
        -Fh "${SHADER_HEADER_DIR}/atlas_quad_vs_bytecode.h"
        -Vn g_AtlasQuadVsBytecode
        "${CMAKE_SOURCE_DIR}/src/shaders/atlas_quad.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -WX -O3
        -Fo "${SHADER_OUTPUT_DIR}/atlas_quad_ps.cso"
        -Fh "${SHADER_HEADER_DIR}/atlas_quad_ps_bytecode.h"
        -Vn g_AtlasQuadPsBytecode
        "${CMAKE_SOURCE_DIR}/src/shaders/atlas_quad.hlsl"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/src/shaders/ring_sdf.hlsl"
        "${CMAKE_SOURCE_DIR}/src/shaders/atlas_quad.hlsl"
    COMMENT "Compiling HLSL Shader Model 6.0 shaders with DXC"
    VERBATIM
)

add_custom_target(KeychainShaders DEPENDS
    "${SHADER_OUTPUT_DIR}/ring_sdf_vs.cso"
    "${SHADER_OUTPUT_DIR}/ring_sdf_ps.cso"
    "${SHADER_OUTPUT_DIR}/atlas_quad_vs.cso"
    "${SHADER_OUTPUT_DIR}/atlas_quad_ps.cso"
)

set(KEYCHAIN_CORE_SOURCES
    src/native/common/types.h
    src/native/config/config_schema.h
    src/native/config/config_store.cpp
    src/native/gesture/gesture_state_machine.h
    src/native/gesture/gesture_state_machine.cpp
    src/native/input/input_manager.h
    src/native/input/input_manager.cpp
    src/native/layout/radial_layout.h
    src/native/layout/radial_layout.cpp
    src/native/animation/spring_animator.h
    src/native/animation/spring_animator.cpp
    src/native/actions/action_dispatcher.h
    src/native/actions/action_dispatcher.cpp
    src/native/diagnostics/diagnostics_manager.h
    src/native/diagnostics/diagnostics_manager.cpp
    src/native/ipc/ipc_server.h
    src/native/ipc/ipc_server.cpp
    src/native/graphics/text_icon_atlas.h
    src/native/graphics/text_icon_atlas.cpp
    src/native/graphics/d3d12_overlay_renderer.h
    src/native/graphics/d3d12_overlay_renderer.cpp
    src/native/app/accessibility_provider.h
    src/native/app/accessibility_provider.cpp
    src/native/app/launcher_app.h
    src/native/app/launcher_app.cpp
)

add_library(KeychainCore STATIC ${KEYCHAIN_CORE_SOURCES})
add_dependencies(KeychainCore KeychainShaders)

target_include_directories(KeychainCore PUBLIC
    "${CMAKE_SOURCE_DIR}/src/native"
    "${CMAKE_SOURCE_DIR}/third_party"
)

target_link_libraries(KeychainCore PUBLIC
    d3d12.lib
    dxgi.lib
    dxguid.lib
    dcomp.lib
    dwrite.lib
    windowscodecs.lib
    dwmapi.lib
    uxtheme.lib
    shlwapi.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    oleacc.lib
    uiautomationcore.lib
    uuid.lib
    advapi32.lib
    user32.lib
    gdi32.lib
    psapi.lib
    wtsapi32.lib
    comctl32.lib
    shcore.lib
)

add_executable(Keychain WIN32
    src/native/main.cpp
)

target_link_libraries(Keychain PRIVATE KeychainCore)

# Copy precompiled .cso shaders next to Keychain.exe on build
add_custom_command(TARGET Keychain POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Keychain>/assets/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SHADER_OUTPUT_DIR}/ring_sdf_vs.cso"
        "${SHADER_OUTPUT_DIR}/ring_sdf_ps.cso"
        "${SHADER_OUTPUT_DIR}/atlas_quad_vs.cso"
        "${SHADER_OUTPUT_DIR}/atlas_quad_ps.cso"
        "$<TARGET_FILE_DIR:Keychain>/assets/shaders/"
    COMMENT "Copying precompiled .cso shader assets to output directory"
)

if(KEYCHAIN_BUILD_TESTS)
    enable_testing()
    add_executable(Keychain.Tests
        tests/native/test_main.cpp
    )
    target_link_libraries(Keychain.Tests PRIVATE KeychainCore psapi.lib)
    add_test(NAME KeychainNativeTests COMMAND Keychain.Tests)
endif()
