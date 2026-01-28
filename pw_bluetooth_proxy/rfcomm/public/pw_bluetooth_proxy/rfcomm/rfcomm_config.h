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

namespace pw::bluetooth::proxy::rfcomm {

// Parameters for a direction of packet flow in an `RfcommChannel`.
struct RfcommChannelConfig {
  /// Channel identifier of the endpoint.
  /// For Rx: Local CID.
  /// For Tx: Remote CID.
  uint16_t cid;

  // Maximum frame size.
  // For Rx: Specified by local device. Indicates the maximum frame size we are
  //         capable of accepting.
  // For Tx: Specified by remote peer. Indicates the maximum frame size we are
  //         allowed to send.
  uint16_t max_frame_size;

  // For Rx: Tracks the number of credits we have currently apportioned to
  //         the remote peer for sending us frames.
  // For Tx: Currently available credits for sending frames. This may be
  //         different from the initial value if the container has already sent
  //         frames and/or received credits.
  uint8_t initial_credits;
};

}  // namespace pw::bluetooth::proxy::rfcomm
