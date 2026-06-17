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

#include <variant>

#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/span.h"

namespace pw::bluetooth::proxy::internal {

/// RxEngine is an interface for channel mode classes that process inbound L2CAP
/// packets.
class RxEngine {
 public:
  using HandlePduFromControllerReturnValue = std::variant<std::monostate,
                                                          multibuf::MultiBuf,
                                                          pw::span<uint8_t>,
                                                          L2capChannelEvent>;

  RxEngine() = default;
  RxEngine(RxEngine&&) = default;
  RxEngine& operator=(RxEngine&&) = default;

  virtual ~RxEngine() = default;

  /// Process a PDU received from the controller.
  /// @returns Monostate (do nothing), a buffer to send to the client,
  /// or an error channel event to send to the client.
  virtual HandlePduFromControllerReturnValue HandlePduFromController(
      pw::span<uint8_t> frame) = 0;

  virtual Status AddRxCredits(uint16_t additional_rx_credits) = 0;

  virtual void Reset() {}
};

}  // namespace pw::bluetooth::proxy::internal
