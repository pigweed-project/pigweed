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

#include "pw_rpc2/client.h"

#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/try.h"

namespace pw::rpc2 {

async2::Poll<Result<Client>> ClientFuture::Pend(async2::Context& cx) {
  PW_CHECK(is_pendable());

  if (Connector* connector = std::get_if<Connector>(&state_)) {
    state_.emplace<ConnectFuture>((*connector)->Connect());
  }

  if (ConnectFuture* connect = std::get_if<ConnectFuture>(&state_)) {
    PW_TRY_READY_ASSIGN(auto connection, connect->Pend(cx));
    if (!connection.ok()) {
      state_ = Completed();
      return async2::Ready(Result<Client>(connection.status()));
    }
    state_.emplace<internal::InitiatorHandshakeFuture>(std::move(*connection));
  }

  PW_TRY_READY_ASSIGN(
      auto established,
      std::get<internal::InitiatorHandshakeFuture>(state_).Pend(cx));
  state_ = Completed();
  if (!established.ok()) {
    return async2::Ready(Result<Client>(established.status()));
  }

  Client client(*dispatcher_, *allocator_, std::move(*established));
  if (!client.is_open()) {
    return async2::Ready(Result<Client>(Status::ResourceExhausted()));
  }
  return async2::Ready(Result<Client>(std::move(client)));
}

// A null `connection_task_` (allocation failed) leaves the client empty;
// `ClientFuture::Pend` reports it as RESOURCE_EXHAUSTED.
Client::Client(async2::Dispatcher& dispatcher,
               Allocator& allocator,
               internal::EstablishedConnection established_connection)
    : connection_task_(dispatcher.Post<internal::ClientConnectionTask>(
          allocator, std::move(established_connection), allocator)) {
  AddConnectionHandle();
}

Client& Client::operator=(const Client& other) {
  if (this != &other) {
    // Add first so that assigning between two handles to the same connection
    // never transiently drops its count to zero.
    other.AddConnectionHandle();
    ReleaseConnectionIfHeld();
    connection_task_ = other.connection_task_;
  }
  return *this;
}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    ReleaseConnectionIfHeld();
    connection_task_ = std::move(other.connection_task_);
  }
  return *this;
}

ControlFuture Client::Close() {
  if (connection_task_ == nullptr) {
    return ControlFuture::Resolved(OkStatus());
  }
  ControlFuture future = connection_task_->Close();
  ReleaseConnection();
  return future;
}

void Client::CloseBlocking() {
  if (connection_task_ != nullptr) {
    connection_task_->CloseBlocking();
    ReleaseConnection();
  }
}

}  // namespace pw::rpc2
