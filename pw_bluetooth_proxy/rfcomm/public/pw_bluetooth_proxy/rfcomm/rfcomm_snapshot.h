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
#include <variant>

#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy::rfcomm {

/// Emitted when an RFCOMM channel is removed or closed.
struct RfcommChannelRemoved {
  uint16_t connection_handle = 0;
  uint8_t channel_number = 0;
  RfcommDirection direction = RfcommDirection::kResponder;
};

/// Snapshot record representing an active RFCOMM channel.
struct RfcommChannelSnapshot {
  uint16_t connection_handle = 0;
  uint8_t channel_number = 0;
  RfcommDirection direction = RfcommDirection::kResponder;
  uint16_t local_cid = 0;
  uint16_t remote_cid = 0;
  bool mux_initiator = false;
  uint8_t tx_credits = 0;
  uint8_t rx_credits = 0;
  uint8_t rx_total_credits = 0;
  uint16_t max_frame_size = 0;

  uint8_t dlci() const { return MakeDlci(channel_number, direction); }

  /// Checks primary-key equality against handle/channel/direction or removal.
  bool MatchesKey(uint16_t handle, uint8_t channel, RfcommDirection dir) const;
  bool MatchesKey(const RfcommChannelRemoved& removal) const;

  /// Updates an individual channel snapshot record in-place.
  Status Update(const RfcommChannelSnapshot& update);
};

/// Incremental update payload emitted on RFCOMM state mutations.
using RfcommStateUpdate =
    std::variant<RfcommChannelSnapshot, RfcommChannelRemoved>;

/// Callback type invoked when the RFCOMM subsystem state mutates.
///
/// @note When receiving an @c RfcommStateUpdate, the platform container is
/// responsible for updating the corresponding channel in its persistent
/// @c RfcommSnapshot.
///
/// @warning **Re-entrancy Safety:** Do not invoke proxy methods from within
/// this callback; it is called synchronously while holding internal mutexes.
using RfcommStateUpdateCallback =
    Function<void(const RfcommStateUpdate& update)>;

/// Top-level snapshot for the RFCOMM subsystem.
struct RfcommSnapshot {
  bool snapshot_incomplete = false;
  Vector<RfcommChannelSnapshot,
         PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_RFCOMM_CHANNELS>
      rfcomm_channels;

  /// Applies state updates in-place to the top-level RFCOMM subsystem snapshot.
  Status ApplyStateUpdate(const RfcommStateUpdate& update);
};

}  // namespace pw::bluetooth::proxy::rfcomm
