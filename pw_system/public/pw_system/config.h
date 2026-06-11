// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

// PW_SYSTEM_LOG_BUFFER_SIZE is the log buffer size which determines how many
// log entries can be buffered prior to streaming them.
//
// Defaults to 4KiB.
#ifndef PW_SYSTEM_LOG_BUFFER_SIZE
#define PW_SYSTEM_LOG_BUFFER_SIZE 4096
#endif  // PW_SYSTEM_LOG_BUFFER_SIZE

// PW_SYSTEM_MAX_LOG_ENTRY_SIZE limits the proto-encoded log entry size. This
// value might depend on a target interface's MTU.
//
// Defaults to 256B.
#ifndef PW_SYSTEM_MAX_LOG_ENTRY_SIZE
#define PW_SYSTEM_MAX_LOG_ENTRY_SIZE 256
#endif  // PW_SYSTEM_MAX_LOG_ENTRY_SIZE

// PW_SYSTEM_MAX_TRANSMISSION_UNIT target's MTU.
//
// Defaults to 1055 bytes, which is enough to fit 512-byte payloads when using
// HDLC framing.
#ifndef PW_SYSTEM_MAX_TRANSMISSION_UNIT
#define PW_SYSTEM_MAX_TRANSMISSION_UNIT 1055
#endif  // PW_SYSTEM_MAX_TRANSMISSION_UNIT

// PW_SYSTEM_DEFAULT_CHANNEL_ID RPC channel ID to host.
//
// Defaults to 1.
#ifndef PW_SYSTEM_DEFAULT_CHANNEL_ID
#define PW_SYSTEM_DEFAULT_CHANNEL_ID 1
#endif  // PW_SYSTEM_DEFAULT_CHANNEL_ID

// PW_SYSTEM_LOGGING_CHANNEL_ID logging RPC channel ID to host. If this is
// different from PW_SYSTEM_DEFAULT_CHANNEL_ID, then
// PW_SYSTEM_LOGGING_RPC_HDLC_ADDRESS must also be different from
// PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS.
//
// Defaults to PW_SYSTEM_DEFAULT_CHANNEL_ID.
#ifndef PW_SYSTEM_LOGGING_CHANNEL_ID
#define PW_SYSTEM_LOGGING_CHANNEL_ID PW_SYSTEM_DEFAULT_CHANNEL_ID
#endif  // PW_SYSTEM_LOGGING_CHANNEL_ID

// PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS RPC HDLC default address.
//
// Defaults to 82.
#ifndef PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS
#define PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS 82
#endif  // PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS

// PW_SYSTEM_LOGGING_RPC_HDLC_ADDRESS RPC HDLC logging address.
//
// Defaults to PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS.
#ifndef PW_SYSTEM_LOGGING_RPC_HDLC_ADDRESS
#define PW_SYSTEM_LOGGING_RPC_HDLC_ADDRESS PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS
#endif  // PW_SYSTEM_LOGGING_RPC_HDLC_ADDRESS

// PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID extra logging channel ID.
// If this is different from PW_SYSTEM_LOGGING_CHANNEL_ID, then
// an additional sink will be created to forward logs to
// this channel.
//
// Defaults to PW_SYSTEM_LOGGING_CHANNEL_ID
#ifndef PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID
#define PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID PW_SYSTEM_LOGGING_CHANNEL_ID
#endif  // PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID

// PW_SYSTEM_EXTRA_LOGGING_MAX_BUNDLE_PER_TRICKLE
// Defines the maximum number of log entries allowed in a single trickle.
// This is necessary because the extra logging channel may have size
// constraints.
#ifndef PW_SYSTEM_EXTRA_LOGGING_MAX_BUNDLE_PER_TRICKLE
#define PW_SYSTEM_EXTRA_LOGGING_MAX_BUNDLE_PER_TRICKLE \
  std::numeric_limits<size_t>::max()
#endif  // PW_SYSTEM_EXTRA_LOGGING_MAX_BUNDLE_PER_TRICKLE

// PW_SYSTEM_ENABLE_TRACE_SERVICE specifies if the trace RPC service is enabled.
//
// Defaults to 1.
#ifndef PW_SYSTEM_ENABLE_TRACE_SERVICE
#define PW_SYSTEM_ENABLE_TRACE_SERVICE 1
#endif  // PW_SYSTEM_ENABLE_TRACE_SERVICE

// PW_SYSTEM_ENABLE_TRANSFER_SERVICE specifies if the transfer RPC service is
// enabled.
//
// Defaults to 1.
#ifndef PW_SYSTEM_ENABLE_TRANSFER_SERVICE
#define PW_SYSTEM_ENABLE_TRANSFER_SERVICE 1
#endif  // PW_SYSTEM_ENABLE_TRANSFER_SERVICE

// PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE specifies if the thread snapshot
// RPC service is enabled.
//
// Defaults to 1.
#ifndef PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE
#define PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE 1
#endif  // PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE

// PW_SYSTEM_WORK_QUEUE_MAX_ENTRIES specifies the maximum number of work queue
// entries that may be staged at once.
//
// Defaults to 32.
#ifndef PW_SYSTEM_WORK_QUEUE_MAX_ENTRIES
#define PW_SYSTEM_WORK_QUEUE_MAX_ENTRIES 32
#endif  // PW_SYSTEM_WORK_QUEUE_MAX_ENTRIES

// PW_SYSTEM_SOCKET_IO_PORT specifies the port number to use for the socket
// stream implementation of pw_system's I/O interface.
//
// Defaults to 33000.
#ifndef PW_SYSTEM_SOCKET_IO_PORT
#define PW_SYSTEM_SOCKET_IO_PORT 33000
#endif  // PW_SYSTEM_SOCKET_IO_PORT

// PW_SYSTEM_ASYNC_LOG_THREAD_STACK_SIZE_BYTES specifies the size of the
// internal pw_log thread stack.
#ifndef PW_SYSTEM_ASYNC_LOG_THREAD_STACK_SIZE_BYTES
#define PW_SYSTEM_ASYNC_LOG_THREAD_STACK_SIZE_BYTES 4096
#endif  // PW_SYSTEM_ASYNC_LOG_THREAD_STACK_SIZE_BYTES

// PW_SYSTEM_ASYNC_RPC_THREAD_STACK_SIZE_BYTES specifies the size of the
// internal pw_rpc thread stack.
#ifndef PW_SYSTEM_ASYNC_RPC_THREAD_STACK_SIZE_BYTES
#define PW_SYSTEM_ASYNC_RPC_THREAD_STACK_SIZE_BYTES 2048
#endif  // PW_SYSTEM_ASYNC_RPC_THREAD_STACK_SIZE_BYTES

// PW_SYSTEM_ASYNC_TRANSFER_THREAD_STACK_SIZE_BYTES specifies the size of the
// internal pw_transfer thread stack.
#ifndef PW_SYSTEM_ASYNC_TRANSFER_THREAD_STACK_SIZE_BYTES
#define PW_SYSTEM_ASYNC_TRANSFER_THREAD_STACK_SIZE_BYTES 2048
#endif  // PW_SYSTEM_ASYNC_TRANSFER_THREAD_STACK_SIZE_BYTES

// PW_SYSTEM_ASYNC_DISPATCHER_THREAD_STACK_SIZE_BYTES specifies the size of the
// internal pw_async2 dispatcher thread stack.
#ifndef PW_SYSTEM_ASYNC_DISPATCHER_THREAD_STACK_SIZE_BYTES
#define PW_SYSTEM_ASYNC_DISPATCHER_THREAD_STACK_SIZE_BYTES 2048
#endif  // PW_SYSTEM_ASYNC_DISPATCHER_THREAD_STACK_SIZE_BYTES

// PW_SYSTEM_ASYNC_WORK_QUEUE_THREAD_STACK_SIZE_BYTES specifies the size of the
// internal work queue thread stack.
#ifndef PW_SYSTEM_ASYNC_WORK_QUEUE_THREAD_STACK_SIZE_BYTES
#define PW_SYSTEM_ASYNC_WORK_QUEUE_THREAD_STACK_SIZE_BYTES 2048
#endif  // PW_SYSTEM_ASYNC_WORK_QUEUE_THREAD_STACK_SIZE_BYTES

// PW_SYSTEM_ENABLE_CRASH_HANDLER specifies if the crash handler is enabled.
//
// Defaults to 1.
#ifndef PW_SYSTEM_ENABLE_CRASH_HANDLER
#define PW_SYSTEM_ENABLE_CRASH_HANDLER 1
#endif  // PW_SYSTEM_ENABLE_CRASH_HANDLER

// PW_SYSTEM_CRASH_SNAPSHOT_NOINIT_MEMORY_SECTION specifies the section of
// memory to store the snapshot data.
//
// Defaults to ".noinit"
#ifndef PW_SYSTEM_CRASH_SNAPSHOT_NOINIT_MEMORY_SECTION
#define PW_SYSTEM_CRASH_SNAPSHOT_NOINIT_MEMORY_SECTION ".noinit"
#endif  // PW_SYSTEM_CRASH_SNAPSHOT_NOINIT_MEMORY_SECTION

// PW_SYSTEM_CRASH_SNAPSHOT_MEMORY_SIZE_BYTES specifies how much memory  to
// reserver for snapshots.
//
// Defaults to 2048
#ifndef PW_SYSTEM_CRASH_SNAPSHOT_MEMORY_SIZE_BYTES
#define PW_SYSTEM_CRASH_SNAPSHOT_MEMORY_SIZE_BYTES 2048
#endif  // PW_SYSTEM_CRASH_SNAPSHOT_MEMORY_SIZE_BYTES

// PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE specifies if a default RPC benchmark
// service is added to the system server.
//
// Defaults to 0 (disabled).
#ifndef PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE
#define PW_SYSTEM_ENABLE_RPC_BENCHMARK_SERVICE 0
#endif

// PW_SYSTEM_ALLOCATOR_HEAP_SIZE_BYTES specifies how much memory to reserve for
// dynamic allocation via the system allocator.
//
// Defaults to 8192
#ifndef PW_SYSTEM_ALLOCATOR_HEAP_SIZE_BYTES
#define PW_SYSTEM_ALLOCATOR_HEAP_SIZE_BYTES 8192
#endif

namespace pw::system {

// This is the default channel used by the pw_system RPC server. Some other
// parts of pw_system use this channel ID as the default destination for
// unrequested data streams.
inline constexpr uint32_t kDefaultRpcChannelId = PW_SYSTEM_DEFAULT_CHANNEL_ID;

// This is the channel ID used for logging.
inline constexpr uint32_t kLoggingRpcChannelId = PW_SYSTEM_LOGGING_CHANNEL_ID;
#if PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID != PW_SYSTEM_LOGGING_CHANNEL_ID
inline constexpr uint32_t kExtraLoggingRpcChannelId =
    PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID;
#endif  // PW_SYSTEM_EXTRA_LOGGING_CHANNEL_ID != PW_SYSTEM_LOGGING_CHANNEL_ID

inline constexpr size_t kLogThreadStackSizeBytes =
    PW_SYSTEM_ASYNC_LOG_THREAD_STACK_SIZE_BYTES;
inline constexpr size_t kRpcThreadStackSizeBytes =
    PW_SYSTEM_ASYNC_RPC_THREAD_STACK_SIZE_BYTES;
inline constexpr size_t kTransferThreadStackSizeBytes =
    PW_SYSTEM_ASYNC_TRANSFER_THREAD_STACK_SIZE_BYTES;
inline constexpr size_t kDispatcherThreadStackSizeBytes =
    PW_SYSTEM_ASYNC_DISPATCHER_THREAD_STACK_SIZE_BYTES;
inline constexpr size_t kWorkQueueThreadStackSizeBytes =
    PW_SYSTEM_ASYNC_WORK_QUEUE_THREAD_STACK_SIZE_BYTES;

#undef PW_SYSTEM_ASYNC_LOG_THREAD_STACK_SIZE_BYTES
#undef PW_SYSTEM_ASYNC_RPC_THREAD_STACK_SIZE_BYTES
#undef PW_SYSTEM_ASYNC_TRANSFER_THREAD_STACK_SIZE_BYTES
#undef PW_SYSTEM_ASYNC_DISPATCHER_THREAD_STACK_SIZE_BYTES
#undef PW_SYSTEM_ASYNC_WORK_QUEUE_THREAD_STACK_SIZE_BYTES

}  // namespace pw::system
