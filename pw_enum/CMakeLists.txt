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
include($ENV{PW_ROOT}/pw_enum/pw_cc_enum.cmake)

pw_add_library(pw_enum INTERFACE
  HEADERS
    public/pw_enum/to_string.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_enum._generate_internal_do_not_use INTERFACE
  HEADERS
    public_generate/pw_enum/generate.h
  PUBLIC_INCLUDES
    public_generate
  PUBLIC_DEPS
    pw_preprocessor
)

pw_add_test(pw_enum.to_string_test
  SOURCES
    to_string_test.cc
  PRIVATE_DEPS
    pw_enum
  GROUPS
    modules
    pw_enum
)

pw_cc_enum(pw_enum.base_enum
  HEADERS
    private/pw_enum_private/base_enum.h
  PUBLIC_INCLUDES
    private
)

# DOCSTAG: [pw_enum-basic-cmake]
pw_cc_enum(pw_enum.basic_enum
  HEADERS
    private/pw_enum_private/basic_enum.h
  PUBLIC_INCLUDES
    private
  PUBLIC_DEPS
    pw_enum.base_enum
)
# DOCSTAG: [pw_enum-basic-cmake]

pw_cc_enum(pw_enum._complex_enum
  HEADERS
    private/pw_enum_private/complex_enum.h
  PUBLIC_INCLUDES
    private
)

pw_cc_enum(pw_enum._standalone_enum
  HEADERS
    private/pw_enum_private/standalone_enum.h
  PUBLIC_INCLUDES
    private
)

pw_add_library(pw_enum._test_dep_lib INTERFACE
  HEADERS
    private/pw_enum_private/test_dep_lib.h
  PUBLIC_INCLUDES
    private
)

pw_cc_enum(pw_enum._enum_with_deps
  HEADERS
    private/pw_enum_private/enum_with_deps.h
  PUBLIC_INCLUDES
    private
  PUBLIC_DEPS
    pw_enum._test_dep_lib
)

pw_add_test(pw_enum.enum_test
  SOURCES
    enum_test.cc
  PRIVATE_DEPS
    pw_enum._complex_enum
    pw_enum.basic_enum
    pw_enum._standalone_enum
    pw_enum._enum_with_deps
    pw_enum
    pw_log
    pw_log.args
    pw_tokenizer
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_enum
)
