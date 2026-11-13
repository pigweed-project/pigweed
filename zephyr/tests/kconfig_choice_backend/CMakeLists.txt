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
project(PigweedZephyrKconfigChoiceBackendTest)
enable_language(C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")

# prj.conf leaves every backend choice at its default, Pigweed's Zephyr
# backend.
foreach(facade_and_backend IN ITEMS
    "pw_chrono.system_clock:pw_chrono_zephyr.system_clock"
    "pw_sync.mutex:pw_sync_zephyr.mutex_backend"
    "pw_thread.sleep:pw_thread_zephyr.sleep")
  string(REPLACE ":" ";" facade_and_backend "${facade_and_backend}")
  list(GET facade_and_backend 0 facade)
  list(GET facade_and_backend 1 expected)
  if(NOT "${${facade}_BACKEND}" STREQUAL "${expected}")
    message(FATAL_ERROR
        "${facade} is '${${facade}_BACKEND}', expected the default choice's "
        "${expected}")
  endif()
endforeach()

target_sources(app PRIVATE main.cc)
target_link_libraries(app
  PRIVATE
    pw_sync.mutex
    pw_thread.sleep
)
