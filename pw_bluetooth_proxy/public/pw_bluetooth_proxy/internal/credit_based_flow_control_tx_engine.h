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

#include "pw_bluetooth_proxy/internal/tx_engine.h"
#include "pw_bluetooth_proxy/l2cap_coc_config.h"

namespace pw::bluetooth::proxy::internal {

/// A TxEngine that implements LE Credit Based Flow Control mode.
/// This class is not thread-safe and external synchronization is required.
class CreditBasedFlowControlTxEngine final : public TxEngine {
 public:
  CreditBasedFlowControlTxEngine(ConnectionOrientedChannelConfig& config,
                                 uint16_t connection_handle,
                                 uint16_t local_cid,
                                 Delegate& delegate);

  CreditBasedFlowControlTxEngine(CreditBasedFlowControlTxEngine&& other) =
      delete;
  CreditBasedFlowControlTxEngine& operator=(
      CreditBasedFlowControlTxEngine&& other) = delete;

  uint16_t remaining_credits() const;

  bool sdu_in_progress() const;

  // TxEngine overrides:

  Result<H4PacketWithH4> GenerateNextPacket(const multibuf::MultiBuf& sdu,
                                            bool& keep_payload) override;
  Status CheckWriteParameter(const multibuf::MultiBuf& payload) override;
  Result<bool> AddCredits(uint16_t credits) override;
  HandlePduFromHostReturnValue HandlePduFromHost(
      pw::span<uint8_t> frame) override;

 private:
  std::optional<uint16_t> MaxBasicL2capPayloadSize() const;

  Delegate& delegate_;
  const uint16_t mtu_;
  const uint16_t mps_;
  const uint16_t connection_handle_;
  const uint16_t remote_cid_;
  const uint16_t local_cid_;

  uint16_t credits_;
  uint16_t sdu_offset_ = 0;
  bool is_continuing_segment_ = false;
};

}  // namespace pw::bluetooth::proxy::internal
