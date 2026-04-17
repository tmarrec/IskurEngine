# Iskur Scene Packer (standalone solution)
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

cmake_minimum_required(VERSION 4.2)
project(IskurScenePacker LANGUAGES CXX)

# C++ Settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (C:/IskurEngine)
get_filename_component(ISKUR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(THIRD_PARTY_OS_ROOT "${ISKUR_ROOT}/third_party/open_source")
set(AGILITY_INCLUDE_DIR "${ISKUR_ROOT}/third_party/proprietary/Agility/build/native/include")

# Output directory for binaries and libraries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ISKUR_ROOT}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ISKUR_ROOT}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${ISKUR_ROOT}/bin)
foreach(OUTPUTCONFIG Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${ISKUR_ROOT}/bin/${OUTPUTCONFIG})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${ISKUR_ROOT}/bin/${OUTPUTCONFIG})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ${ISKUR_ROOT}/bin/${OUTPUTCONFIG})
endforeach()
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Third-Party Dependencies (subset)
add_subdirectory("${THIRD_PARTY_OS_ROOT}/meshoptimizer-1.1" "${CMAKE_BINARY_DIR}/_deps/meshoptimizer" EXCLUDE_FROM_ALL)
add_subdirectory("${THIRD_PARTY_OS_ROOT}/fastgltf-0.9.0" "${CMAKE_BINARY_DIR}/_deps/fastgltf" EXCLUDE_FROM_ALL)
add_subdirectory("${THIRD_PARTY_OS_ROOT}/DirectXTex-mar2026" "${CMAKE_BINARY_DIR}/_deps/DirectXTex" EXCLUDE_FROM_ALL)

# MikkTSpace
add_library(mikktspace STATIC "${THIRD_PARTY_OS_ROOT}/MikkTSpace/mikktspace.c")
target_include_directories(mikktspace PUBLIC "${THIRD_PARTY_OS_ROOT}/MikkTSpace")

# Iskur scene packer
file(GLOB ISKUR_SCENE_GEN_SOURCES
  "${ISKUR_ROOT}/code/tools/IskurScenePacker/*.cpp"
  "${ISKUR_ROOT}/code/tools/IskurScenePacker/*.h"
  "${ISKUR_ROOT}/code/common/*.cpp"
  "${ISKUR_ROOT}/code/common/*.h"
  "${ISKUR_ROOT}/data/shaders/*CPUGPU.h"
)

add_executable(IskurScenePacker ${ISKUR_SCENE_GEN_SOURCES})
target_compile_definitions(IskurScenePacker PRIVATE
  _HAS_EXCEPTIONS=0
  UNICODE
  _UNICODE
  ISKUR_ROOT="${ISKUR_ROOT}"
  NOMINMAX
)
target_include_directories(IskurScenePacker PRIVATE
  "${AGILITY_INCLUDE_DIR}"
  "${ISKUR_ROOT}/data"
  "${ISKUR_ROOT}/code"
)
target_link_libraries(IskurScenePacker PRIVATE
  meshoptimizer
  fastgltf::fastgltf
  DirectXTex
  mikktspace
  ole32
)
target_link_options(IskurScenePacker PRIVATE "/SUBSYSTEM:CONSOLE")
