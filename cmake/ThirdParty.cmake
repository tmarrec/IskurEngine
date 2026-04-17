# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

# Third-party roots
set(THIRD_PARTY_OS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/open_source")
set(AGILITY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/proprietary/Agility/build/native/include")

# DXC (DirectX Shader Compiler)
set(DXC_SDK_ROOT "${THIRD_PARTY_OS_ROOT}/dxc_2026_02_20")
set(DXC_BIN_DIR "${DXC_SDK_ROOT}/bin/x64")

add_library(dxcompiler STATIC IMPORTED)
set_target_properties(dxcompiler PROPERTIES
  IMPORTED_LOCATION "${DXC_SDK_ROOT}/lib/x64/dxcompiler.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${DXC_SDK_ROOT}/inc"
)
set(DIRECTX_DXC_PATH "${DXC_BIN_DIR}")

# Open-source dependencies used by the engine target.
set(ENGINE_OPEN_SOURCE_DEPENDENCIES
  D3D12MemoryAllocator-3.1.0
  DirectXTex-mar2026
)
foreach(dep IN LISTS ENGINE_OPEN_SOURCE_DEPENDENCIES)
  add_subdirectory("${THIRD_PARTY_OS_ROOT}/${dep}" EXCLUDE_FROM_ALL)
endforeach()

# Open-source dependencies used by the scene packer target.
set(PACKER_OPEN_SOURCE_DEPENDENCIES
  meshoptimizer-1.1
  fastgltf-0.9.0
)
foreach(dep IN LISTS PACKER_OPEN_SOURCE_DEPENDENCIES)
  add_subdirectory("${THIRD_PARTY_OS_ROOT}/${dep}" EXCLUDE_FROM_ALL)
endforeach()

# ImGui
set(IMGUI_DIR "${THIRD_PARTY_OS_ROOT}/imgui-1.92.7")
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")

add_library(mikktspace STATIC "${THIRD_PARTY_OS_ROOT}/MikkTSpace/mikktspace.cpp")
target_include_directories(mikktspace PUBLIC "${THIRD_PARTY_OS_ROOT}/MikkTSpace")
