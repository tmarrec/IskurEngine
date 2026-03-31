# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

# C++ Settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake helpers
function(first_existing_dir out_var)
  set(result "")
  foreach(candidate IN LISTS ARGN)
    if (EXISTS "${candidate}")
      set(result "${candidate}")
      break()
    endif()
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(copy_directory_next_to_target target source_dir comment)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMENT "${comment}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${source_dir}" "$<TARGET_FILE_DIR:${target}>"
  )
endfunction()

function(copy_files_next_to_target target comment)
  if (ARGN)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMENT "${comment}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${ARGN}
              "$<TARGET_FILE_DIR:${target}>"
    )
  endif()
endfunction()

function(set_all_output_dirs root_dir)
  if(CMAKE_CONFIGURATION_TYPES)
    set(configs ${CMAKE_CONFIGURATION_TYPES})
  else()
    set(configs Debug Release RelWithDebInfo MinSizeRel)
  endif()

  foreach(kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${kind}_OUTPUT_DIRECTORY "${root_dir}" PARENT_SCOPE)
  endforeach()
  foreach(cfg IN LISTS configs)
    string(TOUPPER "${cfg}" cfg_upper)
    foreach(kind RUNTIME LIBRARY ARCHIVE)
      set(CMAKE_${kind}_OUTPUT_DIRECTORY_${cfg_upper} "${root_dir}/${cfg}" PARENT_SCOPE)
    endforeach()
  endforeach()
endfunction()

if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo;MinSizeRel" CACHE STRING "Supported build configurations" FORCE)
endif()

function(enable_ipo_for_target target)
  set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
endfunction()

# Output directories
set_all_output_dirs("${CMAKE_SOURCE_DIR}/bin")

# MSVC runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(_ISKUR_OPT_CONFIG_EXPR "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>")

# Use shipping-style linker behavior for optimized and profiling configurations.
add_link_options(
  "$<${_ISKUR_OPT_CONFIG_EXPR}:/INCREMENTAL:NO>"
  "$<${_ISKUR_OPT_CONFIG_EXPR}:/OPT:REF>"
  "$<${_ISKUR_OPT_CONFIG_EXPR}:/OPT:ICF>"
)

add_compile_options("$<$<AND:${_ISKUR_OPT_CONFIG_EXPR},$<COMPILE_LANGUAGE:C,CXX>>:/fp:fast>")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_compile_options("$<$<AND:${_ISKUR_OPT_CONFIG_EXPR},$<COMPILE_LANGUAGE:C,CXX>>:/arch:AVX2>")
endif()
