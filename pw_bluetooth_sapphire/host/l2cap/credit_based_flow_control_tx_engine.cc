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

#include "pw_bluetooth_sapphire/internal/host/l2cap/credit_based_flow_control_tx_engine.h"

#include <pw_assert/check.h>
#include <pw_bluetooth/l2cap_frames.emb.h>

#include <algorithm>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bt::l2cap::internal {
namespace {

namespace emboss = pw::bluetooth::emboss;

constexpr auto kLeCreditBasedFlowControlMode =
    CreditBasedFlowControlMode::kLeCreditBasedFlowControl;

constexpr auto kSduHeaderSize = emboss::KFrameSduHeader::IntrinsicSizeInBytes();

constexpr uint32_t kMaxCredits = 65535;

template <typename Enum>
constexpr typename std::underlying_type<Enum>::type EnumValue(const Enum& e) {
  return static_cast<typename std::underlying_type<Enum>::type>(e);
}

}  // namespace

CreditBasedFlowControlTxEngine::CreditBasedFlowControlTxEngine(
    ChannelId channel_id,
    uint16_t max_tx_sdu_size,
    TxChannel& channel,
    CreditBasedFlowControlMode mode,
    uint16_t max_tx_pdu_payload_size,
    uint16_t initial_credits)
    : TxEngine(channel_id, max_tx_sdu_size, channel),
      mode_(mode),
      max_tx_pdu_payload_size_(max_tx_pdu_payload_size),
      credits_(initial_credits) {
  // The enhanced flow control mode is not yet supported.
  PW_CHECK(mode_ == kLeCreditBasedFlowControlMode,
           "Credit based flow control mode unsupported: 0x%.2ux",
           EnumValue(mode));

  PW_DCHECK(
      mode != kLeCreditBasedFlowControlMode || max_tx_sdu_size >= kMinLEMTU,
      "Invalid MTU for LE mode: %d",
      max_tx_sdu_size);
  if (max_tx_pdu_payload_size_ < kMinLEMPS) {
    bt_log(DEBUG,
           "l2cap",
           "Peer supplied invalid MPS %u; clamping to %u",
           max_tx_pdu_payload_size_,
           kMinLEMPS);
    max_tx_pdu_payload_size_ = kMinLEMPS;
  }
}

CreditBasedFlowControlTxEngine::~CreditBasedFlowControlTxEngine() = default;

void CreditBasedFlowControlTxEngine::NotifySduQueued() { ProcessSdus(); }

bool CreditBasedFlowControlTxEngine::AddCredits(uint16_t credits) {
  if (static_cast<uint32_t>(credits_) + credits > kMaxCredits)
    return false;

  credits_ += credits;

  // If there are queued SDUs, use the newly added credits to send them.
  ProcessSdus();
  return true;
}

uint16_t CreditBasedFlowControlTxEngine::credits() const { return credits_; }

size_t CreditBasedFlowControlTxEngine::segments_count() const {
  return segments_.size();
}

void CreditBasedFlowControlTxEngine::SegmentSdu(ByteBufferPtr sdu) {
  size_t data_remaining = kSduHeaderSize + sdu->size();

  // In credit based flow control segmentation, the first PDU contains an SDU
  // Length header + a payload of maximum size |max_tx_pdu_payload_size_|, and
  // subsequent PDUs only contain payloads of maximum size
  // |max_tx_pdu_payload_size_|. This loop iterates over each potential packet
  // (and will only execute once if the entire SDU fits in a single PDU).
  do {
    std::unique_ptr<DynamicByteBuffer> segment;
    MutableBufferView payload_view;

    if (data_remaining == kSduHeaderSize + sdu->size()) {
      data_remaining -= kSduHeaderSize;
      segment = std::make_unique<DynamicByteBuffer>(
          kSduHeaderSize +
          std::min<size_t>(data_remaining, max_tx_pdu_payload_size_));
      payload_view = segment->mutable_view(kSduHeaderSize);

      // First frame of the SDU, write the SDU header.
      emboss::KFrameSduHeaderWriter header(segment->mutable_data(),
                                           kSduHeaderSize);
      header.sdu_length().Write(sdu->size());
    } else {
      segment = std::make_unique<DynamicByteBuffer>(
          std::min<size_t>(data_remaining, max_tx_pdu_payload_size_));
      payload_view = segment->mutable_view();
    }

    sdu->Copy(&payload_view, sdu->size() - data_remaining, payload_view.size());
    data_remaining -= payload_view.size();

    segments_.push_back(std::move(segment));
  } while (data_remaining > 0);
}

void CreditBasedFlowControlTxEngine::TrySendSegments() {
  while (credits_ > 0 && !segments_.empty()) {
    ByteBufferPtr segment = std::move(segments_.front());
    // Ordering is important here. We must remove the segment from the queue
    // before notifying the channel so the channel can check whether the segment
    // queue is empty now.
    segments_.pop_front();
    channel().SendFrame(std::move(segment));
    --credits_;
  }
}

void CreditBasedFlowControlTxEngine::ProcessSdus() {
  TrySendSegments();
  while (credits_ > 0) {
    std::optional<ByteBufferPtr> sdu = channel().GetNextQueuedSdu();

    if (!sdu)
      break;
    PW_CHECK(*sdu);

    if ((*sdu)->size() > max_tx_sdu_size()) {
      bt_log(INFO,
             "l2cap",
             "SDU size exceeds channel TxMTU (channel-id: 0x%.4x)",
             channel_id());
      return;
    }

    SegmentSdu(std::move(*sdu));
    TrySendSegments();
  }
}

}  // namespace bt::l2cap::internal
