# Copyright 2021 The Pigweed Authors
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

pw_add_library(pw_assert_zephyr.check INTERFACE
  HEADERS
    public/pw_assert_zephyr/check_zephyr.h
    public_overrides/pw_assert_backend/check_backend.h
  PUBLIC_INCLUDES
    public
    public_overrides
  PUBLIC_DEPS
    pw_preprocessor
)
pw_zephyrize_libraries_ifdef(
  CONFIG_PIGWEED_ASSERT_ZEPHYR pw_assert_zephyr.check)

pw_add_library(pw_assert_zephyr.assert INTERFACE
  HEADERS
    public/pw_assert_zephyr/assert_zephyr.h
    public_overrides/pw_assert_backend/assert_backend.h
  PUBLIC_INCLUDES
    public
    public_overrides
)
pw_zephyrize_libraries_ifdef(
  CONFIG_PIGWEED_ASSERT_ZEPHYR pw_assert_zephyr.assert)

pw_add_library(pw_assert_zephyr.pigweed_overrides INTERFACE
  HEADERS
    zephyr_public_overrides/zephyr_custom_assert.h
  PUBLIC_INCLUDES
    zephyr_public_overrides
  PUBLIC_DEPS
    pw_assert
)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_ASSERT_PIGWEED_OVERRIDES
    pw_assert_zephyr.pigweed_overrides
)
