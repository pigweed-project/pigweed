# Copyright 2020 The Pigweed Authors
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

pw_add_library(pw_checksum STATIC
  HEADERS
    public/pw_checksum/crc16_ccitt.h
    public/pw_checksum/crc32.h
    public/pw_checksum/crc8.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_checksum._config
    pw_span
  SOURCES
    crc16_ccitt.cc
    crc32.cc
)

# TODO: b/284002266 - Unresolved linker error when using pw_checksum above.
# Created this crc32 library and linker error is resolved.
pw_add_library(pw_checksum.crc32 STATIC
  HEADERS
    public/pw_checksum/crc32.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_checksum._config
    pw_span
  SOURCES
    crc32.cc
)

pw_add_library(pw_checksum._config INTERFACE
  HEADERS
    public/pw_checksum/internal/config.h
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_checksum.crc16_ccitt_test
  SOURCES
    crc16_ccitt_test.cc
    crc16_ccitt_test_c.c
  PRIVATE_DEPS
    pw_checksum
    pw_random
  GROUPS
    modules
    pw_checksum
)

pw_add_test(pw_checksum.crc32_test
  SOURCES
    crc32_test.cc
    crc32_test_c.c
  PRIVATE_DEPS
    pw_checksum
    pw_random
  GROUPS
    modules
    pw_checksum
)

pw_add_test(pw_checksum.crc8_test
  SOURCES
    crc8_test.cc
  PRIVATE_DEPS
    pw_checksum
  GROUPS
    modules
    pw_checksum
)
