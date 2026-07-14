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

if(NOT CONFIG_ZEPHYR_PIGWEED_MODULE AND NOT CONFIG_PIGWEED_SYS_IO)
  return()
endif()

pw_add_module_config(pw_sys_io_zephyr_CONFIG)

pw_add_library(pw_sys_io_zephyr.config INTERFACE
  HEADERS
    public/pw_sys_io_zephyr/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_sys_io_zephyr_CONFIG}
)

pw_add_library(pw_sys_io_zephyr STATIC
  SOURCES
    sys_io.cc
  PUBLIC_DEPS
    pw_sys_io.facade
    pw_sys_io.default_putget_bytes
    pw_sys_io_zephyr.config
    zephyr_interface
)

pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_SYS_IO
  pw_sys_io.default_putget_bytes
  pw_sys_io_zephyr
)
