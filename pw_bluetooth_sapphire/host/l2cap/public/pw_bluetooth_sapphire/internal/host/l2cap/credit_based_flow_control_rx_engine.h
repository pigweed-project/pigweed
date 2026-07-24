// Copyright 2024 The Pigweed Authors
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

#include <deque>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/rx_engine.h"

namespace bt::l2cap::internal {

// Implements the receiver state and logic for an L2CAP channel operating in
// either Enhanced or LE Credit-Based Flow Control Mode.
class CreditBasedFlowControlRxEngine final : public RxEngine {
 public:
  // Callback to invoke on a failure condition. In actual operation the
  // callback must disconnect the channel to remain compliant with the spec.
  // See Core Spec Ver 5.4, Vol 3, Part A, Sec 3.4.3.
  using FailureCallback = fit::callback<void()>;
  // Callback to invoke when credits should be returned to the transmitting end
  // of the connection.
  using ReturnCreditsCallback = fit::function<void(uint16_t credits)>;

  explicit CreditBasedFlowControlRxEngine(uint16_t max_mtu,
                                          uint16_t max_mps,
                                          uint16_t initial_credits,
                                          FailureCallback failure_callback,
                                          ReturnCreditsCallback return_credits);
  ~CreditBasedFlowControlRxEngine() override = default;

  ByteBufferPtr ProcessPdu(PDU pdu) override;
  void AcknowledgeRead() override;

  bool IsQueueEmpty() override { return next_sdu_ == nullptr; }

 private:
  uint16_t max_mtu_;
  uint16_t max_mps_;
  uint16_t peer_credits_;

  FailureCallback failure_callback_;
  ReturnCreditsCallback return_credits_callback_;

  MutableByteBufferPtr next_sdu_ = nullptr;
  size_t valid_bytes_ = 0;

  std::deque<uint16_t> unacked_read_credits_ = {};
  uint16_t current_sdu_credits_ = 0;

  // Call the failure callback and reset.
  void OnFailure();

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(CreditBasedFlowControlRxEngine);
};

}  // namespace bt::l2cap::internal
