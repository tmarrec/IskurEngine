# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

cmake_minimum_required(VERSION 4.0.2)
project(IskurEngine LANGUAGES CXX)

message(STATUS "Using toolchain: ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -- Dependencies ----------------------------------------------------------------
find_package(meshoptimizer CONFIG REQUIRED)
find_package(d3d12-memory-allocator CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)
find_package(directxmesh CONFIG REQUIRED)

find_path(TINYGLTF_INCLUDE_DIR NAMES "tiny_gltf.h")
find_path(D3DX12_INCLUDE_DIR NAMES "d3dx12.h")

# wrap header-only libs
add_library(tinygltf_hdr INTERFACE)
target_include_directories(tinygltf_hdr INTERFACE ${TINYGLTF_INCLUDE_DIR})

add_library(d3dx12_hdr INTERFACE)
target_include_directories(d3dx12_hdr INTERFACE ${D3DX12_INCLUDE_DIR})

# -- Common compile settings ----------------------------------------------------
add_library(common_settings INTERFACE)

target_compile_options(common_settings INTERFACE
    /W3                             # Warning level 3
    /EHs-c-                         # Disable exceptions
    /utf-8                          # Treat sources as UTF-8
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/GL>
    $<$<CONFIG:Release>:/arch:AVX2>
)

target_compile_definitions(common_settings INTERFACE
_HAS_EXCEPTIONS=0 # Disable STL exceptions
UNICODE
_UNICODE
)

# Common libraries
target_link_libraries(common_settings INTERFACE
    meshoptimizer::meshoptimizer
    unofficial::D3D12MemoryAllocator
    dxguid
    d3d12
    dxgi
    dxcompiler
    Microsoft::DirectXMesh
    tinygltf_hdr
    d3dx12_hdr
)

# Precompiled header
set(PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/code/pch.h")

# -- Engine Target ---------------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES
    code/*.cpp
    code/*.h
    code/*.natvis
)
# Remove anything inside code/unittests from ENGINE_SOURCES
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/code/unittests/.*")
add_executable(${PROJECT_NAME} ${ENGINE_SOURCES})
target_precompile_headers(${PROJECT_NAME} PRIVATE ${PCH_HEADER})

target_link_libraries(${PROJECT_NAME} PRIVATE
    common_settings
)
 
set_target_properties(${PROJECT_NAME} PROPERTIES
  WIN32_EXECUTABLE ON
  LINK_FLAGS "/SUBSYSTEM:WINDOWS"
)

# -- Unit Tests ------------------------------------------------------------------
file(GLOB_RECURSE UNITTEST_SOURCES
    ${ENGINE_SOURCES}
    code/unittests/*.cpp
    code/unittests/*.h
)

add_executable(${PROJECT_NAME}_unittests ${UNITTEST_SOURCES})
target_precompile_headers(${PROJECT_NAME}_unittests PRIVATE ${PCH_HEADER})

target_link_libraries(${PROJECT_NAME}_unittests PRIVATE
    common_settings
    Catch2::Catch2WithMain
)

# -- VS IntelliSense hack for clang-cl: add /I flags for all externals ---------
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    get_target_property(_incs1 meshoptimizer::meshoptimizer INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_incs2 unofficial::D3D12MemoryAllocator INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_incs3 Microsoft::DirectXMesh INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_incs4 Catch2::Catch2WithMain INTERFACE_INCLUDE_DIRECTORIES)

    set(_all_includes
        ${TINYGLTF_INCLUDE_DIR}
        ${D3DX12_INCLUDE_DIR}
        ${_incs1}
        ${_incs2}
        ${_incs3}
        ${_incs4}
    )

    foreach(_inc IN LISTS _all_includes)
        target_compile_options(${PROJECT_NAME} PRIVATE "/I${_inc}")
        target_compile_options(${PROJECT_NAME}_unittests PRIVATE "/I${_inc}")
    endforeach()
endif()


# -- Visual Studio Startup --------------------------------------------------------
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})