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
pw_add_library(pw_multibuf.v1.chunk STATIC
  HEADERS
    public/pw_multibuf/v1/chunk.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_bytes
    pw_multibuf.config
    pw_preprocessor
    pw_span
    pw_sync.mutex
  PRIVATE_DEPS
    pw_assert.check
  SOURCES
    chunk.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1.chunk_test
  SOURCES
    chunk_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_allocator.testing
    pw_multibuf.v1.chunk
    pw_multibuf.v1.header_chunk_region_tracker
  GROUPS
    modules
    pw_multibuf
)

pw_add_library(pw_multibuf.v1.header_chunk_region_tracker INTERFACE
  HEADERS
    public/pw_multibuf/v1/header_chunk_region_tracker.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_bytes
    pw_multibuf.config
    pw_multibuf.v1.chunk
)

pw_add_test(pw_multibuf.v1.header_chunk_region_tracker_test
  SOURCES
    header_chunk_region_tracker_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_allocator.testing
    pw_multibuf.v1.chunk
    pw_multibuf.v1.header_chunk_region_tracker
    pw_status
  GROUPS
    modules
    pw_multibuf
)

pw_add_library(pw_multibuf.v1.single_chunk_region_tracker INTERFACE
  HEADERS
    public/pw_multibuf/v1/single_chunk_region_tracker.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_bytes
    pw_multibuf.config
    pw_multibuf.v1.chunk
)

pw_add_test(pw_multibuf.v1.single_chunk_region_tracker_test
  SOURCES
    single_chunk_region_tracker_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_multibuf.v1.chunk
    pw_multibuf.v1.single_chunk_region_tracker
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1.multibuf STATIC
  HEADERS
    public/pw_multibuf/v1/multibuf.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.config
    pw_multibuf.v1.chunk
    pw_preprocessor
  SOURCES
    multibuf.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1.multibuf_test
  SOURCES
    multibuf_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_assert.check
    pw_bytes
    pw_multibuf
    pw_multibuf.v1._internal_test_utils
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1.allocator STATIC
  HEADERS
    public/pw_multibuf/v1/allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.intrusive_forward_list
    pw_multibuf.config
    pw_multibuf.v1.multibuf
    pw_result
    pw_sync.interrupt_spin_lock
  SOURCES
    allocator.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1.allocator_async STATIC
  HEADERS
    public/pw_multibuf/v1/allocator_async.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_async2
    pw_multibuf.config
    pw_multibuf.v1.allocator
  SOURCES
    allocator_async.cc
)

# TODO: https://pwbug.dev/384583239 - Split async into its own test.
pw_add_test(pw_multibuf.v1.allocator_test
  SOURCES
    allocator_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_async2
    pw_async2.testing
    pw_multibuf.v1.allocator
    pw_multibuf.v1.allocator_async
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1.simple_allocator STATIC
  HEADERS
    public/pw_multibuf/v1/simple_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_containers.intrusive_list
    pw_multibuf.config
    pw_multibuf.v1.allocator
    pw_multibuf.v1.multibuf
  SOURCES
    simple_allocator.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1.simple_allocator_test
  SOURCES
    simple_allocator_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_allocator.testing
    pw_allocator.null_allocator
    pw_multibuf.v1.simple_allocator
  GROUPS
    modules
    pw_multibuf
)

pw_add_library(pw_multibuf.v1.stream STATIC
  HEADERS
    public/pw_multibuf/v1/stream.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.config
    pw_multibuf.v1.multibuf
    pw_stream
  SOURCES
    stream.cc
)

pw_add_test(pw_multibuf.v1.stream_test
  SOURCES
    stream_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_allocator.testing
    pw_bytes
    pw_multibuf
    pw_multibuf.v1._internal_test_utils
    pw_multibuf.v1.stream
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_multibuf.v1.from_span STATIC
  HEADERS
    public/pw_multibuf/v1/from_span.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_multibuf.v1.multibuf
    pw_function
  SOURCES
    from_span.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, BUILD.gn)

# LINT.IfChange
pw_add_test(pw_multibuf.v1.from_span_test
  SOURCES
    from_span_test.cc
  PRIVATE_DEFINES
    PW_MULTIBUF_VERSION=1
  PRIVATE_DEPS
    pw_allocator.testing
    pw_multibuf.from_span
  GROUPS
    modules
    pw_multibuf
)

pw_add_library(pw_multibuf.v1.testing INTERFACE
  HEADERS
    public/pw_multibuf/v1/simple_allocator_for_test.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_allocator.synchronized_allocator
    pw_allocator.testing
    pw_multibuf.config
    pw_multibuf.v1.simple_allocator
    pw_sync.mutex
)

pw_add_library(pw_multibuf.v1._internal_test_utils INTERFACE
  HEADERS
    pw_multibuf_private/test_utils.h
  PUBLIC_DEPS
    pw_assert
    pw_allocator.testing
    pw_bytes
    pw_multibuf.config
    pw_multibuf.v1.chunk
    pw_multibuf.v1.header_chunk_region_tracker
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)
