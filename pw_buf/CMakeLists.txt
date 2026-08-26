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

pw_add_library(pw_buf STATIC
  HEADERS
    public/pw_buf/buf.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert
    pw_bytes
    pw_containers.iterator
    pw_span
  PRIVATE_DEPS
    pw_assert.check
  SOURCES
    buf.cc
)

pw_add_test(pw_buf.buf_test
  SOURCES
    buf_test.cc
  PRIVATE_DEPS
    pw_allocator
    pw_allocator.null_allocator
    pw_allocator.testing
    pw_assert
    pw_buf
    pw_bytes
  GROUPS
    modules
    pw_buf
)
