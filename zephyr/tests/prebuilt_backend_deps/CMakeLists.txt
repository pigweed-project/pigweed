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
project(PigweedZephyrPrebuiltBackendDepsTest)
enable_language(C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")

# Stands in for a library that another build system (such as Bazel or GN)
# compiles and the application links into the Zephyr image as a prebuilt
# archive. app.prebuilt_build compiles the archive with the Pigweed headers it
# uses but links nothing, as another build system's output would.
set(prebuilt_dir "${CMAKE_CURRENT_BINARY_DIR}/prebuilt")
set(prebuilt_name
    "${CMAKE_STATIC_LIBRARY_PREFIX}prebuilt${CMAKE_STATIC_LIBRARY_SUFFIX}")

add_library(app.prebuilt_build STATIC prebuilt.cc)
target_link_libraries(app.prebuilt_build
  PRIVATE
    zephyr_interface
    $<COMPILE_ONLY:pw_chrono.system_timer>
    $<COMPILE_ONLY:pw_thread.sleep>
)
add_dependencies(app.prebuilt_build zephyr_generated_headers)
set_target_properties(app.prebuilt_build
  PROPERTIES
    OUTPUT_NAME prebuilt
    ARCHIVE_OUTPUT_DIRECTORY "${prebuilt_dir}"
)

# The application imports the archive. The CONFIG_PIGWEED_* options only set
# the backends, as pw_set_backend() does, so the imported library declares the
# Pigweed libraries it uses, which brings in their backends. An imported
# library isn't compiled, so these are INTERFACE dependencies.
add_library(app.prebuilt STATIC IMPORTED)
set_target_properties(app.prebuilt
  PROPERTIES
    IMPORTED_LOCATION "${prebuilt_dir}/${prebuilt_name}"
)
target_link_libraries(app.prebuilt
  INTERFACE
    pw_chrono.system_timer
    pw_thread.sleep
)

target_sources(app PRIVATE main.cc)
target_link_libraries(app PRIVATE app.prebuilt)
add_dependencies(app app.prebuilt_build)
