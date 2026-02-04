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

### Libraries and tests

# LINT.IfChange

pw_add_library(pw_multibuf.v2.properties INTERFACE
  HEADERS
    public/pw_multibuf/v2/properties.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_multibuf.v2.iterators INTERFACE
  HEADERS
    public/pw_multibuf/v2/chunks.h
    public/pw_multibuf/v2/internal/byte_iterator.h
    public/pw_multibuf/v2/internal/chunk_iterator.h
    public/pw_multibuf/v2/internal/entry.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert.assert
    pw_bytes
    pw_containers.dynamic_deque
    pw_preprocessor
)

pw_add_test(pw_multibuf.v2.chunk_iterator_test
  SOURCES
    chunk_iterator_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
  PRIVATE_DEPS
    pw_multibuf.v2.iterators
    pw_multibuf.v2._iterator_testing
  GROUPS
    modules
    pw_multibuf
)

pw_add_test(pw_multibuf.v2.byte_iterator_test
  SOURCES
    byte_iterator_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
  PRIVATE_DEPS
    pw_multibuf.v2.iterators
    pw_multibuf.v2._iterator_testing
  GROUPS
    modules
    pw_multibuf
)

pw_add_library(pw_multibuf.v2._iterator_testing INTERFACE
  HEADERS
    pw_multibuf_private/iterator_testing.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.bump_allocator
    pw_multibuf.v2.iterators
    pw_unit_test
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v2.multibuf STATIC
  HEADERS
    public/pw_multibuf/v2/multibuf.h
    public/pw_multibuf/v2/observer.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_allocator.null_allocator
    pw_bytes
    pw_containers.algorithm
    pw_containers.dynamic_deque
    pw_multibuf.v2.iterators
    pw_multibuf.v2.properties
  SOURCES
    generic_multibuf.cc
  PRIVATE_DEPS
    pw_assert.check
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v2.multibuf_test
  SOURCES
    multibuf_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
  PRIVATE_DEPS
    pw_allocator.chunk_pool
    pw_allocator.testing
    pw_assert.check
    pw_bytes
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_multibuf
    pw_multibuf.v2.iterators
    pw_result
    pw_status
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)
