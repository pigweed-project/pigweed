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

pw_add_library(pw_hex_dump STATIC
  SOURCES
    hex_dump.cc
  HEADERS
    public/pw_hex_dump/hex_dump.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_span
    pw_status
    pw_string.builder
    pw_string.to_string
)

pw_add_library(pw_hex_dump.log_bytes STATIC
  HEADERS
    public/pw_hex_dump/log_bytes.h
  SOURCES
    log_bytes.cc
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_hex_dump
    pw_log
    pw_preprocessor
)
