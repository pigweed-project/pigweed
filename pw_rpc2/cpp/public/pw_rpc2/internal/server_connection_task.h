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

#include "pw_allocator/allocator.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_rpc2/internal/connection_task.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_transport/socket.h"

namespace pw::rpc2 {

class Service;

namespace internal {

class ServerCall;
class ServerTask;

/// Connection task specialized for servers, managing the responder handshake,
/// incoming request dispatch, and lifecycle of server calls.
class ServerConnectionTask final : public ConnectionTask {
 public:
  /// Constructs a server connection task for a newly accepted connection.
  /// Begins in `State::kHandshaking`.
  ///
  /// The server must outlive the connection. A server it accepted through
  /// guarantees this itself: shutting down closes, unposts, and drops the last
  /// reference to every connection in its list. A connection constructed
  /// outside the server --- only tests do this --- must be deregistered and
  /// released before the server goes, or the dispatcher will outlive the
  /// server and destroy the connection too late.
  ServerConnectionTask(transport::ReliableDatagramSocket connection,
                       Allocator& allocator,
                       ServerTask& server_task);

  /// Constructs a server connection task for a connection whose handshake has
  /// already completed. Begins in `State::kActive`. For tests only.
  ServerConnectionTask(EstablishedConnection established_connection,
                       Allocator& allocator,
                       ServerTask& server_task);

  ~ServerConnectionTask() override;

  /// Retires every call on this connection, even ones that are still running.
  /// On an open connection, running calls end as if their methods returned.
  void ForceRetireAllCalls();

 private:
  async2::Poll<> DoPend(async2::Context& cx) override;
  bool PollConnection(async2::Context& cx);
  bool PollHandshake(async2::Context& cx);
  void HandleIncomingRequest(InboundPacket&& packet);

  /// Retires every server call that is done with, either because its method
  /// finished or because the call was closed underneath it.
  ///
  /// This runs on the connection rather than on the calls themselves because
  /// retirement destroys a call's method future and may free the call, which
  /// a call cannot do from inside its own poll.
  void CleanUpCalls();

  // The server that accepted this connection. A server outlives every
  // connection it accepted: its shutdown closes each one, unposts it, and
  // drops the last reference to it.
  ServerTask& server_task_;

  ResponderHandshakeFuture handshake_future_;
};

}  // namespace internal
}  // namespace pw::rpc2
