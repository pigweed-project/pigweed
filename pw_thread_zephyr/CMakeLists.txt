# Copyright 2023 The Pigweed Authors
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

pw_add_module_config(pw_thread_zephyr_CONFIG)

pw_add_library(pw_thread_zephyr.config INTERFACE
  HEADERS
    public/pw_thread_zephyr/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_thread_zephyr_CONFIG}
)

# This target provides the backend for pw::Thread::id & pw::this_thread::get_id.
pw_add_library(pw_thread_zephyr.id INTERFACE
  HEADERS
    public/pw_thread_zephyr/id_inline.h
    public/pw_thread_zephyr/id_native.h
    id_public_overrides/pw_thread_backend/id_inline.h
    id_public_overrides/pw_thread_backend/id_native.h
  PUBLIC_INCLUDES
    public
    id_public_overrides
  PUBLIC_DEPS
    pw_assert
    pw_interrupt.context
    pw_thread.thread.facade
    pw_thread_zephyr.config
)
pw_zephyrize_libraries_ifdef(
  CONFIG_PIGWEED_THREAD
  pw_thread_zephyr.id
)

pw_add_library(pw_thread_zephyr.sleep STATIC
  HEADERS
    public/pw_thread_zephyr/sleep_inline.h
    sleep_public_overrides/pw_thread_backend/sleep_inline.h
  PUBLIC_INCLUDES
    public
    sleep_public_overrides
  PUBLIC_DEPS
    pw_chrono.system_clock
    pw_thread.sleep.facade
    pw_thread_zephyr.config
  PRIVATE_DEPS
    pw_assert.check
    pw_thread.thread
  SOURCES
    sleep.cc
)
pw_zephyrize_libraries_ifdef(
  CONFIG_PIGWEED_THREAD_SLEEP
  pw_thread_zephyr.sleep
)

# This target provides the backend for pw::Thread and the headers needed for
# thread creation.
pw_add_library(pw_thread_zephyr.thread STATIC
  HEADERS
    public/pw_thread_zephyr/context.h
    public/pw_thread_zephyr/options.h
    public/pw_thread_zephyr/thread_inline.h
    public/pw_thread_zephyr/thread_native.h
    thread_public_overrides/pw_thread_backend/thread_inline.h
    thread_public_overrides/pw_thread_backend/thread_native.h
  PUBLIC_INCLUDES
    public
    thread_public_overrides
  PUBLIC_DEPS
    pw_assert
    pw_function
    pw_span
    pw_string
    pw_thread.thread.facade
    pw_thread_zephyr.config
  SOURCES
    thread.cc
)
pw_zephyrize_libraries_ifdef(
  CONFIG_PIGWEED_THREAD
  pw_thread_zephyr.thread
)

# This target provides the backend for pw::this_thread::yield.
pw_add_library(pw_thread_zephyr.yield INTERFACE
  HEADERS
    public/pw_thread_zephyr/yield_inline.h
    yield_public_overrides/pw_thread_backend/yield_inline.h
  PUBLIC_INCLUDES
    public
    yield_public_overrides
  PUBLIC_DEPS
    pw_assert
    pw_thread.yield.facade
    pw_thread_zephyr.config
)
pw_zephyrize_libraries_ifdef(
    CONFIG_PIGWEED_THREAD_YIELD
    pw_thread_zephyr.yield
)

pw_add_library(pw_thread_zephyr.thread_creation INTERFACE
  HEADERS
    public/pw_thread_zephyr/stack.h
    thread_public_overrides/pw_thread_backend/stack.h
  PUBLIC_INCLUDES
    public
    thread_public_overrides
  PUBLIC_DEPS
    pw_thread_zephyr.config
)

pw_add_library(pw_thread_zephyr.thread_iteration STATIC
  PUBLIC_INCLUDES
    public
    thread_public_overrides
  PUBLIC_DEPS
    pw_thread.thread_info
    pw_thread.thread_iteration.facade
    pw_thread_zephyr.config
  SOURCES
    pw_thread_zephyr_private/thread_iteration.h
    thread_iteration.cc
)
pw_zephyrize_libraries_ifdef(
  CONFIG_PIGWEED_THREAD_ITERATION
  pw_thread_zephyr.thread_iteration
)
