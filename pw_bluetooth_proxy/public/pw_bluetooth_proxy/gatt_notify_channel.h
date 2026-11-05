// Copyright 2024 The Pigweed Authors
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

#include "pw_bluetooth_proxy/internal/generic_l2cap_channel.h"

namespace pw::bluetooth::proxy {

/// `GattNotifyChannel` supports sending GATT characteristic notifications to a
/// remote peer.
class GattNotifyChannel final : public internal::GenericL2capChannel {
 public:
  GattNotifyChannel(GattNotifyChannel&& other) = default;
  GattNotifyChannel& operator=(GattNotifyChannel&& other) = default;

  uint16_t attribute_handle() const { return attribute_handle_; }

 private:
  friend class L2capChannelManager;

  explicit GattNotifyChannel(L2capChannel& channel, uint16_t attribute_handle);

  /// @copydoc internal::GenericL2capChannel::DoCheckWriteParameter
  Status DoCheckWriteParameter(const multibuf::MultiBuf& payload) override;

  uint16_t attribute_handle_;
  uint16_t max_attribute_size_ = 0;
};

}  // namespace pw::bluetooth::proxy
