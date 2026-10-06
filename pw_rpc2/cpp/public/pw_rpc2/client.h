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

#include <utility>
#include <variant>

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_assert/assert.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/client_connection_task.h"
#include "pw_rpc2/internal/control_future.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

namespace pw::rpc2 {

class Client;

namespace internal {
class GeneratedServiceClient;
}  // namespace internal

/// Future that resolves to a connected `Client`. Returned by
/// `Client::Connect()`.
class ClientFuture {
 public:
  using value_type = Result<Client>;

  /// Constructs an empty future, which is neither pendable nor complete.
  constexpr ClientFuture() = default;

  ClientFuture(const ClientFuture&) = delete;
  ClientFuture& operator=(const ClientFuture&) = delete;

  ClientFuture(ClientFuture&& other) noexcept { MoveFrom(other); }
  ClientFuture& operator=(ClientFuture&& other) noexcept {
    if (this != &other) {
      MoveFrom(other);
    }
    return *this;
  }

  [[nodiscard]] bool is_pendable() const {
    return !std::holds_alternative<std::monostate>(state_) && !is_complete();
  }
  [[nodiscard]] bool is_complete() const {
    return std::holds_alternative<Completed>(state_);
  }

  /// Connects (if created from a connector), performs the handshake, and
  /// starts the client's connection task.
  ///
  /// @returns
  /// * `OK` with the connected `Client`.
  /// * Any error from the connector's `Connect()`.
  /// * `DATA_LOSS` if the peer sent an invalid handshake.
  /// * `UNAVAILABLE` or `CANCELLED` if the connection closed during the
  ///   handshake.
  /// * `RESOURCE_EXHAUSTED` if the connection task could not be allocated.
  async2::Poll<Result<Client>> Pend(async2::Context& cx);

 private:
  friend class Client;

  struct Completed {};

  using Connector = transport::ReliableDatagramConnector*;
  using ConnectFuture = transport::ReliableDatagramConnector::ConnectFuture;

  ClientFuture(async2::Dispatcher& dispatcher,
               Allocator& allocator,
               transport::ReliableDatagramConnector& connector)
      : dispatcher_(&dispatcher), allocator_(&allocator), state_(&connector) {}

  ClientFuture(async2::Dispatcher& dispatcher,
               Allocator& allocator,
               transport::ReliableDatagramSocket&& connection)
      : dispatcher_(&dispatcher),
        allocator_(&allocator),
        state_(std::in_place_type<internal::InitiatorHandshakeFuture>,
               std::move(connection)) {}

  void MoveFrom(ClientFuture& other) {
    dispatcher_ = other.dispatcher_;
    allocator_ = other.allocator_;
    state_ = std::exchange(other.state_, std::monostate());
  }

  async2::Dispatcher* dispatcher_ = nullptr;
  Allocator* allocator_ = nullptr;

  // Empty (`std::monostate`), waiting to connect, connecting, handshaking, or
  // complete.
  std::variant<std::monostate,
               Connector,
               ConnectFuture,
               internal::InitiatorHandshakeFuture,
               Completed>
      state_;
};

static_assert(async2::Future<ClientFuture>);

/// Shared handle to an established RPC connection.
///
/// A `Client` refers to the connection it was created from: closing it closes
/// the transport and cancels every call on it. `Client` is cheaply copyable,
/// and all copies refer to the same underlying connection. Service clients
/// wrap a `Client` and bind it to a service ID.
///
/// @note **Threading.** Everything reachable through a client --- its calls,
/// their handles, and their futures --- is bound to the dispatcher the client
/// was created on and may only be used from that dispatcher's thread.
/// Exceptions: `Close()`, `CloseBlocking()`, `is_open()`, and copying or
/// destroying a `Client` are safe from any thread.
///
/// @note **Closing.** Destroying the last copy of a `Client` (including copies
/// held by service clients) closes the connection and cancels its in-flight
/// calls. The close happens asynchronously on the dispatcher. To be sure it has
/// finished before destroying the dispatcher or allocator, call
/// `CloseBlocking()` or await `Close()`.
class Client {
 public:
  /// Constructs an empty client, which refers to no connection.
  constexpr Client() = default;

  ~Client() { ReleaseConnectionIfHeld(); }

  Client(const Client& other) : connection_task_(other.connection_task_) {
    AddConnectionHandle();
  }
  Client& operator=(const Client& other);

  // Moving transfers the handle, so the handle count is unchanged.
  Client(Client&& other) noexcept = default;
  Client& operator=(Client&& other) noexcept;

  /// Returns a future that connects to a peer through `connector`, performs
  /// the handshake, and resolves to the connected `Client`.
  ///
  /// The client's connection task is allocated from `allocator` and runs on
  /// `dispatcher`.
  static ClientFuture Connect(async2::Dispatcher& dispatcher,
                              Allocator& allocator,
                              transport::ReliableDatagramConnector& connector) {
    return ClientFuture(dispatcher, allocator, connector);
  }

  /// Returns a future that performs the handshake over an already-connected
  /// `connection` and resolves to the connected `Client`.
  ///
  /// The client's connection task is allocated from `allocator` and runs on
  /// `dispatcher`.
  static ClientFuture Connect(async2::Dispatcher& dispatcher,
                              Allocator& allocator,
                              transport::ReliableDatagramSocket connection) {
    return ClientFuture(dispatcher, allocator, std::move(connection));
  }

  /// Closes the connection shared by this client and all its copies,
  /// completing every call on it with `CANCELLED`, and releases this client's
  /// handle to the connection. The returned future resolves once the
  /// dispatcher has torn the connection down, or immediately if the client is
  /// empty or already closed.
  ///
  /// Call handles and futures still held by the user stay valid but inert
  /// until destroyed.
  ControlFuture Close();

  /// `Close()`, but blocks the calling thread until the connection is closed.
  ///
  /// @pre Must not be called from the client's own dispatcher thread.
  void CloseBlocking();

  /// Returns true if this client refers to a connection that has not been
  /// closed.
  [[nodiscard]] bool is_open() const {
    return connection_task_ != nullptr && connection_task_->is_open();
  }

 private:
  friend class ClientFuture;
  friend class internal::GeneratedServiceClient;
  friend struct internal::CallAccess;

  Client(async2::Dispatcher& dispatcher,
         Allocator& allocator,
         internal::EstablishedConnection established_connection);

  IntrusivePtr<internal::Call> CreateCall() const {
    PW_ASSERT(connection_task_ != nullptr);
    return connection_task_->CreateCall();
  }

  void AddConnectionHandle() const {
    if (connection_task_ != nullptr) {
      connection_task_->AddUserHandle();
    }
  }

  // Drops this handle, closing the connection if it was the last one, and
  // leaves this client empty.
  void ReleaseConnectionIfHeld() {
    if (connection_task_ != nullptr) {
      ReleaseConnection();
    }
  }

  void ReleaseConnection() {
    connection_task_->ReleaseUserHandle();
    connection_task_ = nullptr;
  }

  SharedPtr<internal::ClientConnectionTask> connection_task_;
};

}  // namespace pw::rpc2
