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
pw_add_library(pw_multibuf.v1_adapter.chunk STATIC
  HEADERS
    public/pw_multibuf/v1_adapter/chunk.h
    public/pw_multibuf/v1_adapter/internal/chunk_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert.assert
    pw_bytes
    pw_bytes.alignment
    pw_containers.dynamic_vector
    pw_containers.storage
    pw_numeric.checked_arithmetic
    pw_preprocessor
    pw_result
  PRIVATE_DEPS
    pw_assert.check
  SOURCES
    chunk.cc
    chunk_allocator.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1_adapter.single_chunk_region_tracker INTERFACE
  HEADERS
    public/pw_multibuf/v1_adapter/single_chunk_region_tracker.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_allocator.block.tiny_block
    pw_allocator.first_fit
    pw_bytes
    pw_containers.storage
    pw_multibuf.v1_adapter.chunk
    pw_multibuf.v2.iterators
)

pw_add_test(pw_multibuf.v1_adapter.single_chunk_region_tracker_test
  SOURCES
    single_chunk_region_tracker_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
    PW_MULTIBUF_INCLUDE_V1_ADAPTERS=1
  PRIVATE_DEPS
    pw_multibuf.chunk
    pw_multibuf.single_chunk_region_tracker
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1_adapter.multibuf STATIC
  HEADERS
    public/pw_multibuf/v1_adapter/multibuf.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_bytes
    pw_multibuf.v1_adapter.chunk
    pw_multibuf.v2.iterators
    pw_multibuf.v2.multibuf
    pw_status
  SOURCES
    multibuf.cc
  PRIVATE_DEPS
    pw_assert.check
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1_adapter.multibuf_v1_test
  SOURCES
    multibuf_v1_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
    PW_MULTIBUF_INCLUDE_V1_ADAPTERS=1
  PRIVATE_DEPS
    pw_allocator
    pw_allocator.bump_allocator
    pw_allocator.testing
    pw_assert.check
    pw_bytes
    pw_multibuf
    pw_status
  GROUPS
    modules
    pw_multibuf
)

pw_add_library(pw_multibuf.v1_adapter.allocator INTERFACE
  HEADERS
    public/pw_multibuf/v1_adapter/allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf
    pw_multibuf.config
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1_adapter.simple_allocator STATIC
  HEADERS
    public/pw_multibuf/v1_adapter/simple_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_allocator.best_fit
    pw_allocator.synchronized_allocator
    pw_allocator.block.small_alignable_block
    pw_bytes
    pw_bytes.alignment
    pw_multibuf.v1_adapter.allocator
    pw_multibuf.v1_adapter.chunk
    pw_multibuf.v1_adapter.multibuf
    pw_sync.lock_annotations
    pw_sync.mutex
  SOURCES
    simple_allocator.cc
  PRIVATE_DEPS
    pw_assert.check
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1_adapter.simple_allocator_test
  SOURCES
    simple_allocator_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
    PW_MULTIBUF_INCLUDE_V1_ADAPTERS=1
  PRIVATE_DEPS
    pw_allocator.null_allocator
    pw_allocator.testing
    pw_containers.storage
    pw_containers.vector
    pw_multibuf.chunk
    pw_multibuf.simple_allocator
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1_adapter.from_span STATIC
  HEADERS
    public/pw_multibuf/v1_adapter/from_span.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_bytes
    pw_function
    pw_multibuf.config
    pw_multibuf.v1_adapter.multibuf
  SOURCES
    from_span.cc
  PRIVATE_DEPS
    pw_multibuf.v1_adapter.chunk
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1_adapter.from_span_test
  SOURCES
    from_span_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=2
    PW_MULTIBUF_INCLUDE_V1_ADAPTERS=1
  PRIVATE_DEPS
    pw_allocator.testing
    pw_multibuf.from_span
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)
