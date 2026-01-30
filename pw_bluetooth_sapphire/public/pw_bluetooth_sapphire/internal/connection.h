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

#include "pw_async/heap_dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_bluetooth/low_energy/connection2.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_handle.h"

namespace pw::bluetooth_sapphire::internal {

// Must be constructed on the Sapphire thread, but can be destroyed on any
// thread.
class Connection final : public pw::bluetooth::low_energy::Connection2 {
 public:
  // `dispatcher` must outlive this object.
  Connection(bt::PeerId peer_id,
             std::unique_ptr<bt::gap::LowEnergyConnectionHandle> handle,
             pw::async::Dispatcher& dispatcher);
  ~Connection() override;

  // Connection2 overrides:
  async2::Poll<DisconnectReason> PendDisconnect(async2::Context& cx) override;
  pw::bluetooth::gatt::Client2* GattClient() override;
  uint16_t AttMtu() override;
  async2::Poll<uint16_t> PendAttMtuChange(async2::Context& cx) override;
  ConnectionParameters Parameters() override;
  async2::ValueFuture<
      std::optional<pw::expected<void, ConnectionParameterUpdateError>>>
  RequestParameterUpdate(RequestedConnectionParameters parameters) override;
  async2::ValueFuture<
      std::optional<pw::Result<pw::bluetooth::low_energy::Channel::Ptr>>>
  ConnectL2cap(ConnectL2capParameters parameters) override;

 private:
  void Release() override { delete this; }

  // Internal lock for synchronizing user & Sapphire threads.
  pw::sync::Mutex& lock();

  std::optional<pw::bluetooth::low_energy::Connection2::DisconnectReason>
      disconnect_reason_ PW_GUARDED_BY(lock());
  const bt::PeerId peer_id_;
  pw::async::HeapDispatcher dispatcher_;
  // Must only be used/destroyed on the Sapphire thread.
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> handle_
      PW_GUARDED_BY(lock());

  async2::Waker waker_ PW_GUARDED_BY(lock());

  WeakSelf<Connection> weak_factory_ PW_GUARDED_BY(lock()){this};

  // Thread safe to copy and destroy, but lock must be held when using (WeakRef
  // is not thread safe).
  WeakSelf<Connection>::WeakPtr self_{weak_factory_.GetWeakPtr()};
};

}  // namespace pw::bluetooth_sapphire::internal
