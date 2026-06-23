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

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

# Defines a C++ library that automatically tokenizes versioned enums.
#
# Args:
#   ENUM_HEADERS: Standard .h files containing enum definitions to tokenize (required).
#   HEADERS: Public non-enum headers of the library.
#   SOURCES: C++ source files of the library.
#   PUBLIC_DEPS: Public dependencies.
#   PRIVATE_DEPS: Private dependencies.
#   PUBLIC_INCLUDES: Prefix to strip from the include path of generated headers.
function(pw_add_enum_library NAME)
  pw_parse_arguments(
    NUM_POSITIONAL_ARGS 1
    ONE_VALUE_ARGS PUBLIC_INCLUDES
    MULTI_VALUE_ARGS ENUM_HEADERS HEADERS SOURCES PUBLIC_DEPS PRIVATE_DEPS
  )

  if(NOT arg_ENUM_HEADERS)
    message(FATAL_ERROR "pw_add_enum_library requires ENUM_HEADERS")
  endif()
  set(enum_headers ${arg_ENUM_HEADERS})
  set(other_headers ${arg_HEADERS})

  list(LENGTH arg_PUBLIC_INCLUDES public_includes_len)
  if(NOT public_includes_len EQUAL 1)
    message(FATAL_ERROR "pw_add_enum_library requires exactly one value for PUBLIC_INCLUDES")
  endif()

  if("${arg_PUBLIC_INCLUDES}" STREQUAL ".")
    message(FATAL_ERROR "pw_add_enum_library does not allow '.' as PUBLIC_INCLUDES. "
        "Since CMake doesn't isolate headers, it is too easy for sources to "
        "accidentally include original headers instead of generated ones.")
  endif()

  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/${NAME}")

  # Write the placeholder C++ source file.
  file(WRITE "${out_dir}/base.cc" "// placeholder\n")

  # Create an internal target to collect includes and options from dependencies.
  add_library("${NAME}._base_lib" OBJECT "${out_dir}/base.cc")
  target_include_directories("${NAME}._base_lib" PRIVATE
      "${arg_PUBLIC_INCLUDES}"
  )
  target_link_libraries("${NAME}._base_lib" PRIVATE
      pw_enum._generate_internal_do_not_use
      ${arg_PUBLIC_DEPS}
      ${arg_PRIVATE_DEPS}
  )

  # Determine the C++ standard to use.
  get_target_property(target_cxx_std "${NAME}._base_lib" CXX_STANDARD)
  if(NOT target_cxx_std)
    set(target_cxx_std "${CMAKE_CXX_STANDARD}")
  endif()
  if(NOT target_cxx_std)
    set(target_cxx_std "17")
  endif()

  # Collect compilation flags.
  set(content "")
  string(REPLACE " " "\n" cxx_flags "${CMAKE_CXX_FLAGS}")
  set(content "${cxx_flags}\n")
  set(content "${content}-std=c++${target_cxx_std}\n")
  set(content "${content}$<IF:$<BOOL:$<TARGET_PROPERTY:${NAME}._base_lib,INCLUDE_DIRECTORIES>>,-I$<JOIN:$<TARGET_PROPERTY:${NAME}._base_lib,INCLUDE_DIRECTORIES>,\n-I>\n,>")
  set(content "${content}$<IF:$<BOOL:$<TARGET_PROPERTY:${NAME}._base_lib,COMPILE_DEFINITIONS>>,-D$<JOIN:$<TARGET_PROPERTY:${NAME}._base_lib,COMPILE_DEFINITIONS>,\n-D>\n,>")
  set(content "${content}${out_dir}/base.cc\n")

  file(GENERATE
    OUTPUT "${out_dir}/flags-$<COMPILE_LANGUAGE>.txt"
    CONTENT "${content}"
  )

  set(output_files "")
  foreach(header IN LISTS enum_headers)
    cmake_path(IS_ABSOLUTE header is_absolute)
    if(is_absolute)
      message(FATAL_ERROR
          "pw_add_enum_library ENUM_HEADERS must be relative, but '${header}' is absolute")
    endif()

    cmake_path(IS_PREFIX arg_PUBLIC_INCLUDES "${header}" NORMALIZE is_prefix)
    if(NOT is_prefix)
      message(FATAL_ERROR
          "pw_add_enum_library ENUM_HEADERS must be nested under the PUBLIC_INCLUDES path, "
          "but '${header}' is not nested under '${arg_PUBLIC_INCLUDES}'.")
    endif()

    cmake_path(RELATIVE_PATH header BASE_DIRECTORY "${arg_PUBLIC_INCLUDES}"
        OUTPUT_VARIABLE relative_header)
    list(APPEND output_files "${out_dir}/include/${relative_header}")
  endforeach()

  add_custom_command(
    OUTPUT ${output_files}
    COMMAND python3 $ENV{PW_ROOT}/pw_enum/py/pw_enum/generate.py
            ${enum_headers}
            "--outputs" ${output_files}
            "--compiler" "${CMAKE_CXX_COMPILER}"
            "--compiler-flags" "${out_dir}/flags-CXX.txt"
            "--base-cc" "${out_dir}/base.cc"
    DEPENDS ${enum_headers}
            "${out_dir}/flags-CXX.txt"
            "$ENV{PW_ROOT}/pw_enum/py/pw_enum/generate.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating versioned enums for ${NAME}"
  )

  add_custom_target("${NAME}.generate" DEPENDS ${output_files})

  if("${arg_SOURCES}" STREQUAL "")
    set(lib_type INTERFACE)
  else()
    set(lib_type STATIC)
  endif()

  pw_add_library("${NAME}" ${lib_type}
    SOURCES
      ${arg_SOURCES}
    GENERATED_HEADERS
      ${output_files}
    HEADERS
      ${other_headers}
    PUBLIC_DEPS
      pw_tokenizer
      pw_enum._generate_internal_do_not_use
      ${arg_PUBLIC_DEPS}
    PRIVATE_DEPS
      ${arg_PRIVATE_DEPS}
    # Use -iquote so generated headers take priority over source directories.
    PUBLIC_COMPILE_OPTIONS
      "-iquote${out_dir}/include"
  )

  add_dependencies("${NAME}" "${NAME}.generate")
endfunction()
