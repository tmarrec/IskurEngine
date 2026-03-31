# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

cmake_minimum_required(VERSION 4.3)
project(IskurEngine LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectSetup.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdParty.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Targets.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Streamline.cmake")

# IDE Settings
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME}
)

