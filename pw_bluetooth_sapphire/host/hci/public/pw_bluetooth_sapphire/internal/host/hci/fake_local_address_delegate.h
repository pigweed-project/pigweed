// Copyright 2023 The Pigweed Authors
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
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"

namespace bt::hci {

class FakeLocalAddressDelegate : public LocalAddressDelegate {
 public:
  explicit FakeLocalAddressDelegate(pw::async::Dispatcher& pw_dispatcher)
      : heap_dispatcher_(pw_dispatcher) {}
  ~FakeLocalAddressDelegate() override = default;

  void EnablePrivacy(bool enabled);

  // Returns true if the privacy feature is currently enabled.
  bool privacy_enabled() const { return privacy_enabled_; }

  std::optional<UInt128> irk() const override { return std::nullopt; }
  DeviceAddress identity_address() const override { return identity_address_; }
  void EnsureLocalAddress(std::optional<DeviceAddress::Type> address_type,
                          AddressCallback callback) override;

  // Assign a callback to be notified any time the LE address changes.
  void register_address_changed_callback(fit::closure callback) {
    address_changed_callback_ = std::move(callback);
  }

  void UpdateRandomAddress(DeviceAddress& address);

  const DeviceAddress current_address() const {
    return (privacy_enabled_ && random_) ? random_.value() : identity_address_;
  }

  // If set to true EnsureLocalAddress runs its callback asynchronously.
  void set_async(bool value) { async_ = value; }

  void set_stalled(bool value) { stalled_ = value; }
  bool stalled() const { return stalled_; }

  void set_identity_address(const DeviceAddress& value) {
    identity_address_ = value;
  }
  void set_local_address(const DeviceAddress& value) { local_address_ = value; }

 private:
  fit::closure address_changed_callback_;

  bool async_ = false;
  bool stalled_ = false;
  bool privacy_enabled_ = false;

  // The random device address assigned to the controller if privacy is enabled.
  std::optional<DeviceAddress> random_;

  // LE public address
  DeviceAddress identity_address_ =
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0});

  // LE resolvable private address
  DeviceAddress local_address_ =
      DeviceAddress(DeviceAddress::Type::kLERandom, {0});

  pw::async::HeapDispatcher heap_dispatcher_;
};

}  // namespace bt::hci
