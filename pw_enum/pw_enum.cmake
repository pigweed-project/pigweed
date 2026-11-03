# Copyright 2026 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include_guard(GLOBAL)

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

# Sets OUT_VAR to a PYTHONPATH that prefers the pw_enum and pw_codegen sources
# over any installed packages.
function(_pw_enum_python_path OUT_VAR)
  if("${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "Windows")
    set(separator ";")
  else()
    set(separator ":")
  endif()
  set(python_path "$ENV{PW_ROOT}/pw_enum/py${separator}$ENV{PW_ROOT}/pw_codegen/py")
  if(NOT "$ENV{PYTHONPATH}" STREQUAL "")
    set(python_path "${python_path}${separator}$ENV{PYTHONPATH}")
  endif()
  set("${OUT_VAR}" "${python_path}" PARENT_SCOPE)
endfunction()

# Declares language-neutral enums defined in TOML or JSON files, and generates
# C++ code for them in the <NAME>.cc library, which has a generated header for
# each source.
#
# A source at bar/baz/foo.toml generates the header bar/baz/foo.h. By default,
# paths are relative to CMAKE_SOURCE_DIR. Use STRIP_PREFIX and PREFIX to change
# the paths, which is necessary for paths that don't depend on where the
# sources are checked out.
#
# Args:
#   SOURCES: TOML or JSON enum definitions (required).
#   DEPS: Other pw_add_enum targets whose enumerators the sources reference.
#   STRIP_PREFIX: Directory to remove from the paths of the sources. Relative
#       to the current source directory.
#   PREFIX: Prefix to add to the paths of the sources.
function(pw_add_enum NAME)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS 1
    ONE_VALUE_ARGS STRIP_PREFIX PREFIX
    MULTI_VALUE_ARGS SOURCES DEPS
    REQUIRED_ARGS SOURCES
  )

  if("${arg_STRIP_PREFIX}" STREQUAL "")
    set(root "${CMAKE_SOURCE_DIR}")
  else()
    get_filename_component(root "${arg_STRIP_PREFIX}" ABSOLUTE)
  endif()

  set(prefix "")
  if(NOT "${arg_PREFIX}" STREQUAL "")
    set(prefix "${arg_PREFIX}/")
  endif()

  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/${NAME}")

  set(sources "")
  set(headers "")
  foreach(source IN LISTS arg_SOURCES)
    get_filename_component(source "${source}" ABSOLUTE)
    if(NOT source MATCHES "\\.(toml|json)$")
      message(FATAL_ERROR
          "pw_add_enum sources must be .toml or .json files: ${source}")
    endif()

    cmake_path(IS_PREFIX root "${source}" NORMALIZE is_prefix)
    if(NOT is_prefix)
      message(FATAL_ERROR
          "pw_add_enum source ${source} is not under STRIP_PREFIX ${root}")
    endif()

    cmake_path(RELATIVE_PATH source BASE_DIRECTORY "${root}"
        OUTPUT_VARIABLE relative_source)
    string(REGEX REPLACE "\\.(toml|json)$" ".h" header "${relative_source}")

    list(APPEND sources "${source}")
    list(APPEND headers "${out_dir}/include/${prefix}${header}")
  endforeach()

  # This target holds the sources for pw_add_enum targets that depend on it.
  # Generator expressions collect the sources of transitive deps, so deps may
  # be declared in any order. GENEX_EVAL evaluates the generator expressions
  # stored in deps' properties.
  add_library("${NAME}" INTERFACE)
  set(transitive_sources "${sources}")
  set(dep_sources "")
  foreach(dep IN LISTS arg_DEPS)
    list(APPEND transitive_sources
        "$<GENEX_EVAL:$<TARGET_PROPERTY:${dep},PW_ENUM_TRANSITIVE_SOURCES>>")
    list(APPEND dep_sources "$<TARGET_PROPERTY:${dep},PW_ENUM_SOURCES>")
  endforeach()
  set_target_properties("${NAME}" PROPERTIES
    PW_ENUM_SOURCES "${sources}"
    PW_ENUM_TRANSITIVE_SOURCES "${transitive_sources}"
  )

  set(transitive_genex "$<REMOVE_DUPLICATES:$<GENEX_EVAL:$<TARGET_PROPERTY:${NAME},PW_ENUM_TRANSITIVE_SOURCES>>>")
  file(GENERATE
    OUTPUT "${out_dir}/dep_srcs.txt"
    CONTENT "$<JOIN:${dep_sources},\n>\n"
  )
  file(GENERATE
    OUTPUT "${out_dir}/transitive_srcs.txt"
    CONTENT "$<JOIN:${transitive_genex},\n>\n"
  )

  set(codegen "$ENV{PW_ROOT}/pw_enum/py/pw_enum/codegen.py")
  _pw_enum_python_path(python_path)
  add_custom_command(
    OUTPUT ${headers}
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${python_path}"
            python3 "${codegen}" cc
            --sources ${sources}
            --deps "@${out_dir}/dep_srcs.txt"
            --transitive-deps "@${out_dir}/transitive_srcs.txt"
            --outputs ${headers}
    DEPENDS "${codegen}"
            "${out_dir}/dep_srcs.txt"
            "${out_dir}/transitive_srcs.txt"
            "${transitive_genex}"
    COMMENT "Generating pw_enum C++ headers for ${NAME}"
    VERBATIM
    COMMAND_EXPAND_LISTS
  )
  add_custom_target("${NAME}.generate" DEPENDS ${headers})

  # pw_add_library_generic allows the .cc suffix in the target name.
  pw_add_library_generic("${NAME}.cc" INTERFACE
    GENERATED_HEADERS
      ${headers}
    PUBLIC_INCLUDES
      "${out_dir}/include"
    PUBLIC_DEPS
      pw_enum
      pw_log.args
      pw_status
      pw_tokenizer
  )
  add_dependencies("${NAME}.cc" "${NAME}.generate")
endfunction()
