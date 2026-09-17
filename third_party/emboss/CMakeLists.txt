# Copyright 2024 The Pigweed Authors
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
include($ENV{PW_ROOT}/third_party/emboss/emboss.cmake)

set(dir_pw_third_party_emboss "" CACHE PATH "Path to the emboss installation.")

if("${dir_pw_third_party_emboss}" STREQUAL "")
  pw_add_error_target(pw_third_party.emboss
    MESSAGE
    "Attempted to use emboss without configuring it, see "
    "pigweed.dev/third_party/emboss/"
  )
  return()
endif()

configure_file(
  runtime/cpp/emboss_defines.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/runtime/cpp/emboss_defines.h
  @ONLY
)

if(EXISTS ${dir_pw_third_party_emboss}/gen/sources.cmake)
  include(${dir_pw_third_party_emboss}/gen/sources.cmake)
  set(emboss_headers)
  foreach(source IN LISTS EMBOSS_RUNTIME_CPP_SOURCES)
    list(APPEND emboss_headers "${dir_pw_third_party_emboss}/${source}")
  endforeach()
else()
  set(emboss_headers
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_arithmetic.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_arithmetic_all_known_generated.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_arithmetic_maximum_operation_generated.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_array_view.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_bit_util.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_constant_view.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_cpp_types.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_cpp_util.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_defines.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_enum_view.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_maybe.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_memory_util.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_prelude.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_text_util.h
    ${dir_pw_third_party_emboss}/runtime/cpp/emboss_view_parameters.h
  )
endif()

pw_add_library(pw_third_party.emboss.cpp_utils INTERFACE
  # Disable the sandbox since the headers are not nested under this directory.
  SANDBOX
    OFF
  HEADERS
    ${CMAKE_CURRENT_BINARY_DIR}/include/runtime/cpp/emboss_defines.h
    ${emboss_headers}
  PUBLIC_INCLUDES
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${dir_pw_third_party_emboss}
  PUBLIC_DEPS
    pw_assert.assert
)
