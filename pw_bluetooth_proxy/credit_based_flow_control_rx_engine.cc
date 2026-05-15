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

#include "pw_bluetooth_proxy/internal/credit_based_flow_control_rx_engine.h"

#include <cmath>

#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_log/log.h"

namespace pw::bluetooth::proxy::internal {

namespace {

// TODO: b/353734827 - Allow client to determine this constant.
const float kRxCreditReplenishThreshold = 0.30f;

}  // namespace

RxEngine::HandlePduFromControllerReturnValue
CreditBasedFlowControlRxEngine::HandlePduFromController(
    pw::span<uint8_t> frame) {
  rx_remaining_credits_--;

  uint16_t rx_credits_used = rx_total_credits_ - rx_remaining_credits_;
  if (rx_credits_used >=
      std::ceil(rx_total_credits_ * kRxCreditReplenishThreshold)) {
    Status status = replenish_rx_credits_fn_(rx_credits_used);
    if (status.IsUnavailable()) {
      PW_LOG_INFO(
          "Unable to send %hu rx credits to remote (it has %hu credits "
          "remaining). Will try on next PDU receive.",
          rx_credits_used,
          rx_total_credits_);
    } else if (status.IsFailedPrecondition()) {
      PW_LOG_WARN(
          "Unable to send rx credits to remote, perhaps the connection has "
          "been closed?");
    } else {
      PW_CHECK(status.ok());
      rx_remaining_credits_ += rx_credits_used;
    }
  }

  ConstByteSpan kframe_payload;
  if (rx_sdu_bytes_remaining_ > 0) {
    // Received PDU that is part of current SDU being assembled.
    Result<emboss::SubsequentKFrameView> subsequent_kframe_view =
        MakeEmbossView<emboss::SubsequentKFrameView>(frame);
    // Lower layers should not (and cannot) invoke this callback on a packet
    // with an incomplete basic L2CAP header.
    PW_CHECK_OK(subsequent_kframe_view);

    // Core Spec v6.0 Vol 3, Part A, 3.4.3: "If the payload size of any K-frame
    // exceeds the receiver's MPS, the receiver shall disconnect the channel."
    uint16_t payload_size =
        static_cast<uint16_t>(subsequent_kframe_view->payload_size().Read());
    if (payload_size > rx_mps_) {
      PW_LOG_ERROR(
          "(CID %#x) Rx K-frame payload exceeds MPS. So stopping channel & "
          "reporting it needs to be closed.",
          local_cid_);
      return L2capChannelEvent::kRxInvalid;
    }

    kframe_payload = as_bytes(span(
        subsequent_kframe_view->payload().BackingStorage().data(),
        static_cast<size_t>(subsequent_kframe_view->payload_size().Read())));
  } else {
    // Received first (or only) PDU of SDU.
    Result<emboss::FirstKFrameView> first_kframe_view =
        MakeEmbossView<emboss::FirstKFrameView>(frame);
    if (!first_kframe_view.ok()) {
      PW_LOG_ERROR(
          "(CID %#x) Buffer is too small for first K-frame. So stopping "
          "channel and reporting it needs to be closed.",
          local_cid_);
      return L2capChannelEvent::kRxInvalid;
    }

    rx_sdu_bytes_remaining_ =
        static_cast<uint16_t>(first_kframe_view->sdu_length().Read());

    // Core Spec v6.0 Vol 3, Part A, 3.4.3: "If the SDU length field value
    // exceeds the receiver's MTU, the receiver shall disconnect the channel."
    if (rx_sdu_bytes_remaining_ > rx_mtu_) {
      PW_LOG_ERROR(
          "(CID %#x) Rx K-frame SDU exceeds MTU. So stopping channel & "
          "reporting it needs to be closed.",
          local_cid_);
      return L2capChannelEvent::kRxInvalid;
    }

    // Core Spec v6.0 Vol 3, Part A, 3.4.3: "If the payload size of any K-frame
    // exceeds the receiver's MPS, the receiver shall disconnect the channel."
    uint16_t payload_size =
        static_cast<uint16_t>(first_kframe_view->payload_size().Read());
    if (payload_size > rx_mps_) {
      PW_LOG_ERROR(
          "(CID %#x) Rx K-frame payload exceeds MPS. So stopping channel & "
          "reporting it needs to be closed.",
          local_cid_);
      return L2capChannelEvent::kRxInvalid;
    }

    rx_sdu_ =
        rx_multibuf_allocator_->AllocateContiguous(rx_sdu_bytes_remaining_);
    if (!rx_sdu_) {
      PW_LOG_ERROR(
          "(CID %#x) Rx MultiBuf allocator out of memory. So stopping channel "
          "and reporting it needs to be closed.",
          local_cid_);
      return L2capChannelEvent::kRxOutOfMemory;
    }

    kframe_payload = as_bytes(
        span(first_kframe_view->payload().BackingStorage().data(),
             static_cast<size_t>(first_kframe_view->payload_size().Read())));
  }

  // Copy segment into rx_sdu_.
  size_t copied = rx_sdu_->CopyFrom(kframe_payload, rx_sdu_offset_).size();
  if (copied < kframe_payload.size()) {
    // Core Spec v6.0 Vol 3, Part A, 3.4.3: "If the sum of the payload sizes
    // for the K-frames exceeds the specified SDU length, the receiver shall
    // disconnect the channel."
    PW_LOG_ERROR(
        "(CID %#x) Sum of K-frame payload sizes exceeds the specified SDU "
        "length. So stopping channel and reporting it needs to be closed.",
        local_cid_);
    return L2capChannelEvent::kRxInvalid;
  }

  rx_sdu_bytes_remaining_ -= kframe_payload.size();
  rx_sdu_offset_ += kframe_payload.size();

  if (rx_sdu_bytes_remaining_ == 0) {
    // We have a full SDU, so invoke client callback.
    multibuf::MultiBuf send_to_client = std::move(rx_sdu_.value());
    rx_sdu_ = std::nullopt;
    rx_sdu_offset_ = 0;
    return send_to_client;
  }

  return std::monostate();
}

Status CreditBasedFlowControlRxEngine::AddRxCredits(
    uint16_t additional_rx_credits) {
  // We treat additional bumps from the client as bumping the total allowed
  // credits.
  rx_total_credits_ += additional_rx_credits;
  rx_remaining_credits_ += additional_rx_credits;
  PW_LOG_INFO(
      "btproxy: CreditBasedFlowControlRxEngine::AddRxCredits -  "
      "additional_rx_credits: %u, rx_total_credits_: %u, "
      "rx_remaining_credits_: %u",
      additional_rx_credits,
      rx_total_credits_,
      rx_remaining_credits_);
  return OkStatus();
}

}  // namespace pw::bluetooth::proxy::internal
