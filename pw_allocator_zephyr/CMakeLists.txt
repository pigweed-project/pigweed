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

if(NOT CONFIG_ZEPHYR_PIGWEED_MODULE)
  return()
endif()

pw_add_library(pw_allocator_zephyr.synchronized_heap_allocator STATIC
  HEADERS
    public/pw_allocator_zephyr/synchronized_heap_allocator.h
  PUBLIC_INCLUDES
    public
  SOURCES
    synchronized_heap_allocator.cc
  PUBLIC_DEPS
    pw_allocator.abstract_allocator
    zephyr_interface
)

pw_add_library(pw_allocator_zephyr.system_heap_allocator STATIC
  HEADERS
    public/pw_allocator_zephyr/system_heap_allocator.h
  PUBLIC_INCLUDES
    public
  SOURCES
    system_heap_allocator.cc
  PUBLIC_DEPS
    pw_allocator.abstract_allocator
    zephyr_interface
)

if(CONFIG_PIGWEED_ALLOCATOR)
  zephyr_append_cmake_library(pw_allocator_zephyr.synchronized_heap_allocator)
  zephyr_append_cmake_library(pw_allocator_zephyr.system_heap_allocator)
endif()

pw_add_test(pw_allocator_zephyr.heap_allocator_test
  SOURCES
    heap_allocator_test.cc
  PRIVATE_DEPS
    pw_allocator_zephyr.synchronized_heap_allocator
    pw_allocator_zephyr.system_heap_allocator
    pw_unit_test
    zephyr_interface
  GROUPS
    modules
    pw_allocator_zephyr
)
