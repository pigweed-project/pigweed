// Copyright 2026 The Pigweed Authors
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

#include <atomic>
#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_containers/forward_list.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_rpc2/internal/control_future.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/service.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"
#include "pw_transport/transport.h"

namespace pw::rpc2 {

class Server;

namespace internal {

// Provides internal access to `Service`'s ID and method table, which are not
// part of its public API.
struct ServiceAccess {
 private:
  friend class ServerTask;
  friend class ServerConnectionTask;

  static uint32_t service_id(const Service& service) {
    return service.service_id();
  }

  static span<const Method> methods(const Service& service) {
    return service.methods();
  }

  static const Method* FindMethod(const Service& service, uint32_t method_id) {
    return service.FindMethod(method_id);
  }
};

/// One registered transport listener.
///
/// Owned by the `ServerTask`'s `listeners_` list, which allocates each record
/// from the server's allocator and so gives the embedded `AcceptFuture` a
/// stable address for its whole life. Except for registrations made before
/// `Start()`, the allocation happens on the dispatcher thread, so `Server`
/// does not require a thread-safe allocator.
///
/// A listener is state, not a thread of control, so it is polled by the
/// `ServerTask` rather than by a task of its own.
class ListenerRecord {
 public:
  /// Creates a record with no listener. Records are allocated before the
  /// request they will serve is taken, so the listener is bound afterwards
  /// with `set_listener()`.
  constexpr ListenerRecord() = default;

  void set_listener(transport::ReliableDatagramListener& listener) {
    listener_ = &listener;
  }

  transport::ReliableDatagramListener& listener() { return *listener_; }
  transport::ReliableDatagramListener::AcceptFuture& accept_future() {
    return accept_future_;
  }

  /// False once the listener has stopped accepting, either because `Accept()`
  /// failed or because the server is shutting down.
  bool is_accepting() const { return accepting_; }
  void stop_accepting() {
    accepting_ = false;
    accept_future_ = transport::ReliableDatagramListener::AcceptFuture();
  }

 private:
  transport::ReliableDatagramListener* listener_ = nullptr;
  transport::ReliableDatagramListener::AcceptFuture accept_future_;
  bool accepting_ = true;
};

/// The single task behind a `Server`.
///
/// It owns the service registry, the listeners and their accept futures, and
/// the accepted connections. All of that lives on one dispatcher and needs no
/// lock. The only shared state is the control inbox: three short lists of
/// pending requests and a flag telling the task to look at them.
///
/// Every control request follows the same pattern: the caller links a node it
/// already owns (the `Service` itself, or the `ControlFuture` it gets back)
/// into a list under `ControlLock()`, sets `control_pending_`, and wakes the
/// task. The task pops nodes and resolves them in O(1) critical sections, so
/// it never holds a reference to a caller's object outside the lock and never
/// has to reconcile a future that was moved or dropped in the meantime.
///
/// ```
/// kUnstarted --Start()--> kRunning --Close()--> kClosing --task--> kClosed
///      |                                                             ^
///      +--------------------------- Close() -------------------------+
/// ```
///
/// @note **Threading.** Only the control operations --- `RegisterService`,
/// `RegisterListener`, `RegisterListenerBlocking`, `Close`, `CloseBlocking`,
/// and `Start` --- may be called from another thread. Everything else, and
/// every object reachable from the registry, belongs to the dispatcher the
/// task was constructed with. See the threading contract in the `pw_rpc2`
/// documentation.
class ServerTask final : public async2::Task {
 public:
  constexpr ServerTask(Allocator& allocator, async2::Dispatcher& dispatcher)
      : Task(kTaskName),
        allocator_(allocator),
        dispatcher_(dispatcher),
        connections_(allocator),
        listeners_(allocator) {}

  ~ServerTask() override;

  ServerTask(const ServerTask&) = delete;
  ServerTask& operator=(const ServerTask&) = delete;

  // --- Control plane. Callable from any thread. ---

  /// Queues `service` for registration. Returns `FAILED_PRECONDITION` if the
  /// server is closing or closed. Registering a duplicate service ID is fatal.
  Status RegisterService(Service& service);

  ControlFuture RegisterListener(transport::ReliableDatagramListener& listener);
  Status RegisterListenerBlocking(
      transport::ReliableDatagramListener& listener) {
    return BlockOn(RegisterListener(listener));
  }

  ControlFuture Close();
  void CloseBlocking() { BlockOn(Close()).IgnoreError(); }

  /// Posts the task to the dispatcher it was constructed with. May only be
  /// called once, and not after `Close()`.
  void Start();

  // --- Dispatcher-thread only. ---

  Allocator& allocator() { return allocator_; }

  /// The dispatcher this server runs on, and which owns everything reachable
  /// from it.
  async2::Dispatcher& dispatcher() { return dispatcher_; }

  /// Returns the service registered with `service_id`, or null.
  Service* FindService(uint32_t service_id);

  /// Drops the server's reference to `connection`, if it still holds one.
  /// Called by a connection task as it retires.
  void RemoveConnection(ServerConnectionTask& connection);

 private:
  static constexpr log::Token kTaskName =
      PW_LOG_TOKEN("pw_async2", "pw::rpc2::Server");

  enum class State : uint8_t {
    kUnstarted,
    kRunning,
    kClosing,
    kClosed,
  };

  async2::Poll<> DoPend(async2::Context& cx) override;

  State state() const { return state_.load(std::memory_order_acquire); }

  void set_state_locked(State state)
      PW_EXCLUSIVE_LOCKS_REQUIRED(ControlLock()) {
    state_.store(state, std::memory_order_release);
  }

  /// Flags the inbox as non-empty and wakes the task, if it is running.
  void SignalControlLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(ControlLock());

  /// Submits a listener registration. Before `Start()`, applies it on the
  /// calling thread; while running, queues it for the dispatcher; otherwise
  /// fails it. In every case `future` ends up resolved or queued.
  void SubmitListener(ControlFuture& future) PW_LOCKS_EXCLUDED(ControlLock());

  /// Submits a close request, resolving `future` once the server is closed.
  void SubmitClose(ControlFuture& future) PW_LOCKS_EXCLUDED(ControlLock());

  /// Runs `future` to completion on the dispatcher in a `FutureTask`, blocking
  /// the calling thread, and returns its status. Takes the status directly if
  /// the future was already resolved on the calling thread.
  Status BlockOn(ControlFuture&& future);

  /// Moves queued services into the registry, or drops them if `closing`.
  /// Registering a duplicate service ID is fatal here.
  void ApplyPendingServices(bool closing) PW_LOCKS_EXCLUDED(ControlLock());

  /// Resolves queued listener registrations, allocating a record for each.
  void ApplyPendingListeners(bool closing) PW_LOCKS_EXCLUDED(ControlLock());

  void PollListeners(async2::Context& cx);

  /// Stops every listener, closes every connection, and frees both. Idempotent.
  void Shutdown();

  Allocator& allocator_;
  async2::Dispatcher& dispatcher_;

  // Lifecycle state. Written under `ControlLock()`; read without it by the
  // dispatcher thread, which is why it is atomic.
  std::atomic<State> state_{State::kUnstarted};

  // Set whenever a request is queued or the state changes, so that `DoPend()`
  // can skip the inbox entirely --- one atomic exchange, no lock --- on the
  // steady-state polls driven by listener and connection activity.
  std::atomic<bool> control_pending_{false};

  // Re-armed on every poll; woken by `SignalControlLocked()`.
  async2::Waker control_waker_;

  // The inbox. Services are their own list nodes, so queuing one allocates
  // nothing; listener requests and close requests are the futures themselves.
  IntrusiveForwardList<Service> pending_services_ PW_GUARDED_BY(ControlLock());
  ControlFuture::List listener_requests_ PW_GUARDED_BY(ControlLock());
  ControlFuture::List close_futures_ PW_GUARDED_BY(ControlLock());

  // Dispatcher thread only.
  IntrusiveForwardList<Service> services_;
  ForwardList<SharedPtr<ServerConnectionTask>> connections_;

  // Dispatcher thread only once started. Before `Start()` there is no
  // dispatcher thread to speak of, and listener registrations are applied on
  // the calling thread under `ControlLock()`.
  ForwardList<ListenerRecord> listeners_;
};

}  // namespace internal
}  // namespace pw::rpc2
