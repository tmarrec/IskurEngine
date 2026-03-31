# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

# Common settings target
add_library(common_settings INTERFACE)
target_compile_definitions(common_settings INTERFACE
  _HAS_EXCEPTIONS=0
  UNICODE
  _UNICODE
  NOMINMAX
)
target_include_directories(common_settings INTERFACE
  "${AGILITY_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/data"
  "${CMAKE_CURRENT_SOURCE_DIR}/code"
)

# Engine settings target
add_library(engine_settings INTERFACE)
target_link_libraries(engine_settings INTERFACE
  common_settings
  dxguid
  d3d12
  dxgi
  D3D12MemoryAllocator
  imgui
  DirectXTex
  dxcompiler
)

# Scene packer settings target
add_library(packer_settings INTERFACE)
target_link_libraries(packer_settings INTERFACE
  common_settings
  meshoptimizer
  fastgltf::fastgltf
  DirectXTex
  mikktspace
  ole32
)

# Engine executable
set(PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/code/pch.h")
file(GLOB_RECURSE ENGINE_SOURCES
  code/*.cpp
  code/*.h
  data/shaders/*CPUGPU.h
)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/code/tools/.*")

add_executable(${PROJECT_NAME} ${ENGINE_SOURCES})
target_precompile_headers(${PROJECT_NAME} PRIVATE ${PCH_HEADER})
target_link_libraries(${PROJECT_NAME} PRIVATE engine_settings)
target_link_options(${PROJECT_NAME} PRIVATE
  "/SUBSYSTEM:WINDOWS"
)
enable_ipo_for_target(${PROJECT_NAME})

# Copy DXC runtime next to the exe
copy_directory_next_to_target(${PROJECT_NAME} "${DXC_BIN_DIR}" "Copying DXC runtime")

# Iskur scene packer
file(GLOB ISKUR_SCENE_GEN_SOURCES
  code/tools/IskurScenePacker/*.cpp
  code/tools/IskurScenePacker/*.h
  code/common/*.cpp
  code/common/*.h
  data/shaders/*CPUGPU.h
)
add_executable(IskurScenePacker ${ISKUR_SCENE_GEN_SOURCES})
target_link_libraries(IskurScenePacker PRIVATE
  packer_settings
)
target_compile_definitions(IskurScenePacker PRIVATE
  ISKUR_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
  NOMINMAX
)
target_link_options(IskurScenePacker PRIVATE "/SUBSYSTEM:CONSOLE")
enable_ipo_for_target(IskurScenePacker)
