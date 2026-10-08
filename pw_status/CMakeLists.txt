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
include($ENV{PW_ROOT}/pw_enum/pw_cc_enum_library.cmake)

pw_add_module_config(pw_status_CONFIG)

pw_add_library(pw_status.config INTERFACE
  HEADERS
    public/pw_status/internal/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_status_CONFIG}
)

pw_add_library(pw_status STATIC
  HEADERS
    public/pw_status/status.h
    public/pw_status/status_base.h
    public/pw_status/status_with_size.h
    public/pw_status/try.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_enum
    pw_status.config
  SOURCES
    status.cc
)

# Use this for pw_status_CONFIG to require pw::Status objects to be used.
pw_add_library(pw_status.check_if_used INTERFACE
  PUBLIC_DEFINES
    PW_STATUS_CFG_CHECK_IF_USED=1
)

pw_add_test(pw_status.status_test
  SOURCES
    status_test.cc
    status_test_c.c
  PRIVATE_DEPS
    pw_enum
    pw_status
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_status
)

pw_add_enum_library(pw_status.status_test_enum
  ENUM_HEADERS
    test_enums/status_test_enum.h
  PUBLIC_INCLUDES
    test_enums
)

pw_add_test(pw_status.status_base_test
  SOURCES
    status_base_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_status
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_status
)

pw_add_test(pw_status.status_base_enum_test
  SOURCES
    status_base_enum_test.cc
  PRIVATE_DEPS
    pw_status
    pw_status.status_test_enum
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_status
)

pw_add_test(pw_status.status_with_size_test
  SOURCES
    status_with_size_test.cc
  PRIVATE_DEPS
    pw_status
  GROUPS
    modules
    pw_status
)

pw_add_test(pw_status.try_test
  SOURCES
    try_test.cc
  PRIVATE_DEPS
    pw_status
  GROUPS
    modules
    pw_status
)
