# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

# Streamline / DLSS
set(STREAMLINE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/proprietary/Streamline")

set(STREAMLINE_BIN_DIR "${STREAMLINE_SDK_DIR}/bin/x64")

if (STREAMLINE_BIN_DIR)
  set(STREAMLINE_RUNTIME_DLLS
    "${STREAMLINE_BIN_DIR}/sl.interposer.dll"
    "${STREAMLINE_BIN_DIR}/sl.common.dll"
    "${STREAMLINE_BIN_DIR}/sl.dlss.dll"
    "${STREAMLINE_BIN_DIR}/sl.pcl.dll"
    "${STREAMLINE_BIN_DIR}/nvngx_dlss.dll"
  )
  foreach(_dll IN LISTS STREAMLINE_RUNTIME_DLLS)
    if (NOT EXISTS "${_dll}")
      message(FATAL_ERROR "Required Streamline runtime DLL missing: ${_dll}")
    endif()
  endforeach()

  copy_files_next_to_target(${PROJECT_NAME} "Copying Streamline DLSS runtime" ${STREAMLINE_RUNTIME_DLLS})

  # Clean up stale DLLs that may exist from previous builds when we copied all plugins.
  set(STREAMLINE_UNUSED_DLLS
    sl.deepdvc.dll
    sl.directsr.dll
    sl.dlss_d.dll
    sl.dlss_g.dll
    sl.imgui.dll
    sl.nis.dll
    sl.nvperf.dll
    sl.reflex.dll
    nvngx_deepdvc.dll
    nvngx_dlssd.dll
    nvngx_dlssg.dll
  )

  set(STREAMLINE_UNUSED_DLL_TARGET_PATHS)
  foreach(_unused_dll IN LISTS STREAMLINE_UNUSED_DLLS)
    list(APPEND STREAMLINE_UNUSED_DLL_TARGET_PATHS "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${_unused_dll}")
  endforeach()

  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMENT "Removing unused Streamline runtime DLLs"
    COMMAND ${CMAKE_COMMAND} -E rm -f ${STREAMLINE_UNUSED_DLL_TARGET_PATHS}
  )

  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMENT "Removing stale Streamline ImGui config"
    COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:${PROJECT_NAME}>/sl.imgui.json"
  )

  # Force Streamline NGX OTA off at runtime to avoid updater process spikes/noise.
  # Prefer NVIDIA's reference config (already contains enableOTA:false) to keep
  # expected schema/fields intact.
  set(STREAMLINE_COMMON_JSON_PATH "${STREAMLINE_SDK_DIR}/scripts/sl.common.json")
  if (NOT EXISTS "${STREAMLINE_COMMON_JSON_PATH}")
    set(STREAMLINE_COMMON_JSON_PATH "${CMAKE_CURRENT_BINARY_DIR}/sl.common.json")
    file(WRITE "${STREAMLINE_COMMON_JSON_PATH}" "{\n  \"enableOTA\": false\n}\n")
  endif()
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMENT "Copying Streamline runtime config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMLINE_COMMON_JSON_PATH}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/sl.common.json"
  )

  set(STREAMLINE_INTERPOSER_JSON_PATH "${CMAKE_CURRENT_BINARY_DIR}/sl.interposer.json")
  file(WRITE "${STREAMLINE_INTERPOSER_JSON_PATH}"
    "{\n"
    "  \"enableInterposer\": true,\n"
    "  \"useDXGIProxy\": true,\n"
    "  \"loadAllFeatures\": false,\n"
    "  \"showConsole\": false,\n"
    "  \"logLevel\": 2,\n"
    "  \"logMessageDelayMs\": 5000,\n"
    "  \"waitForDebugger\": false,\n"
    "  \"vkValidation\": false,\n"
    "  \"forceProxies\": false,\n"
    "  \"forceNonNVDA\": false,\n"
    "  \"trackEngineAllocations\": false\n"
    "}\n"
  )
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMENT "Copying Streamline interposer config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STREAMLINE_INTERPOSER_JSON_PATH}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/sl.interposer.json"
  )

else()
  message(STATUS "Streamline bin directory not found (expected under ${STREAMLINE_SDK_DIR}).")
endif()

# Streamline headers/libs for DLSS
find_path(STREAMLINE_INCLUDE_DIR
  NAMES sl.h
  PATHS "${STREAMLINE_SDK_DIR}"
  PATH_SUFFIXES "include" "include/streamline"
  REQUIRED
)

find_library(STREAMLINE_LIB
  NAMES sl.interposer
  PATHS "${STREAMLINE_SDK_DIR}"
  PATH_SUFFIXES "lib/x64" "lib/win64" "lib" "lib/Release" "bin/x64" "bin"
  REQUIRED
)

add_library(NVIDIA::Streamline INTERFACE IMPORTED)
set_target_properties(NVIDIA::Streamline PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${STREAMLINE_INCLUDE_DIR}"
  INTERFACE_LINK_LIBRARIES "${STREAMLINE_LIB}"
)
target_link_libraries(engine_settings INTERFACE NVIDIA::Streamline)
message(STATUS "Streamline include dir : ${STREAMLINE_INCLUDE_DIR}")
message(STATUS "Streamline lib         : ${STREAMLINE_LIB}")

# DirectX 12 Agility SDK runtime (required at runtime by the engine executable)
set(AGILITY_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/proprietary/Agility")

unset(AGILITY_D3D12CORE_DLL CACHE)
unset(AGILITY_D3D12SDKLAYERS_DLL CACHE)

find_file(AGILITY_D3D12CORE_DLL
  NAMES D3D12Core.dll
  PATHS "${AGILITY_SDK_DIR}"
  PATH_SUFFIXES "build/native/bin/x64" "bin/x64" "bin"
  NO_DEFAULT_PATH
  REQUIRED
)

find_file(AGILITY_D3D12SDKLAYERS_DLL
  NAMES D3D12SDKLayers.dll
  PATHS "${AGILITY_SDK_DIR}"
  PATH_SUFFIXES "build/native/bin/x64" "bin/x64" "bin"
  NO_DEFAULT_PATH
  REQUIRED
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMENT "Copying D3D12 Agility runtime"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/D3D12"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${AGILITY_D3D12CORE_DLL}"
          "${AGILITY_D3D12SDKLAYERS_DLL}"
          "$<TARGET_FILE_DIR:${PROJECT_NAME}>/D3D12"
)

message(STATUS "Agility D3D12Core      : ${AGILITY_D3D12CORE_DLL}")
message(STATUS "Agility D3D12SDKLayers : ${AGILITY_D3D12SDKLAYERS_DLL}")
