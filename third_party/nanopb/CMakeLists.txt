# Copyright 2020 The Pigweed Authors
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
include($ENV{PW_ROOT}/pw_protobuf_compiler/proto.cmake)

set(dir_pw_third_party_nanopb "" CACHE PATH "Path to the Nanopb installation.")
option(pw_third_party_nanopb_ADD_SUBDIRECTORY
    "Whether to add the dir_pw_third_party_nanopb subdirectory" OFF)

if("${dir_pw_third_party_nanopb}" STREQUAL "")
  pw_add_error_target(pw_third_party.nanopb
    MESSAGE
      "Attempted to use nanopb without configuring it, see "
      "pigweed.dev/third_party/nanopb/"
  )
  return()
elseif(pw_third_party_nanopb_ADD_SUBDIRECTORY)
  add_subdirectory("${dir_pw_third_party_nanopb}" third_party/nanopb)
endif()

if(CONFIG_ZEPHYR_PIGWEED_MODULE)
# The nanopb Zephyr module adds nanopb to the Zephyr interface
# Use this target, as the native nanopb target is not aware of Zephyr-provided
# build flags
set(nanopb_main_library zephyr_interface)
else()
set(nanopb_main_library
    $<IF:$<TARGET_EXISTS:protobuf-nanopb-static>,protobuf-nanopb-static,protobuf-nanopb>)
endif()

add_library(pw_third_party.nanopb INTERFACE)
target_link_libraries(pw_third_party.nanopb INTERFACE "${nanopb_main_library}")
target_include_directories(pw_third_party.nanopb
  INTERFACE
    "${dir_pw_third_party_nanopb}"
)

pw_proto_library(pw_third_party.nanopb.proto
  SOURCES
    "${dir_pw_third_party_nanopb}/generator/proto/nanopb.proto"
  STRIP_PREFIX
    "${dir_pw_third_party_nanopb}/generator/proto"
)

# Generates nanopb_pb2.py, which is needed to compile protobufs with Nanopb.
add_custom_command(
  COMMAND
    python3
    "${CMAKE_CURRENT_LIST_DIR}/generate_nanopb_proto.py"
    --nanopb-root "${dir_pw_third_party_nanopb}"
    --nanopb-pb2-dir "${_pw_protobuf_compiler_NANOPB_PB2_DIR}"
    --protoc-binary "$ENV{PW_PIGWEED_CIPD_INSTALL_DIR}/bin/protoc"
  DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/generate_nanopb_proto.py"
    "${dir_pw_third_party_nanopb}/pb.h"
    "${dir_pw_third_party_nanopb}/pb_common.h"
    "${dir_pw_third_party_nanopb}/pb_decode.h"
    "${dir_pw_third_party_nanopb}/pb_encode.h"
    "${dir_pw_third_party_nanopb}/pb_common.c"
    "${dir_pw_third_party_nanopb}/pb_decode.c"
    "${dir_pw_third_party_nanopb}/pb_encode.c"
    "${dir_pw_third_party_nanopb}/generator/nanopb_generator.py"
    "${dir_pw_third_party_nanopb}/generator/proto/google/protobuf"
    "${dir_pw_third_party_nanopb}/generator/proto/google/protobuf/descriptor.proto"
    "${dir_pw_third_party_nanopb}/generator/proto/__init__.py"
    "${dir_pw_third_party_nanopb}/generator/proto/nanopb.proto"
    "${dir_pw_third_party_nanopb}/generator/proto/_utils.py"
    "${dir_pw_third_party_nanopb}/generator/protoc-gen-nanopb"
    "${dir_pw_third_party_nanopb}/generator/protoc"
    "${dir_pw_third_party_nanopb}/generator/protoc-gen-nanopb.bat"
    "${dir_pw_third_party_nanopb}/generator/protoc.bat"
    "$ENV{PW_PIGWEED_CIPD_INSTALL_DIR}/bin/protoc"
  OUTPUT
    "${_pw_protobuf_compiler_NANOPB_PB2_DIR}/nanopb_pb2.py"
)
add_custom_target(pw_third_party.nanopb.generate_proto
  DEPENDS
    "${_pw_protobuf_compiler_NANOPB_PB2_DIR}/nanopb_pb2.py"
)
