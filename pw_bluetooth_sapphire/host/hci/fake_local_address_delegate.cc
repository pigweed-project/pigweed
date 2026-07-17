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

#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"

#include <lib/fit/result.h>
#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bt::hci {

void FakeLocalAddressDelegate::EnablePrivacy(bool enabled) {
  privacy_enabled_ = enabled;

  if (!enabled && random_.has_value()) {
    random_.reset();

    // Notify the callback about the change in address.
    if (address_changed_callback_) {
      address_changed_callback_();
    }
  }
}

void FakeLocalAddressDelegate::EnsureLocalAddress(
    std::optional<DeviceAddress::Type> address_type, AddressCallback callback) {
  PW_DCHECK(callback);

  if (stalled_) {
    return;
  }

  if (!privacy_enabled_ && address_type.has_value() &&
      address_type.value() == DeviceAddress::Type::kLERandom) {
    bt_log(WARN,
           "hci-le",
           "Cannot advertise a random address while privacy is disabled");
    callback(fit::error(HostError::kInvalidParameters));
    return;
  }

  DeviceAddress address = local_address_;
  if (address_type == DeviceAddress::Type::kLEPublic || !privacy_enabled_) {
    address = identity_address_;
  }

  if (privacy_enabled_) {
    random_ = local_address_;
  }

  if (!async_) {
    callback(fit::ok(address));
    return;
  }

  (void)heap_dispatcher_.Post(
      [cb = std::move(callback), addr = std::move(address)](
          pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          cb(fit::ok(addr));
        }
      });
}

void FakeLocalAddressDelegate::UpdateRandomAddress(DeviceAddress& address) {
  random_ = address;

  // Notify the callback about the change in address.
  if (address_changed_callback_) {
    address_changed_callback_();
  }
}
}  // namespace bt::hci
