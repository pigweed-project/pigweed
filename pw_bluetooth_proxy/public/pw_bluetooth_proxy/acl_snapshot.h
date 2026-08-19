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

#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

struct AclConnectionSnapshot {
  uint16_t connection_handle;
  AclTransportType transport;
  uint16_t num_proxy_pending_packets = 0;
  uint16_t num_host_pending_packets = 0;
  uint16_t num_queued_host_packets = 0;

  /// Checks primary-key handle equality.
  bool MatchesKey(uint16_t handle) const;

  /// Updates an individual ACL connection snapshot entry in-place.
  Status Update(const AclConnectionSnapshot& update);
};

/// Represents the persisted credit state of a single ACL transport (LE or
/// BR/EDR).
struct AclTransportSnapshot {
  /// Maximum number of ACL data packets the controller can hold for this
  /// transport.
  uint16_t controller_max_packets;

  /// Total number of uncompleted packets currently in flight across this
  /// transport.
  uint16_t pending = 0;
};

/// Incremental update payload emitted on connection credit mutations.
struct AclStateUpdate {
  /// Updated snapshot of the affected ACL connection.
  AclConnectionSnapshot connection;
};

/// Callback invoked when an ACL connection's credit state mutates.
///
/// @note When receiving an @c AclStateUpdate, the platform container is
/// responsible for updating the corresponding connection in its persistent
/// @c AclSnapshot, as well as updating the matching
/// @c AclTransportSnapshot::pending count (the sum of pending packets across
/// all connections on that transport).
///
/// @warning **Re-entrancy Safety:** Do not invoke proxy methods from within
/// this callback; it is called synchronously while holding internal mutexes.
using AclStateUpdateCallback = Function<void(const AclStateUpdate& update)>;

struct AclSnapshot {
  bool snapshot_incomplete = false;
  AclTransportSnapshot le_transport;
  AclTransportSnapshot br_edr_transport;
  Vector<AclConnectionSnapshot,
         PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_CONNECTIONS>
      acl_connections;

  /// Applies state updates in-place to the top-level ACL subsystem snapshot.
  Status ApplyStateUpdate(const AclStateUpdate& update);
};

}  // namespace pw::bluetooth::proxy
