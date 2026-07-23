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

if(NOT CONFIG_ZEPHYR_PIGWEED_MODULE)
  return()
endif()

pw_add_library(pw_chrono_zephyr.system_clock INTERFACE
  HEADERS
    public/pw_chrono_zephyr/system_clock_constants.h
    public/pw_chrono_zephyr/system_clock_config.h
    public/pw_chrono_zephyr/system_clock_inline.h
    public_overrides/system_clock/pw_chrono_backend/system_clock_config.h
    public_overrides/system_clock/pw_chrono_backend/system_clock_inline.h
  PUBLIC_INCLUDES
    public
    public_overrides/system_clock
  PUBLIC_DEPS
    pw_function
    pw_chrono.epoch
    pw_chrono.system_clock.facade
    zephyr_interface
)
if(CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK)
  zephyr_link_libraries(pw_chrono_zephyr.system_clock)
endif()

pw_add_library(pw_chrono_zephyr.system_timer STATIC
  HEADERS
    public/pw_chrono_zephyr/system_timer_inline.h
    public/pw_chrono_zephyr/system_timer_native.h
    public_overrides/system_timer/pw_chrono_backend/system_timer_inline.h
    public_overrides/system_timer/pw_chrono_backend/system_timer_native.h
  SOURCES
    system_timer.cc
  PUBLIC_INCLUDES
    public
    public_overrides/system_timer
  PUBLIC_DEPS
    pw_chrono.system_timer.facade
    zephyr_interface
)
if(CONFIG_PIGWEED_CHRONO_SYSTEM_TIMER)
  zephyr_link_libraries(pw_chrono_zephyr.system_timer)
endif()
