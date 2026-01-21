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

cmake_minimum_required(VERSION 3.20)
project(Pigweed)

# Do not rely on the PW_ROOT environment variable being set through bootstrap.
# Regardless of whether it's set or not the following include will ensure it is.
include(pw_build/pigweed.cmake)

if(CONFIG_ZEPHYR_PIGWEED_MODULE)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_ZEPHYR
      pw_assert.assert pw_assert_zephyr.assert pw_assert/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_ZEPHYR
      pw_assert.check pw_assert_zephyr.check pw_assert/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_TOKENIZED
      pw_assert.assert pw_assert_tokenized.assert_backend pw_assert/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_TOKENIZED
      pw_assert.check pw_assert_tokenized.check_backend pw_assert/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK
      pw_chrono.system_clock pw_chrono_zephyr.system_clock pw_chrono/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_CHRONO_SYSTEM_TIMER
      pw_chrono.system_timer pw_chrono_zephyr.system_timer pw_chrono/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_INTERRUPT_CONTEXT
      pw_interrupt.context pw_interrupt_zephyr.context pw_interrupt/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_STRING
      pw_log pw_log_string pw_log/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_ZEPHYR
      pw_log pw_log_zephyr pw_log/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED
      pw_log_tokenized.handler pw_log_zephyr.tokenized_handler pw_log_tokenized/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED
      pw_log pw_log_tokenized pw_log/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_HDLC_RPC_SERVER
    pw_system.rpc_server pw_system.hdlc_rpc_server  pw_system/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_LOG_BACKEND
    pw_log pw_system.log_backend pw_log/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_LOG_BACKEND
    pw_log_tokenized.handler pw_system.log_backend pw_log_tokenized/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_TARGET_HOOKS
    pw_system.target_hooks pw_system.zephyr_target_hooks  pw_system/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD_ITERATION
      pw_thread.thread_iteration pw_thread_zephyr.thread_iteration pw_thread/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD_SLEEP
      pw_thread.sleep pw_thread_zephyr.sleep pw_thread/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD_YIELD
      pw_thread.yield pw_thread_zephyr.yield pw_thread/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD
      pw_thread.id pw_thread_zephyr.id pw_thread/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD
      pw_thread.thread pw_thread_zephyr.thread pw_thread/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD
      pw_thread.thread_creation pw_thread_zephyr.thread_creation pw_thread/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_BINARY_SEMAPHORE
      pw_sync.binary_semaphore pw_sync_zephyr.binary_semaphore_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_COUNTING_SEMAPHORE
      pw_sync.counting_semaphore pw_sync_zephyr.counting_semaphore_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_INTERRUPT_SPIN_LOCK
      pw_sync.interrupt_spin_lock pw_sync_zephyr.interrupt_spin_lock_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_THREAD_NOTIFICATION
    pw_sync.thread_notification pw_sync_zephyr.thread_notification_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_TIMED_THREAD_NOTIFICATION
      pw_sync.timed_thread_notification pw_sync_zephyr.timed_thread_notification_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_MUTEX
      pw_sync.mutex pw_sync_zephyr.mutex_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_TIMED_MUTEX
      pw_sync.timed_mutex pw_sync_zephyr.timed_mutex_backend pw_sync/backend.cmake)
  pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYS_IO
      pw_sys_io pw_sys_io_zephyr pw_sys_io/backend.cmake)

  if(CONFIG_NANOPB AND "${dir_pw_third_party_nanopb}" STREQUAL "")
    set(dir_pw_third_party_nanopb "${ZEPHYR_NANOPB_MODULE_DIR}" CACHE PATH "" FORCE)
  endif()
endif()

add_subdirectory(pw_alignment EXCLUDE_FROM_ALL)
add_subdirectory(pw_allocator EXCLUDE_FROM_ALL)
add_subdirectory(pw_assert EXCLUDE_FROM_ALL)
add_subdirectory(pw_assert_basic EXCLUDE_FROM_ALL)
add_subdirectory(pw_assert_fuchsia EXCLUDE_FROM_ALL)
add_subdirectory(pw_assert_log EXCLUDE_FROM_ALL)
add_subdirectory(pw_assert_tokenized EXCLUDE_FROM_ALL)
add_subdirectory(pw_assert_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_async EXCLUDE_FROM_ALL)
add_subdirectory(pw_async_basic EXCLUDE_FROM_ALL)
add_subdirectory(pw_async2 EXCLUDE_FROM_ALL)
add_subdirectory(pw_async_fuchsia EXCLUDE_FROM_ALL)
add_subdirectory(pw_atomic EXCLUDE_FROM_ALL)
add_subdirectory(pw_base64 EXCLUDE_FROM_ALL)
add_subdirectory(pw_blob_store EXCLUDE_FROM_ALL)
add_subdirectory(pw_bluetooth EXCLUDE_FROM_ALL)
add_subdirectory(pw_bluetooth_proxy EXCLUDE_FROM_ALL)
add_subdirectory(pw_build EXCLUDE_FROM_ALL)
add_subdirectory(pw_build_info EXCLUDE_FROM_ALL)
add_subdirectory(pw_bytes EXCLUDE_FROM_ALL)
add_subdirectory(pw_channel EXCLUDE_FROM_ALL)
add_subdirectory(pw_checksum EXCLUDE_FROM_ALL)
add_subdirectory(pw_chrono EXCLUDE_FROM_ALL)
add_subdirectory(pw_chrono_freertos EXCLUDE_FROM_ALL)
add_subdirectory(pw_chrono_stl EXCLUDE_FROM_ALL)
add_subdirectory(pw_chrono_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_compilation_testing EXCLUDE_FROM_ALL)
add_subdirectory(pw_containers EXCLUDE_FROM_ALL)
add_subdirectory(pw_cpu_exception EXCLUDE_FROM_ALL)
add_subdirectory(pw_cpu_exception_cortex_m EXCLUDE_FROM_ALL)
add_subdirectory(pw_digital_io EXCLUDE_FROM_ALL)
add_subdirectory(pw_digital_io_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_display EXCLUDE_FROM_ALL)
add_subdirectory(pw_elf EXCLUDE_FROM_ALL)
add_subdirectory(pw_file EXCLUDE_FROM_ALL)
add_subdirectory(pw_flatbuffers EXCLUDE_FROM_ALL)
add_subdirectory(pw_function EXCLUDE_FROM_ALL)
add_subdirectory(pw_fuzzer EXCLUDE_FROM_ALL)
add_subdirectory(pw_hex_dump EXCLUDE_FROM_ALL)
add_subdirectory(pw_hdlc EXCLUDE_FROM_ALL)
add_subdirectory(pw_i2c EXCLUDE_FROM_ALL)
add_subdirectory(pw_i2c_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_interrupt EXCLUDE_FROM_ALL)
add_subdirectory(pw_interrupt_cortex_m EXCLUDE_FROM_ALL)
add_subdirectory(pw_interrupt_freertos EXCLUDE_FROM_ALL)
add_subdirectory(pw_interrupt_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_intrusive_ptr EXCLUDE_FROM_ALL)
add_subdirectory(pw_json EXCLUDE_FROM_ALL)
add_subdirectory(pw_kvs EXCLUDE_FROM_ALL)
add_subdirectory(pw_libc EXCLUDE_FROM_ALL)
add_subdirectory(pw_libcxx EXCLUDE_FROM_ALL)
add_subdirectory(pw_log EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_basic EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_fuchsia EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_null EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_string EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_tokenized EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_rpc EXCLUDE_FROM_ALL)
add_subdirectory(pw_log_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_malloc EXCLUDE_FROM_ALL)
add_subdirectory(pw_memory EXCLUDE_FROM_ALL)
add_subdirectory(pw_metric EXCLUDE_FROM_ALL)
add_subdirectory(pw_multibuf EXCLUDE_FROM_ALL)
add_subdirectory(pw_multisink EXCLUDE_FROM_ALL)
add_subdirectory(pw_numeric EXCLUDE_FROM_ALL)
add_subdirectory(pw_persistent_ram EXCLUDE_FROM_ALL)
add_subdirectory(pw_perf_test EXCLUDE_FROM_ALL)
add_subdirectory(pw_polyfill EXCLUDE_FROM_ALL)
add_subdirectory(pw_preprocessor EXCLUDE_FROM_ALL)
add_subdirectory(pw_protobuf EXCLUDE_FROM_ALL)
add_subdirectory(pw_protobuf_compiler EXCLUDE_FROM_ALL)
add_subdirectory(pw_random EXCLUDE_FROM_ALL)
add_subdirectory(pw_random_fuchsia EXCLUDE_FROM_ALL)
add_subdirectory(pw_result EXCLUDE_FROM_ALL)
add_subdirectory(pw_ring_buffer EXCLUDE_FROM_ALL)
add_subdirectory(pw_router EXCLUDE_FROM_ALL)
add_subdirectory(pw_rpc EXCLUDE_FROM_ALL)
add_subdirectory(pw_rpc_transport EXCLUDE_FROM_ALL)
add_subdirectory(pw_sensor EXCLUDE_FROM_ALL)
add_subdirectory(pw_snapshot EXCLUDE_FROM_ALL)
add_subdirectory(pw_span EXCLUDE_FROM_ALL)
add_subdirectory(pw_spi EXCLUDE_FROM_ALL)
add_subdirectory(pw_status EXCLUDE_FROM_ALL)
add_subdirectory(pw_stream EXCLUDE_FROM_ALL)
add_subdirectory(pw_string EXCLUDE_FROM_ALL)
add_subdirectory(pw_sync EXCLUDE_FROM_ALL)
add_subdirectory(pw_sync_baremetal EXCLUDE_FROM_ALL)
add_subdirectory(pw_sync_freertos EXCLUDE_FROM_ALL)
add_subdirectory(pw_sync_stl EXCLUDE_FROM_ALL)
add_subdirectory(pw_sync_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_sys_io EXCLUDE_FROM_ALL)
add_subdirectory(pw_sys_io_stdio EXCLUDE_FROM_ALL)
add_subdirectory(pw_sys_io_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_system EXCLUDE_FROM_ALL)
add_subdirectory(pw_tls_client EXCLUDE_FROM_ALL)
add_subdirectory(pw_thread EXCLUDE_FROM_ALL)
add_subdirectory(pw_thread_freertos EXCLUDE_FROM_ALL)
add_subdirectory(pw_thread_stl EXCLUDE_FROM_ALL)
add_subdirectory(pw_thread_zephyr EXCLUDE_FROM_ALL)
add_subdirectory(pw_tokenizer EXCLUDE_FROM_ALL)
add_subdirectory(pw_toolchain EXCLUDE_FROM_ALL)
add_subdirectory(pw_trace EXCLUDE_FROM_ALL)
add_subdirectory(pw_trace_tokenized EXCLUDE_FROM_ALL)
add_subdirectory(pw_transfer EXCLUDE_FROM_ALL)
add_subdirectory(pw_uart EXCLUDE_FROM_ALL)
add_subdirectory(pw_uuid EXCLUDE_FROM_ALL)
add_subdirectory(pw_unit_test EXCLUDE_FROM_ALL)
add_subdirectory(pw_varint EXCLUDE_FROM_ALL)
add_subdirectory(pw_work_queue EXCLUDE_FROM_ALL)

add_subdirectory(third_party/abseil-cpp EXCLUDE_FROM_ALL)
add_subdirectory(third_party/boringssl EXCLUDE_FROM_ALL)
add_subdirectory(third_party/emboss EXCLUDE_FROM_ALL)
add_subdirectory(third_party/flatbuffers EXCLUDE_FROM_ALL)
add_subdirectory(third_party/freertos EXCLUDE_FROM_ALL)
add_subdirectory(third_party/fuchsia EXCLUDE_FROM_ALL)
add_subdirectory(third_party/fuzztest EXCLUDE_FROM_ALL)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
add_subdirectory(third_party/nanopb EXCLUDE_FROM_ALL)

add_custom_target(pw_apps)

# pw_system example apps
add_subdirectory(targets/host_device_simulator EXCLUDE_FROM_ALL)
add_dependencies(pw_apps pw_system.system_example)

if(CONFIG_ZEPHYR_PIGWEED_MODULE)
  add_subdirectory(zephyr)
else()
  add_subdirectory(targets/host EXCLUDE_FROM_ALL)

  add_dependencies(pw_apps pw_hdlc.rpc_example)
endif()
