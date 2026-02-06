// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/channel.h"
#include "pw_async2/poll.h"
#include "pw_async2/runnable_dispatcher.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_containers/intrusive_map.h"
#include "pw_function/function.h"
#include "pw_thread/id.h"

namespace pw::bluetooth::proxy {

class L2capChannelManager;

namespace internal {

/// Implementation detail class for L2capChannelManager.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is true; that is, when
/// the proxy is using an synchronous execution model with a dispatcher that
/// runs concurrent tasks sequentially.
class L2capChannelManagerImpl {
 public:
  using L2capChannelMap = IntrusiveMap<uint32_t, L2capChannel::Handle>;
  using L2capChannelIterator = L2capChannelMap::iterator;

  L2capChannelManagerImpl(L2capChannelManager& manager, Allocator& allocator);

  constexpr Allocator& allocator() { return allocator_; }

  constexpr async2::Dispatcher& dispatcher() {
    PW_ASSERT(dispatcher_ != nullptr);
    return *dispatcher_;
  }

  constexpr const thread::Id& dispatcher_thread_id() {
    return dispatcher_thread_id_;
  }

  /// Sets the dispatcher, posts the drain task and records the dispatcher
  /// thread ID.
  Status Init(async2::Dispatcher& dispatcher);

  /// Creates a new sender for the credit MPSC channel. This can be used by
  /// external components to signal that new TX credits are available.
  async2::Sender<bool> CreateCreditSender();

  /// Returns true if the current thread is the dispatcher thread.
  [[nodiscard]] bool IsRunningOnDispatcherThread() const;

  /// Update iterators on channel registration.
  void OnRegister() PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex_);

  /// Update iterators on channel deregistration.
  void OnDeregister(const L2capChannel& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex_);

  /// Update iterators on channel deletion.
  void OnDeletion() PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex_);

  /// See L2capChannelManager::ReportNewTxPacketsOrCredits
  void ReportNewTxPacketsOrCredits();

  /// See L2capChannelManager::DrainChannelQueuesIfNewTx
  void DrainChannelQueuesIfNewTx();

  /// Handle deregistering all channels.
  ///
  /// Unlike the sync channel manager, the async channel manager can be
  /// destroyed at any time and the `async2::Channel`s will handle the
  /// disconnection. As a result, this method is trivial.
  void FinishDraining(std::unique_lock<internal::Mutex>&) {}

 private:
  friend class pw::bluetooth::proxy::L2capChannelManager;

  /// See L2capChannelManager::DrainChannelQueuesIfNewTx
  async2::Poll<> DoDrainChannelQueuesIfNewTx(async2::Context& context);

  static constexpr sync::NoLock& channels_mutex()
      PW_LOCK_RETURNED(channels_mutex_) {
    return channels_mutex_;
  }

  /// Fake mutex to satisfy lock annotations. Locking is not needed as all these
  /// routines are run on the dispatcher thread.
  static sync::NoLock channels_mutex_;

  L2capChannelManager& manager_;
  Allocator& allocator_;

  /// Dispatcher used to run asynchronous tasks.
  async2::Dispatcher* dispatcher_ = nullptr;

  /// ID of the thread that called `SetDispatcher` or `CreateDispatcher`.
  thread::Id dispatcher_thread_id_;

  /// Task wrapper that calls `DoDrainChannelQueuesIfNewTx`.
  class DrainTask final : public async2::Task {
   public:
    explicit DrainTask(L2capChannelManagerImpl& impl)
        : async2::Task(PW_ASYNC_TASK_NAME("L2capChannelManager:Drain")),
          impl_(impl) {}

    ~DrainTask() override { Deregister(); }

   private:
    async2::Poll<> DoPend(async2::Context& context) override;
    L2capChannelManagerImpl& impl_;
  } drain_task_;

  /// Wakes the drain task when channels are registered.
  async2::Waker waker_;

  /// Iterator to "least recently drained" channel.
  L2capChannelIterator lrd_channel_;

  /// Iterator to final channel to be visited in ongoing round robin.
  L2capChannelIterator round_robin_terminus_;

  /// Notifies the manager that additional send credits are available.
  ///
  /// TODO(b/469152981): If a `NotificationChannel` or similar is added to
  /// pw_async2, use that instead.
  async2::MpscChannelHandle<bool> credit_handle_;
  async2::Receiver<bool> credit_receiver_;
  async2::Sender<bool> credit_sender_;
  async2::ReceiveFuture<bool> credit_future_;
};

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
