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
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert
    pw_bytes
  PRIVATE_DEPS
    pw_assert.check
  SOURCES
    chunk.cc
)
# LINT.ThenChange(../Android.bp, BUILD.bazel, CMakeLists.txt)

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
    pw_bytes
    pw_allocator.bump_allocator
    pw_allocator.testing
    pw_multibuf
  GROUPS
    modules
    pw_multibuf
)
# LINT.ThenChange(BUILD.bazel, BUILD.gn)
