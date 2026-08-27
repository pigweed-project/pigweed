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

#include <cstdint>

namespace pw::bluetooth::proxy {
/// Parameters for a direction of packet flow in an `L2capCoc`.
struct ConnectionOrientedChannelConfig {
  /// Channel identifier of the endpoint.
  /// For Rx: Local CID.
  /// For Tx: Remote CID.
  uint16_t cid;
  /// Maximum Transmission Unit.
  /// For Rx: Specified by local device. Indicates the maximum SDU size we are
  ///         capable of accepting.
  /// For Tx: Specified by remote peer. Indicates the maximum SDU size we are
  ///         allowed to send.
  uint16_t mtu;
  /// Maximum PDU payload Size.
  /// For Rx: Specified by local device. Indicates the maximum payload size
  ///         for an L2CAP packet we are capable of accepting.
  /// For Tx: Specified by remote peer. Indicates the maximum payload size for
  ///         for an L2CAP packet we are allowed to send.
  uint16_t mps;
  /// For Rx: Tracks the number of credits we have currently apportioned to
  ///         the remote peer for sending us K-frames in LE Credit Based Flow
  ///         Control mode.
  /// For Tx: Currently available credits for sending K-frames in LE Credit
  ///         Based Flow Control mode. This may be different from the initial
  ///         value if the container has already sent K-frames and/or received
  ///         credits.
  uint16_t credits;
  /// Whether this channel tolerates data loss for snapshot recovery.
  bool allow_data_loss = false;
};
}  // namespace pw::bluetooth::proxy
