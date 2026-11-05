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
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_coc_config.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

/// L2CAP connection-oriented channel that supports writing to and reading
/// from a remote peer.
class L2capCoc final : public internal::GenericL2capChannel {
 public:
  // TODO: https://pwbug.dev/382783733 - Move downstream client to
  // `L2capChannelEvent` instead of `L2capCoc::Event` and delete this alias.
  using Event = L2capChannelEvent;

  using CocConfig = ConnectionOrientedChannelConfig;

  using internal::GenericL2capChannel::SendAdditionalRxCredits;

  L2capCoc(L2capCoc&& other) = default;
  L2capCoc& operator=(L2capCoc&& other) = default;

 private:
  friend class L2capChannelManager;

  explicit L2capCoc(L2capChannel& channel, uint16_t tx_mtu);

  /// @copydoc internal::GenericL2capChannel::DoCheckWriteParameter
  Status DoCheckWriteParameter(const multibuf::MultiBuf& payload) override;

  uint16_t tx_mtu_ = 0;
};

}  // namespace pw::bluetooth::proxy
