# Copyright 2025 The Pigweed Authors
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

pw_add_library(pw_memory INTERFACE
  HEADERS
    public/pw_memory/container_of.h
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_memory.container_of_test
  SOURCES
    container_of_test.cc
  PRIVATE_DEPS
    pw_memory
  GROUPS
    modules
    pw_memory
)

pw_add_library(pw_memory.no_destructor INTERFACE
  HEADERS
    public/pw_memory/no_destructor.h
  PUBLIC_DEPS
    pw_polyfill
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_memory.no_destructor_test
  SOURCES
    no_destructor_test.cc
  PRIVATE_DEPS
    pw_assert.check
    pw_polyfill
    pw_memory.no_destructor
  GROUPS
    modules
    pw_memory
)

pw_add_library(pw_memory.globals INTERFACE
  HEADERS
    public/pw_memory/globals.h
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_memory.globals_test
  SOURCES
    globals_test.cc
  PRIVATE_DEPS
    pw_assert.check
    pw_polyfill
    pw_memory.globals
  GROUPS
    modules
    pw_memory
)

pw_add_library(pw_memory.busy_wait_forever INTERFACE
  HEADERS
    public/pw_memory/busy_wait_forever.h
  PUBLIC_DEPS
    pw_preprocessor
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_memory.busy_wait_forever_test
  SOURCES
    busy_wait_forever_test.cc
    busy_wait_forever_test_c.c
  PRIVATE_DEPS
    pw_memory.busy_wait_forever
    pw_unit_test
  GROUPS
    modules
    pw_memory
)

pw_add_library(pw_memory._sibling_cast INTERFACE
  HEADERS
    public/pw_memory/internal/sibling_cast.h
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_memory.sibling_cast_test
  SOURCES
    sibling_cast_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_memory._sibling_cast
  GROUPS
    modules
    pw_memory
)
