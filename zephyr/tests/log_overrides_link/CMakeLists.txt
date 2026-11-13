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
include($ENV{PW_ROOT}/pw_log_string/backend.cmake)

# With CONFIG_PIGWEED_LOG_STRING, the application implements
# pw_log_string_HandleMessageVaList() (in main.cc). Backends are read when
# Zephyr processes Pigweed's CMake, so set it before find_package(Zephyr).
pw_set_backend(pw_log_string.handler app.log_string_handler)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(PigweedZephyrLogOverridesLinkTest)
enable_language(C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")

add_library(app.log_string_handler INTERFACE)

# CONFIG_PIGWEED_LOG_STRING redirects Zephyr's log macros, including
# LOG_HEXDUMP_*, to Pigweed. Code that uses them, such as Zephyr's own kernel
# and subsystems or zephyr_logging.c, doesn't link to Pigweed in CMake, so
# enabling the redirection must also link what the redirected macros call.
# Note that the app deliberately doesn't link to any Pigweed library.
target_sources(app PRIVATE main.cc zephyr_logging.c)
