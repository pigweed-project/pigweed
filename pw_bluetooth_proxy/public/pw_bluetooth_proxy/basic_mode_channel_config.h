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

#include "pw_bluetooth_proxy/connection_handle.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"

namespace pw::bluetooth::proxy {

/// Parameters for configuring an L2CAP channel in basic mode.
struct BasicModeChannelConfig {
  /// The connection handle of the remote peer.
  ConnectionHandle connection_handle;

  /// L2CAP channel ID of the local endpoint.
  uint16_t local_channel_id;

  /// L2CAP channel ID of the remote endpoint.
  uint16_t remote_channel_id;

  /// Logical link transport type.
  AclTransportType transport;

  /// Whether this channel tolerates data loss for snapshot recovery.
  bool allow_data_loss = false;
};

}  // namespace pw::bluetooth::proxy
