# Copyright 2022 The Pigweed Authors
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
include($ENV{PW_ROOT}/pw_system/backend.cmake)
include($ENV{PW_ROOT}/pw_protobuf_compiler/proto.cmake)

# WARNING: Many of the dependencies in this file are missing and need to be
# added/updated. This is provided as a starting point, but currently does not
# work.
if("${pw_cpu_exception.entry_BACKEND}" STREQUAL "")
  pw_add_library(pw_system.config INTERFACE
    HEADERS
      public/pw_system/config.h
    PUBLIC_INCLUDES
      public
    PUBLIC_DEFINES
      PW_SYSTEM_ENABLE_CRASH_HANDLER=0
  )
else()
  pw_add_library(pw_system.config INTERFACE
    HEADERS
      public/pw_system/config.h
    PUBLIC_INCLUDES
      public
  )
endif()

pw_add_library(pw_system.log STATIC
  PUBLIC_DEPS
    pw_log_rpc.log_service
    pw_log_rpc.rpc_log_drain_thread
    pw_multisink
  PRIVATE_DEPS
    pw_system.config
    pw_system.rpc_server
    pw_log_rpc.rpc_log_drain
    pw_sync.lock_annotations
    pw_sync.mutex
  HEADERS
    public/pw_system/log.h
  PUBLIC_INCLUDES
    public
  SOURCES
    log.cc
)

pw_add_library(pw_system.log_backend STATIC
  PRIVATE_DEPS
    pw_system.config
    pw_system.log
    pw_bytes
    pw_chrono.system_clock
    pw_log.facade
    pw_log.proto_utils
    pw_log_string.handler.facade
    pw_log_tokenized.handler
    pw_log_tokenized.metadata
    pw_multisink
    pw_result
    pw_sync.interrupt_spin_lock
    pw_sync.lock_annotations
    pw_tokenizer
  SOURCES
    log_backend.cc
)

pw_add_facade(pw_system.rpc_server INTERFACE
  BACKEND
    pw_system.rpc_server_BACKEND
  HEADERS
    public/pw_system/rpc_server.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_rpc.server
    pw_system.config
    pw_thread.thread_core
)

pw_add_library(pw_system.hdlc_rpc_server STATIC
  PRIVATE_DEPS
    pw_assert
    pw_hdlc.default_addresses
    pw_hdlc.rpc_channel_output
    pw_sync.mutex
    pw_system.config
    pw_system.io
    pw_system.rpc_server.facade
    pw_system.sys_io_target_io
    pw_thread.thread_core
    pw_trace
    pw_log
  SOURCES
    hdlc_rpc_server.cc
)

pw_add_library(pw_system.thread_snapshot_service STATIC
  HEADERS
    public/pw_system/thread_snapshot_service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_thread.thread_snapshot_service
  SOURCES
    thread_snapshot_service.cc
)

pw_add_library(pw_system.transfer_handlers STATIC
  HEADERS
    public/pw_system/transfer_handlers.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_persistent_ram
    pw_system.config
    pw_trace_tokenized.config
    pw_transfer
    pw_transfer.proto.pwpb
  SOURCES
    transfer_handlers.cc
)

pw_add_library(pw_system.file_manager STATIC
  HEADERS
    public/pw_system/file_manager.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_system.config
    pw_system.transfer_handlers
    pw_persistent_ram.flat_file_system_entry
  PRIVATE_DEPS
    pw_system.trace_service
  SOURCES
    file_manager.cc
)

pw_add_library(pw_system.transfer_service STATIC
  HEADERS
    public/pw_system/transfer_service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_transfer
  PRIVATE_DEPS
    pw_system.file_manager
  SOURCES
    transfer_service.cc
)

pw_add_library(pw_system.benchmark_service STATIC
  HEADERS
    public/pw_system/benchmark_service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_rpc.benchmark
  SOURCES
    benchmark_service.cc
)


pw_add_library(pw_system.file_service STATIC
  HEADERS
    public/pw_system/file_service.h
  PUBLIC_INCLUDES
    public
  PRIVATE_DEPS
    pw_system.file_manager
  SOURCES
    file_service.cc
)

pw_add_library(pw_system.trace_service STATIC
  HEADERS
    public/pw_system/trace_service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_system.transfer_handlers
  PRIVATE_DEPS
    pw_persistent_ram
    pw_trace_tokenized.trace_service_pwpb
  SOURCES
    trace_service.cc
)

pw_add_facade(pw_system.io INTERFACE
  BACKEND
    pw_system.io_BACKEND
  HEADERS
    public/pw_system/io.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_stream
)

pw_add_facade(pw_system.device_handler INTERFACE
  BACKEND
    pw_system.device_handler_BACKEND
  HEADERS
    public/pw_system/device_handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_snapshot.snapshot_proto.pwpb
)

pw_add_library(pw_system.unknown_device_handler STATIC
  SOURCES
    unknown_device_handler.cc
  PRIVATE_DEPS
    pw_system.device_handler.facade
)

pw_proto_library(pw_system.device_service_proto
  SOURCES
    pw_system_protos/device_service.proto
  INPUTS
    pw_system_protos/device_service.options
)

pw_add_library(pw_system.device_service_pwpb STATIC
  HEADERS
    public/pw_system/device_service_pwpb.h
  PUBLIC_INCLUDES
    public
  SOURCES
    device_service_pwpb.cc
  PUBLIC_DEPS
    pw_system.device_service_proto.pwpb_rpc
  PRIVATE_DEPS
    pw_system.device_handler
)

pw_add_library(pw_system.device_service STATIC
  HEADERS
    public/pw_system/device_service.h
  PUBLIC_INCLUDES
    public
  SOURCES
    device_service.cc
  PRIVATE_DEPS
    pw_system.device_service_pwpb
)

pw_add_library(pw_system.init STATIC
  HEADERS
    public/pw_system/init.h
  PUBLIC_INCLUDES
    public
  SOURCES
    init.cc
  PRIVATE_DEPS
    pw_system.benchmark_service
    pw_system.device_service
    pw_system.file_manager
    pw_system.file_service
    pw_system.log
    pw_system.rpc_server
    pw_system.target_hooks.facade
    pw_system.thread_snapshot_service
    pw_system.trace_service
    pw_system.transfer_service
    pw_system.work_queue
    pw_metric.global
    pw_metric.metric_service_pwpb
    pw_rpc.pwpb.echo_service
    pw_thread.thread
    pw_trace
)

pw_add_library(pw_system.work_queue STATIC
  HEADERS
    public/pw_system/work_queue.h
  PUBLIC_INCLUDES
    public
  SOURCES
    work_queue.cc
  PUBLIC_DEPS
    pw_work_queue
  PRIVATE_DEPS
    pw_system.config
)

pw_add_library(pw_system.sys_io_target_io STATIC
  PRIVATE_DEPS
    pw_system.io.facade
    pw_stream
    pw_stream.sys_io_stream
  SOURCES
    sys_io_target_io.cc
)

pw_add_library(pw_system.socket_target_io STATIC
  SOURCES
    socket_target_io.cc
  PRIVATE_DEPS
    pw_assert
    pw_stream
    pw_stream.socket_stream
    pw_system.config
    pw_system.io.facade
)

pw_add_facade(pw_system.target_hooks INTERFACE
  BACKEND
    pw_system.target_hooks_BACKEND
  HEADERS
    public/pw_system/target_hooks.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_thread.thread
)

pw_add_library(pw_system.stl_target_hooks STATIC
  PRIVATE_DEPS
    pw_system.config
    pw_thread.sleep
    pw_thread.thread
    pw_thread_stl.thread
  SOURCES
    stl_target_hooks.cc
)

pw_add_library(pw_system.freertos_target_hooks STATIC
  SOURCES
    freertos_target_hooks.cc
  PRIVATE_DEPS
    pw_system.config
    pw_thread.thread
    pw_thread_freertos.thread

  # TODO: b/234876414 - This should depend on FreeRTOS but our third parties
  # currently do not have CMake support.
)

if(CONFIG_ZEPHYR_PIGWEED_MODULE)
  pw_add_library(pw_system.zephyr_target_hooks STATIC
    SOURCES
      zephyr_target_hooks.cc
    PRIVATE_DEPS
      pw_system.target_hooks.facade
      pw_thread.thread
      pw_thread_zephyr.thread
  )
endif()
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_SYSTEM_TARGET_HOOKS
  pw_system.zephyr_target_hooks
)

add_executable(pw_system.system_example EXCLUDE_FROM_ALL
  example_user_app_init.cc
)
target_link_libraries(pw_system.system_example
  PRIVATE
    pw_system.init
    pw_system.io
    pw_system.log
    pw_system.rpc_server
    pw_system.work_queue
    pw_log
    pw_thread.sleep
    pw_trace
    pw_rpc.pwpb.echo_service
    pw_system.target_hooks
    targets.host_device_simulator.boot
)

pw_add_library(pw_system.async STATIC
  HEADERS
    public/pw_system/system.h
  PUBLIC_INCLUDES
    public
  SOURCES
    pw_system_private/threads.h
    system.cc
    threads.cc
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_async2
    pw_channel
    pw_rpc.server
  PRIVATE_DEPS
    pw_system.async_packet_io
    pw_system.device_service
    pw_system.file_manager
    pw_system.file_service
    pw_system.log
    pw_system.thread_snapshot_service
    pw_system.transfer_service
    pw_system.work_queue
    pw_allocator.best_fit_block_allocator
    pw_allocator.synchronized_allocator
    pw_async2.allocate_task
    pw_async2.basic_dispatcher
    pw_async2.func_task
    pw_hdlc.router
    pw_rpc.pwpb.echo_service
    pw_sync.interrupt_spin_lock
    pw_thread.thread
)

pw_add_library(pw_system.async_packet_io STATIC
  HEADERS
    public/pw_system/internal/async_packet_io.h
  PUBLIC_INCLUDES
    public
  SOURCES
    async_packet_io.cc
  PUBLIC_DEPS
    pw_async2
    pw_channel.forwarding_channel
    pw_containers.inline_var_len_entry_queue
    pw_hdlc.router
    pw_multibuf.simple_allocator
    pw_rpc.server
    pw_sync.lock_annotations
    pw_sync.mutex
    pw_sync.thread_notification
    pw_thread.thread
    pw_thread.thread_core
    pw_allocator.allocator
    pw_channel
    pw_multibuf
  PRIVATE_DEPS
    pw_system.config
    pw_assert
    pw_log
)

if(NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
  # TODO: b/317922402 - Run on Windows when thread detaching is supported.
  pw_add_test(pw_system.async_packet_io_test
    SOURCES
      async_packet_io_test.cc
    PRIVATE_DEPS
      pw_system.async_packet_io
      pw_allocator.testing
      pw_channel.loopback_channel
      pw_multibuf.testing
    GROUPS
      modules
      pw_system
  )

  pw_add_test(pw_system.system_async_test
    SOURCES
      system_async_test.cc
    PRIVATE_DEPS
      pw_system.async
      pw_allocator.testing
      pw_channel.loopback_channel
      pw_multibuf.simple_allocator
    GROUPS
      modules
      pw_system
  )
endif()
