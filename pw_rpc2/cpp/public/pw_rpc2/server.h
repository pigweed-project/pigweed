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

#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_rpc2/internal/control_future.h"
#include "pw_rpc2/internal/server_task.h"
#include "pw_rpc2/service.h"
#include "pw_status/status.h"
#include "pw_transport/transport.h"

namespace pw::rpc2 {

/// Serves RPC services over one or more transport listeners.
///
/// A `Server` owns three things: the service registry, the set of transport
/// listeners, and the connections those listeners accept. Inbound request
/// packets are resolved against the service registry and dispatched to the
/// matching method.
///
/// Listeners and services may be registered before the server is started; they
/// are activated by `Start()`.
///
/// @note **Threading.** A started `Server` and everything reachable from it ---
/// its services' method bodies, the `Reader`, `Writer`, and `UnaryWriter`
/// handles they are given, and every future those produce --- belong to the
/// dispatcher passed to the constructor, and may only be touched from the
/// thread running that dispatcher. There is no locking.
///
/// The functions below are the only exceptions. They may be called from any
/// thread at any point in the server's life, including concurrently with the
/// dispatcher and with each other:
///
/// * `Start()`
/// * `RegisterService()`
/// * `RegisterListener()` / `RegisterListenerBlocking()`
/// * `Close()` / `CloseBlocking()`
///
/// Each of these hands a request to the server's dispatcher and returns;
/// nothing is allocated on the calling thread. `RegisterService()` needs no
/// completion signal, since a queued registration cannot fail. The future the
/// others return is the one kind of `pw_rpc2` object with no dispatcher
/// affinity --- it may be polled from any dispatcher, moved between threads,
/// or dropped. The `*Blocking()` forms instead block the calling thread until
/// the operation finishes, and so must not be called from the server's own
/// dispatcher thread once started.
///
/// Before `Start()` there is no dispatcher thread to hand requests to, so
/// listener registrations are applied on the calling thread instead and their
/// futures come back already resolved. This is the one time `Server` touches
/// its allocator off the dispatcher.
///
/// @note **Teardown.** A started server must be closed before it is destroyed:
/// either call `Close()` and run the dispatcher until the returned future
/// resolves, or call `CloseBlocking()` from a non-dispatcher thread. `~Server`
/// crashes otherwise. Closing unregisters every service and frees every
/// listener and connection.
///
/// @note **Close is terminal.** A closed server cannot be restarted or moved
/// to another dispatcher. Create a new `Server` instead.
class Server {
 public:
  /// Creates a server that will run on `dispatcher`. The dispatcher must
  /// outlive the server.
  ///
  /// `allocator` is only accessed on the dispatcher thread, except by listener
  /// registrations before `Start()`, so it does not need to be thread-safe
  /// unless shared with other threads outside `Server`.
  constexpr Server(Allocator& allocator, async2::Dispatcher& dispatcher)
      : task_(allocator, dispatcher) {}

  ~Server() = default;

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) = delete;
  Server& operator=(Server&&) = delete;

  /// Registers a service with the server so its methods can be invoked.
  ///
  /// Callable from any thread. Returns `OK` once the registration is queued;
  /// the dispatcher applies it before processing any further packets. Returns
  /// `FAILED_PRECONDITION` if the server is closing or closed.
  ///
  /// Registering a service whose ID is already registered is a fatal error,
  /// detected when the dispatcher applies the registration. The service stays
  /// registered until the server closes, and must outlive the server.
  Status RegisterService(Service& service) {
    return task_.RegisterService(service);
  }

  /// Starts accepting connections on `listener`.
  ///
  /// Callable from any thread. The returned future resolves once the listener
  /// is registered, with `RESOURCE_EXHAUSTED` if its bookkeeping could not be
  /// allocated or `FAILED_PRECONDITION` if the server is closing or closed.
  /// Dropping the future before it resolves withdraws the request.
  ///
  /// Before `Start()`, the registration is applied on the calling thread and
  /// the future is returned already resolved; the listener begins accepting
  /// when the server starts.
  ControlFuture RegisterListener(
      transport::ReliableDatagramListener& listener) {
    return task_.RegisterListener(listener);
  }

  /// `RegisterListener()`, but blocks the calling thread until the listener is
  /// registered.
  ///
  /// @pre Must not be called from the server's own dispatcher thread.
  Status RegisterListenerBlocking(
      transport::ReliableDatagramListener& listener) {
    return task_.RegisterListenerBlocking(listener);
  }

  /// Tears down the server: stops every listener, closes every connection and
  /// the calls on it, and unregisters every service.
  ///
  /// Callable from any thread. The returned future resolves once all of that
  /// has happened on the dispatcher. Closing is terminal; the server cannot be
  /// restarted.
  ControlFuture Close() { return task_.Close(); }

  /// `Close()`, but blocks the calling thread until teardown is complete.
  ///
  /// Works on a server that was never started, in which case it returns
  /// immediately.
  ///
  /// @pre Must not be called from the server's own dispatcher thread, as
  /// blocking the dispatcher would deadlock.
  void CloseBlocking() { task_.CloseBlocking(); }

  /// Starts the server on the dispatcher it was constructed with.
  ///
  /// Callable from any thread, but only once, and not after `Close()`.
  void Start() { task_.Start(); }

 private:
  // Holds everything: the registry, the listeners, the connections, and the
  // control inbox the thread-safe functions above post to.
  internal::ServerTask task_;
};

}  // namespace pw::rpc2
