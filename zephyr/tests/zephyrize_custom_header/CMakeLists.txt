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

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(PigweedZephyrZephyrizeCustomHeaderTest)
enable_language(C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")

# With CONFIG_LOG_CUSTOM_HEADER, every user of <zephyr/logging/log.h>,
# including Zephyr's own kernel and subsystems, includes an
# application-provided zephyr_custom_log.h. The library that provides it is
# registered with pw_zephyrize_libraries_ifdef(), which must make it available
# to all of Zephyr, not just to targets that link the library directly.
add_library(app.custom_log_header INTERFACE)
target_include_directories(app.custom_log_header
  INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/custom_log_header"
)
pw_zephyrize_libraries_ifdef(CONFIG_LOG_CUSTOM_HEADER app.custom_log_header)

# app links zephyr_interface itself, so main.c only checks the application.
# zephyr_library.c is built the way Zephyr's own libraries are, so it checks
# that the header reaches them too.
zephyr_library_named(app_zephyr_library)
zephyr_library_sources(zephyr_library.c)
# find_package(Zephyr) orders its own libraries after the generated headers;
# a library the application adds later has to do it itself.
add_dependencies(app_zephyr_library zephyr_generated_headers)

target_sources(app PRIVATE main.c)
