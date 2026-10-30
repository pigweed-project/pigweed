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

if(NOT CONFIG_ZEPHYR_PIGWEED_MODULE)
  return()
endif()

pw_add_library(pw_digital_io_zephyr STATIC
  HEADERS
    public/pw_digital_io_zephyr/digital_io.h
  PUBLIC_INCLUDES
    public
  SOURCES
    digital_io.cc
  PUBLIC_DEPS
    pw_assert
    pw_digital_io
    zephyr_interface
    pw_third_party.fuchsia.stdcompat
)

pw_add_test(pw_digital_io_zephyr.digital_io_test
  SOURCES
    digital_io_test.cc
  PRIVATE_DEPS
    pw_digital_io
    pw_digital_io_zephyr
    zephyr_interface
  GROUPS
    modules
    pw_digital_io_zephyr
)
