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

#include "pw_bluetooth_proxy/internal/rx_engine.h"
#include "pw_bluetooth_proxy/l2cap_coc_config.h"
#include "pw_multibuf/allocator.h"

namespace pw::bluetooth::proxy::internal {

/// An RxEngine that implements LE Credit Based Flow Control mode.
/// This class is not thread-safe and external synchronization is required.
class CreditBasedFlowControlRxEngine final : public RxEngine {
 public:
  CreditBasedFlowControlRxEngine(
      ConnectionOrientedChannelConfig config,
      multibuf::MultiBufAllocator& rx_multibuf_allocator,
      Function<Status(uint16_t)> replenish_rx_credits_fn)
      : local_cid_(config.cid),
        rx_mtu_(config.mtu),
        rx_mps_(config.mps),
        replenish_rx_credits_fn_(std::move(replenish_rx_credits_fn)),
        rx_remaining_credits_(config.credits),
        rx_total_credits_(config.credits),
        rx_multibuf_allocator_(&rx_multibuf_allocator) {}

  CreditBasedFlowControlRxEngine(CreditBasedFlowControlRxEngine&& other) =
      default;
  CreditBasedFlowControlRxEngine& operator=(
      CreditBasedFlowControlRxEngine&& other) = default;

  // RxEngine overrides:

  HandlePduFromControllerReturnValue HandlePduFromController(
      pw::span<uint8_t> frame) override;

  Status AddRxCredits(uint16_t additional_rx_credits) override;

  void Reset() override;

 private:
  uint16_t local_cid_;
  uint16_t rx_mtu_;
  uint16_t rx_mps_;
  Function<Status(uint16_t)> replenish_rx_credits_fn_;
  std::optional<multibuf::MultiBuf> rx_sdu_ = std::nullopt;
  uint16_t rx_sdu_offset_ = 0;
  uint16_t rx_sdu_bytes_remaining_ = 0;
  uint16_t rx_remaining_credits_;
  uint16_t rx_total_credits_;
  multibuf::MultiBufAllocator* rx_multibuf_allocator_;
};

}  // namespace pw::bluetooth::proxy::internal
