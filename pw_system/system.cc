// Copyright 2024 The Pigweed Authors
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

#define PW_LOG_MODULE_NAME "pw_system"

#include "pw_system/system.h"

#include <utility>

#include "pw_allocator/best_fit.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/allocate_task.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/func_task.h"
#include "pw_log/log.h"
#include "pw_rpc/echo_service_pwpb.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_system/config.h"
#include "pw_system/device_service.h"
#include "pw_system/file_service.h"
#include "pw_system/internal/async_packet_io.h"
#include "pw_system/log.h"
#include "pw_system/thread_snapshot_service.h"
#include "pw_system/transfer_service.h"
#include "pw_system/work_queue.h"
#include "pw_system_private/threads.h"
#include "pw_thread/detached_thread.h"

#if PW_SYSTEM_ENABLE_CRASH_HANDLER
#include "pw_system/crash_handler.h"
#include "pw_system/crash_snapshot.h"
#endif  // PW_SYSTEM_ENABLE_CRASH_HANDLER

namespace pw {

void SystemStart(channel::ByteReaderWriter& io_channel) {
  system::StartAndClobberTheStack(io_channel);
}

namespace system {
namespace {

using pw::allocator::BestFitAllocator;
using pw::allocator::SynchronizedAllocator;

// TODO: b/349654108 - Standardize component declaration and initialization.
alignas(internal::PacketIO) std::byte packet_io_[sizeof(internal::PacketIO)];

internal::PacketIO& InitializePacketIoGlobal(
    channel::ByteReaderWriter& io_channel) {
  static std::byte buffer[256];
  internal::PacketIO* packet_io = new (packet_io_) internal::PacketIO(
      io_channel, buffer, System().allocator(), System().rpc_server());
  return *packet_io;
}

// Functions for quickly creating a task from a function. This functions could
// be moved to `pw::system::AsyncCore`.
template <typename Func>
[[nodiscard]] bool PostTaskFunction(Func&& func) {
  async2::Task* task = async2::AllocateTask<async2::PendFuncTask<Func>>(
      System().allocator(), std::forward<Func>(func));
  if (task == nullptr) {
    return false;
  }
  System().dispatcher().Post(*task);
  return true;
}

template <typename Func>
void PostTaskFunctionOrCrash(Func&& func) {
  PW_CHECK(PostTaskFunction(std::forward<Func>(func)));
}

async2::RunnableDispatcher& runnable_dispatcher() {
  static async2::BasicDispatcher dispatcher;
  return dispatcher;
}

}  // namespace

async2::Dispatcher& AsyncCore::dispatcher() { return runnable_dispatcher(); }

Allocator& AsyncCore::allocator() {
  alignas(
      uintptr_t) static std::byte buffer[PW_SYSTEM_ALLOCATOR_HEAP_SIZE_BYTES];
  static BestFitAllocator<> block_allocator(buffer);
  static SynchronizedAllocator<::pw::sync::InterruptSpinLock> sync_allocator(
      block_allocator);
  return sync_allocator;
}

bool AsyncCore::RunOnce(Function<void()>&& function) {
  return GetWorkQueue().PushWork(std::move(function)).ok();
}

void AsyncCore::Init(channel::ByteReaderWriter& io_channel) {
#if PW_SYSTEM_ENABLE_CRASH_HANDLER
  RegisterCrashHandler();
#endif  // PW_SYSTEM_ENABLE_CRASH_HANDLER

  PW_LOG_INFO("Initializing pw_system");

#if PW_SYSTEM_ENABLE_CRASH_HANDLER
  if (HasCrashSnapshot()) {
    PW_LOG_ERROR("==========================");
    PW_LOG_ERROR("======CRASH DETECTED======");
    PW_LOG_ERROR("==========================");
    PW_LOG_ERROR("Crash snapshots available.");
    PW_LOG_ERROR(
        "Run `device.get_crash_snapshots()` to download and clear the "
        "snapshots.");
  } else {
    PW_LOG_DEBUG("No crash snapshot");
  }
#endif  // PW_SYSTEM_ENABLE_CRASH_HANDLER

  PostTaskFunctionOrCrash(InitTask);

  // Initialize the packet_io subsystem
  internal::PacketIO& packet_io = InitializePacketIoGlobal(io_channel);

  PW_CONSTINIT static ThreadContextFor<kRpcThread> rpc_thread;
  packet_io.Start(System().dispatcher(), GetThreadOptions(rpc_thread));

  PW_CONSTINIT static ThreadContextFor<kDispatcherThread> dispatcher_thread;
  Thread(dispatcher_thread, [] {
    runnable_dispatcher().RunForever();
  }).detach();

  PW_CONSTINIT static ThreadContextFor<kWorkQueueThread> work_queue_thread;
  Thread(work_queue_thread, GetWorkQueue()).detach();
}

async2::Poll<> AsyncCore::InitTask(async2::Context&) {
  PW_LOG_INFO("Initializing pw_system");

  const Status status = GetLogThread().OpenUnrequestedLogStream(
      kLoggingRpcChannelId, System().rpc_server(), GetLogService());
  if (!status.ok()) {
    PW_LOG_ERROR("Error opening unrequested log streams %d",
                 static_cast<int>(status.code()));
  }

  System().rpc_server().RegisterService(GetLogService());

  PW_CONSTINIT static ThreadContextFor<kLogThread> log_thread;
  Thread(log_thread, GetLogThread()).detach();

  static rpc::EchoService echo_service;
  System().rpc_server().RegisterService(echo_service);

  RegisterDeviceService(System().rpc_server());

  if (PW_SYSTEM_ENABLE_THREAD_SNAPSHOT_SERVICE != 0) {
    RegisterThreadSnapshotService(System().rpc_server());
  }

  if (PW_SYSTEM_ENABLE_TRANSFER_SERVICE != 0) {
    RegisterTransferService(System().rpc_server());
    RegisterFileService(System().rpc_server());

    PW_CONSTINIT static ThreadContextFor<kTransferThread> transfer_thread;
    Thread(transfer_thread, GetTransferThread()).detach();
    InitTransferService();
  }

  PW_LOG_INFO("pw_system initialization complete");
  return async2::Ready();
}

void StartAndClobberTheStack(channel::ByteReaderWriter& io_channel) {
  System().Init(io_channel);

  StartSchedulerAndClobberTheStack();
}

}  // namespace system
}  // namespace pw
