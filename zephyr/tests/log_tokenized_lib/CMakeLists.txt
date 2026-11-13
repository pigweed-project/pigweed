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

cmake_minimum_required(VERSION 3.20)

# Pigweed's CMake has to come before find_package(Zephyr), which is what
# sets ZEPHYR_PIGWEED_MODULE_DIR, so include pigweed.cmake by its path (an
# application includes it from its own checkout of Pigweed). It sets PW_ROOT
# for the files it includes if the environment doesn't.
include("${CMAKE_CURRENT_LIST_DIR}/../../../pw_build/pigweed.cmake")
include($ENV{PW_ROOT}/pw_log_tokenized/backend.cmake)

# CONFIG_PIGWEED_LOG_TOKENIZED_LIB tokenizes logs and leaves handling them to
# the application, which implements pw_log_tokenized_HandleLog() (in main.cc).
# Kconfig sets pw_log's backend; Pigweed must not replace the application's
# handler with one of its own. Backends are read when Zephyr processes
# Pigweed's CMake, so set the handler before find_package(Zephyr).
pw_set_backend(pw_log_tokenized.handler app.log_handler)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(PigweedZephyrLogTokenizedLibTest)
enable_language(C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")

add_library(app.log_handler INTERFACE)

if(NOT "${pw_log_BACKEND}" STREQUAL "pw_log_tokenized")
  message(FATAL_ERROR
      "pw_log is '${pw_log_BACKEND}', expected CONFIG_PIGWEED_LOG_TOKENIZED_LIB "
      "to set pw_log_tokenized")
endif()

# Linking can't tell which handler won: the application defines
# pw_log_tokenized_HandleLog() first, so Pigweed's copy is never pulled in.
# main.cc checks at run time that the application's handler gets the logs.
if(NOT "${pw_log_tokenized.handler_BACKEND}" STREQUAL "app.log_handler")
  message(FATAL_ERROR
      "pw_log_tokenized.handler is '${pw_log_tokenized.handler_BACKEND}', "
      "expected the application's app.log_handler")
endif()

target_sources(app PRIVATE main.cc)
target_link_libraries(app PRIVATE pw_log)
