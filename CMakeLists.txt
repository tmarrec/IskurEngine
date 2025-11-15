# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

cmake_minimum_required(VERSION 4.2)
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
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# DXC (DirectX Shader Compiler)
set(DXC_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc_2025_07_14")
set(DXC_BIN_DIR "${DXC_SDK_ROOT}/bin/x64")

add_library(dxcompiler STATIC IMPORTED)
set_target_properties(dxcompiler PROPERTIES
  IMPORTED_LOCATION "${DXC_SDK_ROOT}/lib/x64/dxcompiler.lib"
  INTERFACE_INCLUDE_DIRECTORIES     "${DXC_SDK_ROOT}/inc"
)
set(DIRECTX_DXC_PATH "${DXC_BIN_DIR}" CACHE PATH "Directory containing dxc.exe")

# Third-Party Dependencies
add_subdirectory(third_party/DirectX-Headers-1.618.2 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/meshoptimizer-0.25 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/D3D12MemoryAllocator-3.0.1 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/tinygltf-2.9.7 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/DirectXTex-oct2025 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/DirectXMesh-oct2025 EXCLUDE_FROM_ALL)
add_subdirectory(third_party/DirectXTK12-oct2025 EXCLUDE_FROM_ALL)

# ImGui
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-1.92.4")
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")

# MikkTSpace
add_library(mikktspace STATIC third_party/MikkTSpace/mikktspace.c)
target_include_directories(mikktspace PUBLIC third_party/MikkTSpace)

# FidelityFX SDK v2.0.0 — headers + prebuilt DX12 DLLs
set(FFX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FidelityFX-SDK-v2.0.0" CACHE PATH "FidelityFX SDK root")

# Headers
find_path(FFX_INCLUDE_DIR     NAMES ffx_api.hpp   PATHS "${FFX_SDK_DIR}" PATH_SUFFIXES "Kits/FidelityFX/api/include" REQUIRED)
find_path(FFX_UPSCALE_INC_DIR NAMES ffx_upscale.hpp PATHS "${FFX_SDK_DIR}" PATH_SUFFIXES "Kits/FidelityFX/upscalers/include" REQUIRED)

# Import lib for the DX12 loader (link-time)
# NOTE: libs are in Kits/FidelityFX/bin or Kits/FidelityFX/signedbin in your tree.
find_library(FFX_LOADER_LIB
  NAMES amd_fidelityfx_loader_dx12
  PATHS "${FFX_SDK_DIR}"
  PATH_SUFFIXES
    "Kits/FidelityFX/signedbin"
    "Kits/FidelityFX/bin"
    # keep these as fallback in case layout changes
    "Kits/FidelityFX/api/signedbin"
    "Kits/FidelityFX/api/bin"
    "api/signedbin"
    "api/bin"
  REQUIRED)

# Runtime DLLs (copy beside .exe)
find_file(FFX_DLL_LOADER     NAMES amd_fidelityfx_loader_dx12.dll
  PATHS "${FFX_SDK_DIR}"
  PATH_SUFFIXES
    "Kits/FidelityFX/signedbin"
    "Kits/FidelityFX/bin"
    "Kits/FidelityFX/api/signedbin"
    "Kits/FidelityFX/api/bin"
    "api/signedbin"
    "api/bin")

find_file(FFX_DLL_UPSCALER   NAMES amd_fidelityfx_upscaler_dx12.dll
  PATHS "${FFX_SDK_DIR}"
  PATH_SUFFIXES
    "Kits/FidelityFX/signedbin"
    "Kits/FidelityFX/bin"
    "Kits/FidelityFX/api/signedbin"
    "Kits/FidelityFX/api/bin"
    "api/signedbin"
    "api/bin")

find_file(FFX_DLL_FRAMEGEN   NAMES amd_fidelityfx_framegeneration_dx12.dll
  PATHS "${FFX_SDK_DIR}"
  PATH_SUFFIXES
    "Kits/FidelityFX/signedbin"
    "Kits/FidelityFX/bin"
    "Kits/FidelityFX/api/signedbin"
    "Kits/FidelityFX/api/bin"
    "api/signedbin"
    "api/bin")

# Interface target you can link against
add_library(AMD::FidelityFX INTERFACE IMPORTED)
set_target_properties(AMD::FidelityFX PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${FFX_INCLUDE_DIR};${FFX_UPSCALE_INC_DIR}"
  INTERFACE_LINK_LIBRARIES "${FFX_LOADER_LIB}"
)

# (optional) help during configuration
message(STATUS "FFX include dir : ${FFX_INCLUDE_DIR}")
message(STATUS "FFX loader .lib : ${FFX_LOADER_LIB}")
message(STATUS "FFX loader .dll : ${FFX_DLL_LOADER}")

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
  AMD::FidelityFX
  mikktspace
)
target_include_directories(core_settings INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/data"
  "${CMAKE_CURRENT_SOURCE_DIR}/code"
)

# Engine Core (Object Library)
set(PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/code/pch.h")
file(GLOB_RECURSE ENGINE_SOURCES
  code/*.cpp
  code/*.h
  code/*.natvis
  data/*CPUGPU.h
)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/code/tools/.*")

add_library(engine_core OBJECT ${ENGINE_SOURCES})
target_precompile_headers(engine_core PRIVATE ${PCH_HEADER})
target_link_libraries(engine_core PUBLIC core_settings)

# ---- Engine Executable -----------------------------------------------------------------------
add_executable(${PROJECT_NAME} $<TARGET_OBJECTS:engine_core>)
target_link_libraries(${PROJECT_NAME} PRIVATE core_settings)
target_link_options(${PROJECT_NAME} PRIVATE "/SUBSYSTEM:WINDOWS")

# Copy DXC runtime next to the exe
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMENT "Copying DXC runtime"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${DXC_BIN_DIR}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
)

# Copy FidelityFX DLLs next to the exe (loader is required; upscaler/framegen depend on what you use)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMENT "Copying FidelityFX DLLs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<IF:$<BOOL:${FFX_DLL_LOADER}>,${FFX_DLL_LOADER},$<1:>>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<IF:$<BOOL:${FFX_DLL_UPSCALER}>,${FFX_DLL_UPSCALER},$<1:>>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<IF:$<BOOL:${FFX_DLL_FRAMEGEN}>,${FFX_DLL_FRAMEGEN},$<1:>>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
)

# Iskur scene packer
file(GLOB ISKUR_SCENE_GEN_SOURCES
  code/tools/IskurScenePacker/*.cpp
  code/tools/IskurScenePacker/*.h
  code/common/*.cpp
  code/common/*.h
  data/*CPUGPU.h
)
add_executable(IskurScenePacker ${ISKUR_SCENE_GEN_SOURCES})
target_link_libraries(IskurScenePacker PRIVATE
  core_settings
)
target_include_directories(IskurScenePacker PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/data"
  "${CMAKE_CURRENT_SOURCE_DIR}/code"
)
target_link_options(IskurScenePacker PRIVATE "/SUBSYSTEM:CONSOLE")

# IDE Settings
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME}
)
