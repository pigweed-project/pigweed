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

# This file is responsible for mapping Kconfig backend selections to the
# appropriate Pigweed backend targets. It is included by the top-level
# CMakeLists.txt when CONFIG_ZEPHYR_PIGWEED_MODULE is set.

# pw_assert.*
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_ZEPHYR
    pw_assert.assert pw_assert_zephyr.assert ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_assert/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_ZEPHYR
    pw_assert.check pw_assert_zephyr.check ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_assert/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_TOKENIZED
    pw_assert.assert pw_assert_tokenized.assert_backend ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_assert/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_ASSERT_TOKENIZED
    pw_assert.check pw_assert_tokenized.check_backend ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_assert/backend.cmake)

# pw_chrono.*
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_CHRONO_SYSTEM_CLOCK
    pw_chrono.system_clock pw_chrono_zephyr.system_clock ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_chrono/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_CHRONO_SYSTEM_TIMER
    pw_chrono.system_timer pw_chrono_zephyr.system_timer ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_chrono/backend.cmake)

# pw_interrupt.context
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_INTERRUPT_CONTEXT
    pw_interrupt.context pw_interrupt_zephyr.context ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_interrupt/backend.cmake)

# pw_log
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_STRING
    pw_log pw_log_string ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_log/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_ZEPHYR
    pw_log pw_log_zephyr ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_log/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED_HANDLER_ZEPHYR
    pw_log_tokenized.handler pw_log_zephyr.tokenized_handler ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_log_tokenized/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED
    pw_log pw_log_tokenized ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_log/backend.cmake)
# pw_system.log_backend isn't a pw_log backend: pw_log uses pw_log_tokenized,
# and the tokenized handler below routes messages to pw_system.
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_LOG_BACKEND
  pw_log pw_log_tokenized ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_log/backend.cmake)
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED_HANDLER_SYSTEM
  pw_log_tokenized.handler pw_system.log_backend ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_log_tokenized/backend.cmake)

# pw_system.rpc_server
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_HDLC_RPC_SERVER
  pw_system.rpc_server pw_system.hdlc_rpc_server
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_system/backend.cmake)

# pw_system.target_hooks
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYSTEM_TARGET_HOOKS
  pw_system.target_hooks pw_system.zephyr_target_hooks
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_system/backend.cmake)

# pw_thread.thread_iteration
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD_ITERATION
  pw_thread.thread_iteration pw_thread_zephyr.thread_iteration
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_thread/backend.cmake)

# pw_thread.sleep
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD_SLEEP
  pw_thread.sleep pw_thread_zephyr.sleep
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_thread/backend.cmake)

# pw_thread.yield
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD_YIELD
  pw_thread.yield pw_thread_zephyr.yield
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_thread/backend.cmake)

# pw_thread.id
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD
  pw_thread.id pw_thread_zephyr.id
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_thread/backend.cmake)

# pw_thread.thread
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD
  pw_thread.thread pw_thread_zephyr.thread
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_thread/backend.cmake)

# pw_thread.thread_creation
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_THREAD
  pw_thread.thread_creation pw_thread_zephyr.thread_creation
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_thread/backend.cmake)

# pw_sync.binary_semaphore
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_BINARY_SEMAPHORE
  pw_sync.binary_semaphore pw_sync_zephyr.binary_semaphore_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sync.counting_semaphore
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_COUNTING_SEMAPHORE
  pw_sync.counting_semaphore pw_sync_zephyr.counting_semaphore_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sync.interrupt_spin_lock
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_INTERRUPT_SPIN_LOCK
  pw_sync.interrupt_spin_lock pw_sync_zephyr.interrupt_spin_lock_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sync.thread_notification
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_THREAD_NOTIFICATION
  pw_sync.thread_notification pw_sync_zephyr.thread_notification_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sync.timed_thread_notification
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_TIMED_THREAD_NOTIFICATION
  pw_sync.timed_thread_notification pw_sync_zephyr.timed_thread_notification_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sync.mutex
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_MUTEX
  pw_sync.mutex pw_sync_zephyr.mutex_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sync.timed_mutex
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYNC_TIMED_MUTEX
  pw_sync.timed_mutex pw_sync_zephyr.timed_mutex_backend
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sync/backend.cmake)

# pw_sys_io
pw_set_zephyr_backend_ifdef(CONFIG_PIGWEED_SYS_IO
  pw_sys_io pw_sys_io_zephyr
  ${ZEPHYR_PIGWEED_MODULE_DIR}/pw_sys_io/backend.cmake)
