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

# LINT.IfChange
pw_add_library(pw_multibuf INTERFACE
  HEADERS
    public/pw_multibuf/multibuf.h
    public/pw_multibuf/observer.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.chunk
    pw_multibuf.config
    pw_multibuf.v1.multibuf
    pw_multibuf.v2.multibuf
)

### Module configuration

pw_add_module_config(pw_multibuf_CONFIG)

pw_add_library(pw_multibuf.config INTERFACE
  HEADERS
    public/pw_multibuf/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_multibuf_CONFIG}
)

### V1 Libraries

pw_add_library(pw_multibuf.chunk INTERFACE
  HEADERS
    public/pw_multibuf/chunk.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.config
    pw_multibuf.v1.chunk
)

pw_add_library(pw_multibuf.header_chunk_region_tracker INTERFACE
  HEADERS
    public/pw_multibuf/header_chunk_region_tracker.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.chunk
    pw_multibuf.config
    pw_multibuf.v1.header_chunk_region_tracker
)

pw_add_library(pw_multibuf.single_chunk_region_tracker INTERFACE
  HEADERS
    public/pw_multibuf/single_chunk_region_tracker.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.chunk
    pw_multibuf.config
    pw_multibuf.v1.single_chunk_region_tracker
)

pw_add_library(pw_multibuf.allocator INTERFACE
  HEADERS
    public/pw_multibuf/allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf
    pw_multibuf.config
    pw_multibuf.v1.allocator
)

pw_add_library(pw_multibuf.allocator_async INTERFACE
  HEADERS
    public/pw_multibuf/allocator_async.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.allocator
    pw_multibuf.config
    pw_multibuf.v1.allocator_async
)

pw_add_library(pw_multibuf.from_span INTERFACE
  HEADERS
    public/pw_multibuf/from_span.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf
    pw_multibuf.config
    pw_multibuf.v1.from_span
)

pw_add_library(pw_multibuf.simple_allocator INTERFACE
  HEADERS
    public/pw_multibuf/simple_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf
    pw_multibuf.allocator
    pw_multibuf.config
    pw_multibuf.v1.simple_allocator
)

pw_add_library(pw_multibuf.testing INTERFACE
  HEADERS
    public/pw_multibuf/simple_allocator_for_test.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_multibuf.config
    pw_multibuf.simple_allocator
    pw_multibuf.v1.testing
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)

add_subdirectory(v1)
add_subdirectory(v2)
