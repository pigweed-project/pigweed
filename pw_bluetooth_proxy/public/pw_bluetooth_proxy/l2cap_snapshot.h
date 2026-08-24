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
#include <type_traits>
#include <variant>

#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

struct L2capChannelRemoved {
  uint16_t connection_handle = 0;
  uint16_t local_cid = 0;
};

struct L2capChannelSnapshot {
  uint16_t local_cid = 0;
  uint16_t remote_cid = 0;
  uint16_t connection_handle = 0;
  AclTransportType transport = AclTransportType::kLe;

  /// Checks primary-key equality against handle/CID or removal events.
  bool MatchesKey(uint16_t handle, uint16_t cid) const;
  bool MatchesKey(const L2capChannelRemoved& removal) const;

  /// Updates an individual channel snapshot record in-place.
  Status Update(const L2capChannelSnapshot& update);
};

using L2capStateUpdate =
    std::variant<L2capChannelSnapshot, L2capChannelRemoved>;

/// Callback type invoked when the L2CAP subsystem state mutates.
///
/// @warning **Re-entrancy Safety:** Do not invoke proxy methods from within
/// this callback; it is called synchronously while holding internal mutexes.
using L2capStateUpdateCallback = Function<void(const L2capStateUpdate& update)>;

struct L2capSnapshot {
  bool snapshot_incomplete = false;
  Vector<L2capChannelSnapshot,
         PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_L2CAP_CHANNELS>
      l2cap_channels;

  /// Applies state updates in-place to the top-level L2CAP subsystem snapshot.
  Status ApplyStateUpdate(const L2capStateUpdate& update);
};

}  // namespace pw::bluetooth::proxy
