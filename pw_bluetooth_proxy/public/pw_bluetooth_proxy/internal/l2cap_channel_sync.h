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

#if PW_BLUETOOTH_PROXY_ASYNC == 0

#include <cstddef>
#include <mutex>
#include <optional>

#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_containers/inline_queue.h"
#include "pw_function/function.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/id.h"

namespace pw::bluetooth::proxy {

class L2capChannel;

namespace internal {

class GenericL2capChannel;
class GenericL2capChannelImpl;
class L2capChannelImpl;

/// RAII helper type that guarantees an L2capChannel will not be destroyed as
/// long as an instance of this type remains in scope.
class BorrowedL2capChannel {
 public:
  constexpr BorrowedL2capChannel() = default;

  BorrowedL2capChannel(BorrowedL2capChannel&& other) {
    *this = std::move(other);
  }
  BorrowedL2capChannel& operator=(BorrowedL2capChannel&& other);

  ~BorrowedL2capChannel();

  L2capChannel* operator->();
  L2capChannel& operator*();

 private:
  friend class GenericL2capChannelImpl;
  friend class L2capChannelImpl;
  friend class pw::bluetooth::proxy::L2capChannel;

  BorrowedL2capChannel(L2capChannelImpl& impl);

  L2capChannelImpl* impl_ = nullptr;
};

/// Implementation detail class for L2capChannel.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is false; that is, when
/// the proxy is using a purely synchronous execution model using one or more
/// threads and using mutexes to protect shared state from concurrent
/// modification.
class L2capChannelImpl {
 public:
  // TODO: https://pwbug.dev/349700888 - Make capacity configurable.
  static constexpr size_t kQueueCapacity = 5;

  static constexpr sync::Mutex& mutex() PW_LOCK_RETURNED(static_mutex_) {
    return static_mutex_;
  }

  struct DequeueResult {
    std::optional<H4PacketWithH4> packet;
    bool should_notify = false;
  };

  /// Dequeues the next Tx packet from the channel's payload queue, if one can
  /// be generated.
  ///
  /// Sets `should_notify` if dequeuing freed queue space that a client is
  /// waiting for. The caller must then send
  /// `L2capChannelEvent::kWriteAvailable` after releasing
  /// `L2capChannelManagerImpl::channels_mutex_`, since the client's event
  /// callback may acquire it.
  DequeueResult DequeuePacket() PW_LOCKS_EXCLUDED(mutex_);

  // Returns whether this object has no more pending payloads.
  [[nodiscard]] bool PayloadQueueEmpty() const PW_LOCKS_EXCLUDED(mutex_);

  // Send `event` to client if an event callback was provided.
  void SendEvent(L2capChannelEvent event)
      PW_LOCKS_EXCLUDED(static_mutex_, mutex_);
  void SendEventLocked(L2capChannelEvent event)
      PW_EXCLUSIVE_LOCKS_REQUIRED(static_mutex_);

  // Returns whether this object is stale, that is, whether it has been closed
  // or its client has disconnected.
  bool IsStale() const PW_LOCKS_EXCLUDED(static_mutex_);

 private:
  friend class pw::bluetooth::proxy::L2capChannel;
  friend class BorrowedL2capChannel;
  friend class GenericL2capChannelImpl;

  struct ActiveCallback {
    thread::Id thread_id;
    ActiveCallback* next = nullptr;
  };

  /// Registers a running channel callback (`from_controller_fn_`,
  /// `from_host_fn_`, or `event_fn_`) so a client disconnecting on another
  /// thread waits for it. Inactive if the channel is already closed, in which
  /// case the callback must not run.
  class ClientCallbackGuard {
   public:
    explicit ClientCallbackGuard(L2capChannelImpl& impl)
        PW_LOCKS_EXCLUDED(static_mutex_, mutex_);
    ClientCallbackGuard(const ClientCallbackGuard&) = delete;
    ClientCallbackGuard& operator=(const ClientCallbackGuard&) = delete;
    ClientCallbackGuard(ClientCallbackGuard&&) = delete;
    ClientCallbackGuard& operator=(ClientCallbackGuard&&) = delete;
    ~ClientCallbackGuard() PW_LOCKS_EXCLUDED(static_mutex_, mutex_);

    bool active() const { return borrowed_.impl_ != nullptr; }

   private:
    // Declared first so it is destroyed last, releasing the borrow only after
    // the destructor body has unlinked `callback_`.
    BorrowedL2capChannel borrowed_;
    ActiveCallback callback_{};
  };

  explicit L2capChannelImpl(L2capChannel& channel) : channel_(channel) {}

  /// Completes initialization of the channel details.
  ///
  /// Trivially succeeds when using sync mode.
  constexpr Status Init() { return OkStatus(); }

  /// Returns whether a callback is running on a thread other than `caller_id`.
  bool HasCrossThreadCallbackLocked(thread::Id caller_id) const
      PW_EXCLUSIVE_LOCKS_REQUIRED(static_mutex_);

  /// Disconnects the client and waits for callbacks running on other threads
  /// to return, releasing `lock` (`static_mutex_`) while waiting. Callbacks on
  /// the calling thread are not waited for.
  ///
  /// The caller must not hold any lock that the channel's callbacks acquire.
  void DisconnectClientLocked(std::unique_lock<sync::Mutex>& lock);

  /// Blocks until notified that the number of borrows has reached zero.
  ///
  /// Releases the given `lock` while blocking, and reacquires it when notified.
  void BlockWhileBorrowed(std::unique_lock<sync::Mutex>& lock);

  /// Disconnects the client channel, if connected.
  void Close() PW_LOCKS_EXCLUDED(static_mutex_, mutex_);

  void DisconnectLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(static_mutex_);

  /// @copydoc L2capChannel::Write
  StatusWithMultiBuf Write(multibuf::MultiBuf&& payload)
      PW_LOCKS_EXCLUDED(mutex_);

  /// Determine if channel is ready to accept one or more Write payloads.
  ///
  /// @returns
  /// * @OK: Channel is ready to accept one or more `Write` payloads.
  /// * @UNAVAILABLE: Channel does not yet have the resources to queue a Write
  ///   at this time (transient error). If an `event_fn` has been provided it
  ///   will be called with `L2capChannelEvent::kWriteAvailable` when there is
  ///   queue space available again.
  /// * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  Status IsWriteAvailable() PW_LOCKS_EXCLUDED(mutex_);

  /// Request a "write available" event be sent when space become available in
  /// the payload queue.
  void NotifyOnDequeue() PW_LOCKS_EXCLUDED(mutex_);

  /// @copydoc L2capChannel::ReportNewTxPacketsOrCredits
  void ReportNewTxPacketsOrCredits();

  /// @copydoc L2capChannel::ClearQueue
  void ClearQueue() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /// Mutex for guarding internal and client channel pointers to each other.
  /// This mutex is shared between all internal and client channel objects.
  static sync::Mutex static_mutex_ PW_ACQUIRED_BEFORE(mutex_);

  // Mutex for guarding state.
  mutable sync::Mutex mutex_ PW_ACQUIRED_AFTER(static_mutex_);

  L2capChannel& channel_;

  // Pointer to the acquired client channel while connected, or nullptr for
  // internal channels (e.g. signaling channels) and disconnected channels.
  GenericL2capChannelImpl* client_ PW_GUARDED_BY(static_mutex_) = nullptr;

  // Running callbacks, linked through `ClientCallbackGuard`s on their stacks,
  // so a disconnect can tell callbacks on other threads from re-entrant ones.
  ActiveCallback* active_callbacks_ PW_GUARDED_BY(static_mutex_) = nullptr;

  // Set while `DisconnectClientLocked()` waits. At most one, since only one
  // client owns a channel.
  sync::ThreadNotification* disconnect_waiter_ PW_GUARDED_BY(static_mutex_) =
      nullptr;

  // Stores client Tx payload buffers.
  InlineQueue<multibuf::MultiBuf, kQueueCapacity> payload_queue_
      PW_GUARDED_BY(mutex_);

  // True if the last queue attempt didn't have space. Will be cleared on
  // successful dequeue.
  bool notify_on_dequeue_ PW_GUARDED_BY(mutex_) = false;

  /// The number of outstanding `BorrowedL2capChannel` objects for this object.
  uint8_t num_borrows_ PW_GUARDED_BY(mutex_) = 0;

  // Set by `Close()` or a client disconnect; stops new callbacks from running.
  bool closed_ PW_GUARDED_BY(static_mutex_) = false;

  /// Used to unblock the destructor when the number of borrows drops to zero.
  sync::ThreadNotification notification_;
};

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
