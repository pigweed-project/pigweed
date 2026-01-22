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
include($ENV{PW_ROOT}/pw_async2/backend.cmake)

pw_add_module_config(pw_async2_CONFIG)

pw_add_library(pw_async2.config INTERFACE
  HEADERS
    public/pw_async2/internal/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_async2_CONFIG}
)

pw_add_library(pw_async2.poll INTERFACE
  HEADERS
    public/pw_async2/poll.h
    public/pw_async2/internal/poll_internal.h
    public/pw_async2/try.h
  PUBLIC_DEPS
    pw_polyfill
    pw_string.to_string
    pw_third_party.fuchsia.stdcompat
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.poll_test
  SOURCES
    poll_test.cc
  PRIVATE_DEPS
    pw_async2.poll
    pw_async2.testing
)

pw_add_library(pw_async2 STATIC
  HEADERS
    public/pw_async2/context.h
    public/pw_async2/dispatcher.h
    public/pw_async2/future.h
    public/pw_async2/internal/lock.h
    public/pw_async2/internal/logging.h
    public/pw_async2/owned_task.h
    public/pw_async2/runnable_dispatcher.h
    public/pw_async2/task.h
    public/pw_async2/waker.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert.assert
    pw_async2.config
    pw_async2.poll
    pw_chrono.system_clock
    pw_containers.intrusive_forward_list
    pw_containers.intrusive_list
    pw_log
    pw_log.args
    pw_polyfill
    pw_memory
    pw_sync.interrupt_spin_lock
    pw_sync.lock_annotations
    pw_third_party.fuchsia.stdcompat
    pw_tokenizer
    pw_toolchain.no_destructor
  PRIVATE_DEPS
    pw_assert.check
    pw_thread.sleep
  SOURCES
    context.cc
    dispatcher.cc
    future.cc
    runnable_dispatcher.cc
    task.cc
    waker.cc
)

pw_add_library(pw_async2.dispatcher INTERFACE
  PUBLIC_DEPS
    pw_async2
)

pw_add_library(pw_async2.testing STATIC
  HEADERS
    public/pw_async2/dispatcher_for_test.h
  PUBLIC_DEPS
    pw_async2
    ${pw_async2.dispatcher_for_test_BACKEND}
  PUBLIC_INCLUDES
    public
  SOURCES
    dispatcher_for_test.cc
)

pw_add_test(pw_async2.dispatcher_test
  SOURCES
    dispatcher_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.testing
    pw_containers.vector
)

pw_add_library(pw_async2.basic_dispatcher INTERFACE
  HEADERS
    public/pw_async2/basic_dispatcher.h
  PUBLIC_DEPS
    pw_async2
    pw_sync.thread_notification
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_async2.notified_dispatcher INTERFACE
  HEADERS
    public/pw_async2/notified_dispatcher.h
  PUBLIC_DEPS
    pw_async2
    pw_sync.thread_notification
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_async2.basic_dispatcher_for_test INTERFACE
  HEADERS
    dispatcher_for_test_public_overrides/pw_async2_backend/native_dispatcher_for_test.h
  PUBLIC_DEPS
    pw_async2.basic_dispatcher
  PUBLIC_INCLUDES
    dispatcher_for_test_public_overrides
)

pw_add_library(pw_async2.epoll_dispatcher STATIC
  HEADERS
    public/pw_async2/epoll_dispatcher.h
  SOURCES
    epoll_dispatcher.cc
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_async2
    pw_async2.poll
    pw_status
  PRIVATE_DEPS
    pw_log
)

pw_add_library(pw_async2.epoll_dispatcher_for_test INTERFACE
  HEADERS
    epoll_dispatcher_for_test_public_overrides/pw_async2_backend/native_dispatcher_for_test.h
  PUBLIC_DEPS
    pw_async2.epoll_dispatcher
  PUBLIC_INCLUDES
    epoll_dispatcher_for_test_public_overrides
)

pw_add_library(pw_async2.pendable INTERFACE
  HEADERS
    public/pw_async2/pendable.h
  PUBLIC_DEPS
    pw_async2.poll
    pw_async2
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.pendable_test
  SOURCES
    pendable_test.cc
  PRIVATE_DEPS
    pw_async2.pendable
    pw_async2.testing
)

pw_add_test(pw_async2.future_test
  SOURCES
    future_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.pend_func_task
    pw_async2.poll
    pw_async2.select
    pw_async2.testing
    pw_compilation_testing._pigweed_only_negative_compilation
)

pw_add_library(pw_async2.future_timeout INTERFACE
  HEADERS
    public/pw_async2/future_timeout.h
  PUBLIC_DEPS
    pw_assert.assert
    pw_async2.channel
    pw_async2
    pw_async2.poll
    pw_async2.system_time_provider
    pw_async2.time_provider
    pw_async2.value_future
    pw_chrono.system_clock
    pw_function
    pw_preprocessor
    pw_status
    pw_third_party.fuchsia.stdcompat
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.future_timeout_test
  SOURCES
    future_timeout_test.cc
  PRIVATE_DEPS
    pw_async2.channel
    pw_async2
    pw_async2.future_timeout
    pw_async2.pend_func_task
    pw_async2.poll
    pw_async2.simulated_time_provider
    pw_async2.testing
    pw_async2.value_future
    pw_chrono.system_clock
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_function
    pw_result
    pw_status
)

pw_add_library(pw_async2.value_future INTERFACE
  HEADERS
    public/pw_async2/value_future.h
  PUBLIC_DEPS
    pw_async2
    pw_async2.poll
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.value_future_test
  SOURCES
    value_future_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.pend_func_task
    pw_async2.poll
    pw_async2.testing
    pw_async2.value_future
)

pw_add_test(pw_async2.value_future_thread_test
  SOURCES
    value_future_thread_test.cc
  PRIVATE_DEPS
    pw_async2.callback_task
    pw_async2.poll
    pw_async2.testing
    pw_async2.value_future
    pw_thread.sleep
    pw_thread.test_thread_context
    pw_thread.thread
)

pw_add_library(pw_async2.pend_func_task INTERFACE
  HEADERS
    public/pw_async2/pend_func_task.h
  PUBLIC_DEPS
    pw_async2
    pw_function
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.pend_func_task_test
  SOURCES
    pend_func_task_test.cc
  PRIVATE_DEPS
    pw_async2.testing
    pw_async2.pend_func_task
)

pw_add_library(pw_async2.callback_task INTERFACE
  HEADERS
    public/pw_async2/callback_task.h
  PUBLIC_DEPS
    pw_async2
    pw_function
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.callback_task_test
  SOURCES
    callback_task_test.cc
  PRIVATE_DEPS
    pw_async2.callback_task
    pw_async2.testing
    pw_async2.value_future
    pw_function
)

pw_add_library(pw_async2.waker_queue STATIC
  HEADERS
    public/pw_async2/waker_queue.h
  PUBLIC_DEPS
    pw_async2
    pw_containers.inline_queue
  PUBLIC_INCLUDES
    public
  SOURCES
    waker_queue.cc
  PRIVATE_DEPS
    pw_log
)

pw_add_test(pw_async2.waker_queue_test
  SOURCES
    waker_queue_test.cc
  PRIVATE_DEPS
    pw_async2.pend_func_task
    pw_async2.testing
    pw_async2.waker_queue
)

pw_add_library(pw_async2.allocate_task INTERFACE
  HEADERS
    public/pw_async2/allocate_task.h
  PUBLIC_DEPS
    pw_async2
    pw_allocator.allocator
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.allocate_task_test
  SOURCES
    allocate_task_test.cc
  PRIVATE_DEPS
    pw_async2.allocate_task
    pw_async2.testing
    pw_allocator.testing
)

pw_add_library(pw_async2.once_sender INTERFACE
  HEADERS
    public/pw_async2/once_sender.h
  PUBLIC_DEPS
    pw_async2
    pw_function
    pw_sync.mutex
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.once_sender_test
  SOURCES
    once_sender_test.cc
  PRIVATE_DEPS
    pw_async2.once_sender
    pw_async2.testing
    pw_containers.vector
)

if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  pw_add_library(pw_async2.coro STATIC
    HEADERS
      public/pw_async2/coro.h
    SOURCES
      coro.cc
    PRIVATE_DEPS
      pw_log
    PUBLIC_DEPS
      pw_allocator.allocator
      pw_async2
      pw_function
      pw_status
    PUBLIC_INCLUDES
      public
  )

  pw_add_test(pw_async2.coro_test
    SOURCES
      coro_test.cc
    PRIVATE_DEPS
      pw_async2.testing
      pw_allocator.null_allocator
      pw_allocator.testing
      pw_async2.coro
      pw_status
  )

  pw_add_library(pw_async2.coro_or_else_task INTERFACE
    HEADERS
      public/pw_async2/coro_or_else_task.h
    PUBLIC_DEPS
      pw_async2.coro
      pw_async2
      pw_function
      pw_status
    PUBLIC_INCLUDES
      public
  )

  pw_add_test(pw_async2.coro_or_else_task_test
    SOURCES
      coro_or_else_task_test.cc
    PRIVATE_DEPS
      pw_async2.coro
      pw_async2.coro_or_else_task
      pw_async2
      pw_allocator.null_allocator
      pw_allocator.testing
      pw_status
  )
endif()

pw_add_library(pw_async2.time_provider STATIC
  HEADERS
    public/pw_async2/time_provider.h
  SOURCES
    time_provider.cc
  PUBLIC_DEPS
    pw_async2
    pw_chrono.virtual_clock
    pw_async2.poll
    pw_containers.intrusive_list
    pw_function
    pw_memory
    pw_sync.interrupt_spin_lock
    pw_toolchain.no_destructor
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_async2.system_time_provider STATIC
  HEADERS
    public/pw_async2/system_time_provider.h
  SOURCES
    system_time_provider.cc
  PUBLIC_DEPS
    pw_chrono.system_clock
    pw_async2.time_provider
  PRIVATE_DEPS
    pw_chrono.system_timer
    pw_toolchain.no_destructor
  PUBLIC_INCLUDES
    public
)

if((NOT "${pw_chrono.system_clock_BACKEND}" STREQUAL "") AND
  (NOT "${pw_sync.interrupt_spin_lock_BACKEND}" STREQUAL ""))
  pw_add_test(pw_async2.system_time_provider_test
    SOURCES
      system_time_provider_test.cc
    PRIVATE_DEPS
      pw_async2.system_time_provider
      pw_async2.testing
      pw_chrono.system_clock
    GROUPS
      modules
      pw_async2
  )
endif()

pw_add_library(pw_async2.simulated_time_provider INTERFACE
  HEADERS
    public/pw_async2/simulated_time_provider.h
  PUBLIC_DEPS
    pw_async2.time_provider
    pw_sync.interrupt_spin_lock
)

if((NOT "${pw_chrono.system_clock_BACKEND}" STREQUAL "") AND
  (NOT "${pw_sync.interrupt_spin_lock_BACKEND}" STREQUAL ""))
  pw_add_test(pw_async2.simulated_time_provider_test
    SOURCES
      simulated_time_provider_test.cc
    PRIVATE_DEPS
      pw_async2.simulated_time_provider
      pw_async2.testing
      pw_chrono.system_clock
    GROUPS
      modules
      pw_async2
  )
endif()

pw_add_library(pw_async2.enqueue_heap_func INTERFACE
  HEADERS
    public/pw_async2/enqueue_heap_func.h
  PUBLIC_DEPS
    pw_async2
)

pw_add_test(pw_async2.enqueue_heap_func_test
  SOURCES
    enqueue_heap_func_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.enqueue_heap_func
    pw_async2.testing
  GROUPS
    modules
    pw_async2
)

pw_add_library(pw_async2.join INTERFACE
  HEADERS
    public/pw_async2/join.h
  PUBLIC_DEPS
    pw_async2
)

pw_add_test(pw_async2.join_test
  SOURCES
    join_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.join
    pw_async2.testing
    pw_async2.value_future
    pw_compilation_testing._pigweed_only_negative_compilation
  GROUPS
    modules
    pw_async2
)

pw_add_library(pw_async2.select INTERFACE
  HEADERS
    public/pw_async2/select.h
  PUBLIC_DEPS
    pw_async2
    pw_containers.optional_tuple
)

pw_add_test(pw_async2.select_test
  SOURCES
    select_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.pendable
    pw_async2.select
    pw_async2.testing
    pw_async2.value_future
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_async2
)

pw_add_library(pw_async2.cancellable_task INTERFACE
  HEADERS
    public/pw_async2/cancellable_task.h
  PUBLIC_DEPS
    pw_async2
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.cancellable_task_test
  SOURCES
    cancellable_task_test.cc
  PRIVATE_DEPS
    pw_async2.cancellable_task
    pw_async2
    pw_async2.pendable
    pw_async2.poll
    pw_async2.testing
    pw_function
)

pw_add_library(pw_async2.channel STATIC
  HEADERS
    public/pw_async2/channel.h
  SOURCES
    channel.cc
  PUBLIC_DEPS
    pw_assert
    pw_async2.callback_task
    pw_async2
    pw_async2.poll
    pw_allocator
    pw_containers.deque
    pw_numeric.checked_arithmetic
    pw_sync.interrupt_spin_lock
    pw_sync.timed_thread_notification
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_async2.channel_test
  SOURCES
    channel_test.cc
  PRIVATE_DEPS
    pw_async2.channel
    pw_async2
    pw_async2.pend_func_task
    pw_async2.poll
    pw_async2.testing
    pw_allocator.testing
    pw_containers.vector
)

pw_add_test(pw_async2.channel_thread_test
  SOURCES
    channel_thread_test.cc
  PRIVATE_DEPS
    pw_async2.channel
    pw_async2
    pw_async2.poll
    pw_async2.testing
    pw_containers.vector
    pw_thread.test_thread_context
    pw_thread.thread
)

if((NOT "${pw_unit_test_BACKEND}" STREQUAL "") AND
   (NOT "${pw_unit_test_BACKEND}" STREQUAL "pw_unit_test:light"))
pw_add_test(pw_async2.channel_typed_test
  SOURCES
    channel_typed_test.cc
  PRIVATE_DEPS
    pw_async2.channel
    pw_async2
    pw_async2.pend_func_task
    pw_async2.poll
    pw_async2.testing
    pw_allocator.testing
    pw_containers.vector
)
endif()

pw_add_test(pw_async2.task_test
  SOURCES
    task_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.testing
    pw_sync.binary_semaphore
    pw_sync.mutex
    pw_thread.sleep
    pw_thread.test_thread_context
    pw_thread.thread
)

pw_add_test(pw_async2.dispatcher_thread_test
  SOURCES
    dispatcher_thread_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.testing
    pw_sync.thread_notification
    pw_thread.sleep
    pw_thread.test_thread_context
    pw_thread.thread
)

if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  pw_add_test(pw_async2.channel_coro_test
    SOURCES
      channel_coro_test.cc
    PRIVATE_DEPS
      pw_async2.channel
      pw_async2.coro
      pw_async2.coro_or_else_task
      pw_async2
      pw_async2.poll
      pw_async2.testing
      pw_allocator.testing
      pw_containers.vector
  )
endif()
