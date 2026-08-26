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
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy::hci {

struct SniffConnectionSnapshot {
  uint16_t connection_handle = 0;
  uint16_t max_interval = 0;
  uint16_t min_interval = 0;
  uint16_t attempt = 0;
  uint16_t timeout = 0;
  uint16_t link_inactivity_timeout = 0;
  uint16_t subrating_max_latency = 0;
  uint16_t subrating_min_remote_timeout = 0;
  uint16_t subrating_min_local_timeout = 0;
  bool allow_exit_sniff_on_rx = false;
  bool allow_exit_sniff_on_tx = false;

  /// Checks primary-key handle equality.
  bool MatchesKey(uint16_t handle) const;

  /// Updates an individual Sniff connection snapshot entry in-place.
  Status Update(const SniffConnectionSnapshot& update);
};

struct SniffSnapshot;

/// Variant holding either a top-level global Sniff snapshot or a per-connection
/// update.
using SniffStateUpdate = std::variant<SniffSnapshot, SniffConnectionSnapshot>;

struct SniffSnapshot {
  bool snapshot_incomplete = false;
  bool sniff_enabled = false;
  uint16_t subrating_max_latency = 0;
  uint16_t subrating_min_remote_timeout = 0;
  uint16_t subrating_min_local_timeout = 0;
  bool suppress_mode_change_event = false;
  bool suppress_sniff_subrating_event = false;
  pw::Vector<SniffConnectionSnapshot,
             PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_CONNECTIONS>
      connections;

  /// Updates an individual Sniff connection snapshot entry in-place.
  Status Update(const SniffConnectionSnapshot& update);

  /// Applies state updates in-place to the top-level Sniff subsystem snapshot.
  Status ApplyStateUpdate(const SniffStateUpdate& update);
};

/// Callback type invoked when the Sniff subsystem state mutates.
///
/// @warning **Re-entrancy Safety:** Do not invoke proxy methods from within
/// this callback; it is called synchronously while holding internal mutexes.
using SniffStateUpdateCallback =
    pw::Function<void(const SniffStateUpdate& update)>;

}  // namespace pw::bluetooth::proxy::hci
