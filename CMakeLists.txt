# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

cmake_minimum_required(VERSION 4.1)
project(IskurEngine LANGUAGES CXX)

# C++ Settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directory for binaries and libraries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
foreach(OUTPUTCONFIG Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin/${OUTPUTCONFIG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin/${OUTPUTCONFIG})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin/${OUTPUTCONFIG})
endforeach()

# DXC (DirectX Shader Compiler)
set(DXC_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc_2025_07_14")
set(DXC_BIN_DIR "${DXC_SDK_ROOT}/bin/x64")

add_library(dxcompiler STATIC IMPORTED)
set_target_properties(dxcompiler PROPERTIES
  IMPORTED_LOCATION "${DXC_SDK_ROOT}/lib/x64/dxcompiler.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${DXC_SDK_ROOT}/inc"
)
set(DIRECTX_DXC_PATH "${DXC_BIN_DIR}" CACHE PATH "Directory containing dxc.exe")

# Third-Party Dependencies
add_subdirectory(third_party/DirectX-Headers-1.616.0 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/meshoptimizer-0.24 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/D3D12MemoryAllocator-3.0.1 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/tinygltf-2.9.6 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/DirectXTex-jul2025 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/DirectXMesh-jul2025 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/Catch2-3.9.1 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/DirectXTK12-jul2025 EXCLUDE_FROM_ALL)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-1.92.1")
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
)
add_library(mikktspace STATIC third_party/MikkTSpace/mikktspace.c)
target_include_directories(mikktspace PUBLIC third_party/MikkTSpace)

# FidelityFX API (FSR2/3 + Frame-Generation)
set(FFX_API_BACKEND "DX12_X64" CACHE STRING "" FORCE)
add_subdirectory(third_party/FidelityFX-SDK-v1.1.4/ffx-api EXCLUDE_FROM_ALL)

# Core Settings Target
add_library(core_settings INTERFACE)
target_compile_definitions(core_settings INTERFACE
  _HAS_EXCEPTIONS=0
  UNICODE
  _UNICODE
)
target_link_libraries(core_settings INTERFACE
  dxguid
  d3d12
  dxgi
  meshoptimizer
  D3D12MemoryAllocator
  imgui
  tinygltf
  DirectXTex
  DirectXMesh
  DirectXTK12
  dxcompiler
  DirectX-Headers
  amd_fidelityfx_dx12
  mikktspace
)
target_include_directories(core_settings INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FidelityFX-SDK-v1.1.4/sdk/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# Engine Core (Object Library)
set(PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/code/pch.h")
file(GLOB_RECURSE ENGINE_SOURCES
  code/*.cpp
  code/*.h
  code/*.natvis
  data/*CPUGPU.h
)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/code/unittests/.*")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/code/tools/.*")
add_library(engine_core OBJECT ${ENGINE_SOURCES})
target_precompile_headers(engine_core PRIVATE ${PCH_HEADER})
target_link_libraries(engine_core PUBLIC core_settings)

# Engine Executable
add_executable(${PROJECT_NAME} $<TARGET_OBJECTS:engine_core>)
target_link_libraries(${PROJECT_NAME} PRIVATE core_settings)
target_link_options(${PROJECT_NAME} PRIVATE "/SUBSYSTEM:WINDOWS")

# Copy DXC runtime next to the exe
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMENT "Copying DXC runtime"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${DXC_BIN_DIR}"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
)

# Copy FidelityFX host DLL
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMENT "Copying FidelityFX host DLL"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:amd_fidelityfx_dx12>
    $<TARGET_FILE_DIR:${PROJECT_NAME}>
)

# Unit Tests
file(GLOB_RECURSE UNITTEST_SOURCES
  code/unittests/*.cpp
  code/unittests/*.h
)
add_executable(${PROJECT_NAME}_tests
  $<TARGET_OBJECTS:engine_core>
  ${UNITTEST_SOURCES}
)
target_precompile_headers(${PROJECT_NAME}_tests PRIVATE ${PCH_HEADER})
target_link_libraries(${PROJECT_NAME}_tests PRIVATE
  core_settings
  Catch2WithMain
)

# Iskur scene packer
file(GLOB ISKUR_SCENE_GEN_SOURCES
  code/tools/IskurScenePacker/*.cpp
  code/tools/IskurScenePacker/*.h
)

add_executable(IskurScenePacker ${ISKUR_SCENE_GEN_SOURCES})
target_link_libraries(IskurScenePacker PRIVATE
  core_settings
)
target_link_options(IskurScenePacker PRIVATE "/SUBSYSTEM:CONSOLE")

# IDE Settings
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME}
)