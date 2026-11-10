// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/l2cap/enhanced_retransmission_mode_engines.h"

#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"

namespace bt::l2cap::internal {

// This factory function ensures that their states are properly linked and
// allows for unit tests of their linked behaviors without adding dependencies
// between the classes nor tie their lifetimes together. If tying their
// lifetimes together is desired, one should perhaps hold reference counts on
// the other to preserve the independence.
std::pair<std::unique_ptr<EnhancedRetransmissionModeRxEngine>,
          std::unique_ptr<EnhancedRetransmissionModeTxEngine>>
MakeLinkedEnhancedRetransmissionModeEngines(
    ChannelId channel_id,
    uint16_t max_rx_sdu_size,
    uint16_t max_tx_sdu_size,
    uint8_t max_transmissions,
    uint8_t n_frames_in_tx_window,
    TxEngine::TxChannel& channel,
    EnhancedRetransmissionModeTxEngine::ConnectionFailureCallback
        connection_failure_callback,
    pw::async::Dispatcher& dispatcher) {
  auto rx_engine = std::make_unique<EnhancedRetransmissionModeRxEngine>(
      max_rx_sdu_size,
      kMaxInboundPduPayloadSize,
      fit::bind_member(&channel, &TxEngine::TxChannel::SendFrame),
      connection_failure_callback.share());
  auto tx_engine = std::make_unique<EnhancedRetransmissionModeTxEngine>(
      channel_id,
      max_tx_sdu_size,
      max_transmissions,
      n_frames_in_tx_window,
      channel,
      std::move(connection_failure_callback),
      dispatcher);

  // The direction swap here is because our acknowledgment sequence is based on
  // the peer's transmit sequence and vice versa.
  rx_engine->set_receive_seq_num_callback(
      fit::bind_member<&EnhancedRetransmissionModeTxEngine::UpdateAckSeq>(
          tx_engine.get()));
  rx_engine->set_ack_seq_num_callback(
      fit::bind_member<&EnhancedRetransmissionModeTxEngine::UpdateReqSeq>(
          tx_engine.get()));
  rx_engine->set_remote_busy_set_callback(
      fit::bind_member<&EnhancedRetransmissionModeTxEngine::SetRemoteBusy>(
          tx_engine.get()));
  rx_engine->set_remote_busy_cleared_callback(
      fit::bind_member<&EnhancedRetransmissionModeTxEngine::ClearRemoteBusy>(
          tx_engine.get()));
  rx_engine->set_single_retransmit_set_callback(
      fit::bind_member<
          &EnhancedRetransmissionModeTxEngine::SetSingleRetransmit>(
          tx_engine.get()));
  rx_engine->set_range_retransmit_set_callback(
      fit::bind_member<&EnhancedRetransmissionModeTxEngine::SetRangeRetransmit>(
          tx_engine.get()));
  return {std::move(rx_engine), std::move(tx_engine)};
}

}  // namespace bt::l2cap::internal
