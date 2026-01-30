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

#include "pw_bluetooth_sapphire/internal/connection.h"

namespace pw::bluetooth_sapphire::internal {

namespace {
// This lock has to be global rather than a member variable so that dispatcher
// tasks can lock the mutex before dereferencing their weak pointers.
pw::sync::Mutex g_connection_lock;
}  // namespace

pw::sync::Mutex& Connection::lock() { return g_connection_lock; }

Connection::Connection(
    bt::PeerId peer_id,
    std::unique_ptr<bt::gap::LowEnergyConnectionHandle> handle,
    pw::async::Dispatcher& dispatcher)
    : peer_id_(peer_id), dispatcher_(dispatcher), handle_(std::move(handle)) {
  // This is safe because the constructor is only called on the Bluetooth
  // thread.
  handle_->set_closed_callback([self = self_]() PW_NO_LOCK_SAFETY_ANALYSIS {
    std::lock_guard guard(g_connection_lock);
    if (!self.is_alive()) {
      return;
    }
    // TODO: https://pwbug.dev/396449684 - Update set_closed_callback with
    // disconnect reason.
    self->disconnect_reason_ = DisconnectReason::kFailure;
    self->waker_.Wake();
  });
}

Connection::~Connection() {
  std::lock_guard guard(lock());
  // This will destroy handle_ on the Bluetooth thread.
  Status post_status =
      dispatcher_.Post([handle = std::move(handle_)](auto, auto) {});
  PW_CHECK_OK(post_status);

  weak_factory_.InvalidatePtrs();
}

async2::Poll<pw::bluetooth::low_energy::Connection2::DisconnectReason>
Connection::PendDisconnect(async2::Context& cx) {
  std::lock_guard guard(lock());
  if (disconnect_reason_) {
    return async2::Ready(*disconnect_reason_);
  }
  PW_ASYNC_STORE_WAKER(cx, waker_, "bt-disconnect");
  return async2::Pending();
}

pw::bluetooth::gatt::Client2* Connection::GattClient() {
  // TODO: https://pwbug.dev/396449684 - Return Client2
  return nullptr;
}

uint16_t Connection::AttMtu() {
  // TODO: https://pwbug.dev/396449684 - Return actual MTU
  return 0u;
}

async2::Poll<uint16_t> Connection::PendAttMtuChange(async2::Context&) {
  // TODO: https://pwbug.dev/396449684 - Wire up MTU change logic.
  return async2::Pending();
}

pw::bluetooth::low_energy::Connection2::ConnectionParameters
Connection::Parameters() {
  // TODO: https://pwbug.dev/396449684 - Get the actual connection parameters.
  return pw::bluetooth::low_energy::Connection2::ConnectionParameters();
}

async2::OptionalValueFuture<pw::expected<
    void,
    pw::bluetooth::low_energy::Connection2::ConnectionParameterUpdateError>>
Connection::RequestParameterUpdate(RequestedConnectionParameters) {
  // TODO: https://pwbug.dev/396449684 - Update the parameters.
  return async2::OptionalValueFuture<pw::expected<
      void,
      pw::bluetooth::low_energy::Connection2::ConnectionParameterUpdateError>>::
      Resolved(std::nullopt);
}

async2::ValueFuture<
    std::optional<pw::Result<pw::bluetooth::low_energy::Channel::Ptr>>>
Connection::ConnectL2cap(ConnectL2capParameters) {
  // TODO: https://pwbug.dev/396449684 - Open an L2CAP channel.
  return async2::ValueFuture<
      std::optional<pw::Result<pw::bluetooth::low_energy::Channel::Ptr>>>::
      Resolved(pw::Status::Unimplemented());
}

}  // namespace pw::bluetooth_sapphire::internal
