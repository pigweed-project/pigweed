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

pw_add_library(pw_i2c_zephyr STATIC
  HEADERS
    public/pw_i2c_zephyr/initiator.h
  PUBLIC_INCLUDES
    public
  SOURCES
    initiator.cc
  PUBLIC_DEPS
    pw_i2c.initiator
    pw_status
    pw_sync.lock_annotations
    pw_sync.mutex
    zephyr_interface
)
if(CONFIG_PIGWEED_I2C)
  zephyr_append_cmake_library(pw_i2c_zephyr)
  # Include directory needs to be manually added. Typically
  # pw_zephyrize_libraries_ifdef is used, however, this creates a dependency
  # cycle. The cmake library above doesn't propagage the include directory
  # to its dependencies and only registers the library to be used in final
  # linking.
  zephyr_include_directories(public)
endif()

pw_add_library(pw_i2c_zephyr.responder STATIC
  HEADERS
    public/pw_i2c_zephyr/responder.h
  PUBLIC_INCLUDES
    public
  SOURCES
    responder.cc
  PUBLIC_DEPS
    # Pigweed dependencies
    pw_function          # For pw::Function
    pw_i2c.address       # For pw::i2c::Address
    pw_i2c.responder     # For the pw::i2c::Responder interface
    pw_log               # For logging
    pw_status            # For pw::Status
    zephyr_interface
  PRIVATE_DEPS
    pw_assert.check
)
if(CONFIG_PIGWEED_I2C_RESPONDER)
  zephyr_append_cmake_library(pw_i2c_zephyr.responder)
endif()

pw_add_library(pw_i2c_zephyr.responder_test STATIC
  HEADERS
    public/pw_i2c_backend/responder_test.h
  PUBLIC_INCLUDES
    public
  SOURCES
    responder_test.cc
  PUBLIC_DEPS
    pw_i2c.address
    pw_i2c.responder
    pw_i2c.responder_test_interface
    pw_i2c_zephyr.responder
    pw_status
    zephyr_interface
)
if("${pw_i2c.responder_test_BACKEND}" STREQUAL "pw_i2c_zephyr.responder_test")
  zephyr_append_cmake_library(pw_i2c_zephyr.responder_test)
endif()

pw_add_test(pw_i2c_zephyr.initiator_test
  SOURCES
    initiator_test.cc
  PRIVATE_DEPS
    pw_i2c_zephyr
    pw_function
    pw_unit_test
    zephyr_interface
  GROUPS
    modules
    pw_i2c_zephyr
)
