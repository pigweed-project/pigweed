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

#include "pw_bluetooth_sapphire/internal/host/l2cap/enhanced_retransmission_mode_rx_engine.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <optional>
#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"

namespace bt::l2cap::internal {

namespace {

template <typename T>
std::optional<T> TryCopyHeaderFromPdu(const PDU& pdu) {
  if (pdu.length() < sizeof(T) + sizeof(FrameCheckSequence)) {
    return std::nullopt;
  }

  StaticByteBuffer<sizeof(T)> buf;
  pdu.Copy(&buf, 0, sizeof(T));
  return buf.template To<T>();
}

}  // namespace

using Engine = EnhancedRetransmissionModeRxEngine;

Engine::EnhancedRetransmissionModeRxEngine(
    uint16_t max_rx_sdu_size,
    uint16_t max_rx_pdu_payload_size,
    SendFrameCallback send_frame_callback,
    ConnectionFailureCallback connection_failure_callback)
    : max_rx_sdu_size_(max_rx_sdu_size),
      max_rx_pdu_payload_size_(max_rx_pdu_payload_size),
      next_seqnum_(0),
      remote_is_busy_(false),
      send_frame_callback_(std::move(send_frame_callback)),
      connection_failure_callback_(std::move(connection_failure_callback)),
      weak_factory_(this) {}

ByteBufferPtr Engine::ProcessPdu(PDU pdu) {
  PW_CHECK(pdu.is_valid());
  if (failed_) {
    return nullptr;
  }

  // A note on validation (see Core Spec v6.2, Vol 3, Part A, Sec 3.3.7):
  //
  // Step 1 (Channel ID validation) is handled before frames reach `RxEngine`.
  // Step 2 (FCS validation) is not implemented.
  // Step 3 checks frame length bounds and MPS before sequence-number checking
  // or `PassToTx`.

  const auto control_field_opt =
      TryCopyHeaderFromPdu<EnhancedControlField>(pdu);
  if (!control_field_opt) {
    // TODO(fxbug.dev/42080889): Add metric counting truncated frames.
    bt_log(WARN,
           "l2cap",
           "ERTM frame too short (%u); closing channel",
           pdu.length());
    OnFailure();
    return nullptr;
  }

  auto self = weak_factory_.GetWeakPtr();
  ByteBufferPtr sdu;
  if (control_field_opt->designates_supervisory_frame()) {
    ProcessSupervisoryFrame(pdu);
  } else {
    sdu = ProcessInformationFrame(std::move(pdu));
  }

  // This implements the `PassToTx` action ("Pass the ReqSeq and F-bit value")
  // per Core Spec v6.2, Vol 3, Part A, Sec 8.6.5.6 and must come after updates
  // to the `RemoteBusy` variable in order to avoid transmitting frames when the
  // peer can't accept them.
  if (self.is_alive() && !self->failed_ && self->receive_seq_num_callback_) {
    auto cb = std::move(self->receive_seq_num_callback_);
    cb(control_field_opt->receive_seq_num(),
       control_field_opt->is_poll_response());
    if (self.is_alive()) {
      self->receive_seq_num_callback_ = std::move(cb);
    }
  }
  return sdu;
}

ByteBufferPtr Engine::ProcessInformationFrame(PDU pdu) {
  const auto header_opt =
      TryCopyHeaderFromPdu<SimpleInformationFrameHeader>(pdu);
  PW_CHECK(header_opt.has_value());
  const SimpleInformationFrameHeader header = *header_opt;

  const size_t min_non_payload_len =
      (header.designates_start_of_segmented_sdu()
           ? sizeof(SimpleStartOfSduFrameHeader)
           : sizeof(SimpleInformationFrameHeader)) +
      sizeof(FrameCheckSequence);
  // `ProcessPdu` already verified the minimum I-frame length; the longer
  // Start-of-SDU header minimum is checked after `tx_seq` validation per Core
  // Spec v6.2, Vol 3, Part A, Sec 3.3.7 Step 4.
  if (pdu.length() > min_non_payload_len &&
      pdu.length() - min_non_payload_len > max_rx_pdu_payload_size_) {
    // TODO(fxbug.dev/42080889): Add metric counting oversized frames.
    bt_log(WARN,
           "l2cap",
           "ERTM I-frame payload (%zu) exceeds MPS (%u); closing channel",
           pdu.length() - min_non_payload_len,
           max_rx_pdu_payload_size_);
    OnFailure();
    return nullptr;
  }

  if (header.tx_seq() != next_seqnum_) {
    // TODO(quiche): Send REJ frame.
    // TODO(quiche): Add histogram for |header.tx_seq() - next_seqnum_|. This
    // will give us an upper bound on the potential benefit of sending SREJ
    // frames.
    return nullptr;
  }

  // TODO(quiche): check if the frame is within the permitted window.

  // Core Spec v6.2, Vol 3, Part A, Sec 3.3.7 Steps 4–5: SAR checks (including
  // Start-of-SDU length vs. MTU) run once the I-frame is received in the
  // expected sequence.
  if (header.designates_start_of_segmented_sdu()) {
    const auto start_header_opt =
        TryCopyHeaderFromPdu<SimpleStartOfSduFrameHeader>(pdu);
    if (!start_header_opt) {
      // TODO(fxbug.dev/42080889): Add metric counting truncated Start-of-SDU
      // frames.
      bt_log(WARN,
             "l2cap",
             "ERTM Start-of-SDU frame too short (%u); closing channel",
             pdu.length());
      OnFailure();
      return nullptr;
    }

    const uint16_t sdu_len = pw::bytes::ConvertOrderFrom(
        cpp20::endian::little, start_header_opt->sdu_len);
    if (sdu_len > max_rx_sdu_size_) {
      bt_log(WARN,
             "l2cap",
             "ERTM Start-of-SDU length (%u) exceeds MTU (%u); closing channel",
             sdu_len,
             max_rx_sdu_size_);
      OnFailure();
      return nullptr;
    }

    // TODO(quiche): Implement validation and handling of segmented frames.
    return nullptr;
  }

  if (header.designates_part_of_segmented_sdu()) {
    bt_log(WARN,
           "l2cap",
           "ERTM segmented I-frame received without Start-of-SDU; closing "
           "channel");
    OnFailure();
    return nullptr;
  }

  PW_CHECK(pdu.length() >= min_non_payload_len);
  const size_t payload_len = pdu.length() - min_non_payload_len;

  // Core Spec v6.2, Vol 3, Part A, Sec 3.3.4: "If SAR=0b00 (Unsegmented L2CAP
  // SDU) then the payload size shall not be greater than the peer device's MTU
  // for the channel."
  if (payload_len > max_rx_sdu_size_) {
    bt_log(WARN,
           "l2cap",
           "ERTM unsegmented SDU length (%zu) exceeds MTU (%u); closing "
           "channel",
           payload_len,
           max_rx_sdu_size_);
    OnFailure();
    return nullptr;
  }

  auto self = weak_factory_.GetWeakPtr();

  AdvanceSeqNum();

  if (ack_seq_num_callback_) {
    ack_seq_num_callback_(next_seqnum_);
  }

  if (!self.is_alive()) {
    return nullptr;
  }

  SimpleReceiverReadyFrame ack_frame;
  ack_frame.set_receive_seq_num(next_seqnum_);
  send_frame_callback_(std::make_unique<DynamicByteBuffer>(
      BufferView(&ack_frame, sizeof(ack_frame))));

  if (!self.is_alive()) {
    return nullptr;
  }

  auto sdu = std::make_unique<DynamicByteBuffer>(payload_len);
  pdu.Copy(sdu.get(), sizeof(header), payload_len);
  return sdu;
}

void Engine::ProcessSupervisoryFrame(const PDU& pdu) {
  if (pdu.length() !=
      sizeof(SimpleSupervisoryFrame) + sizeof(FrameCheckSequence)) {
    // TODO(fxbug.dev/42080889): Add metric counting invalid S-frames.
    bt_log(WARN,
           "l2cap",
           "ERTM S-frame has invalid length (%u); closing channel",
           pdu.length());
    OnFailure();
    return;
  }

  const auto sframe_opt = TryCopyHeaderFromPdu<SimpleSupervisoryFrame>(pdu);
  PW_CHECK(sframe_opt.has_value());
  const SimpleSupervisoryFrame sframe = *sframe_opt;
  // Core Spec v5, Vol 3, Part A, Sec 8.6.1.5: "S-Frames shall not be
  // transmitted with both the F-bit and the P-bit set to 1 at the same time."
  if (sframe.is_poll_request() && sframe.is_poll_response()) {
    OnFailure();
    return;
  }

  auto self = weak_factory_.GetWeakPtr();

  // Signal changes to our RemoteBusy variable per Core Spec v5.0, Vol 3, Part
  // A, Sec 8.6.5.6.
  const bool remote_is_busy =
      sframe.function() == SupervisoryFunction::ReceiverNotReady;
  if (remote_is_busy && !remote_is_busy_) {
    if (remote_busy_set_callback_) {
      remote_busy_set_callback_();
    }
  } else if (!remote_is_busy && remote_is_busy_) {
    if (remote_busy_cleared_callback_) {
      remote_busy_cleared_callback_();
    }
  }

  if (!self.is_alive()) {
    return;
  }

  remote_is_busy_ = remote_is_busy;

  // Implements the "Send RRorRNR (F=1)" action of Core Spec, v5, Vol 3, Part A,
  // Section 8.6.5.9, Table 8.6, "Recv RNR (P=1)" and "Send IorRRorRNR(F=1)"
  // action of "Recv RR(P=1)." In the latter case, responding with an I-Frame
  // (F=1) is indistinguishable from responding with an RR (F=1) then an I-Frame
  // (F=0), so that optimization isn't implemented and we always respond with an
  // RR or RNR (F=1), but not an I-Frame (F=1).
  if (sframe.function() == SupervisoryFunction::ReceiverReady ||
      sframe.function() == SupervisoryFunction::ReceiverNotReady) {
    if (sframe.is_poll_request()) {
      // See Core Spec, v5, Vol 3, Part A, Section 8.6.5.9, Table 8.6, "Recv
      // RR(P=1)".
      //
      // Note, however, that there may be additional work to do if we're in the
      // REJ_SENT state. See Core Spec, v5, Vol 3, Part A, Section 8.6.5.10,
      // Table 8.7, "Recv RR(P=1)".
      //
      // TODO(fxbug.dev/42054996): Respond with RNR when LocalBusy.
      SimpleReceiverReadyFrame poll_response;
      poll_response.set_is_poll_response();
      poll_response.set_receive_seq_num(next_seqnum_);
      send_frame_callback_(std::make_unique<DynamicByteBuffer>(
          BufferView(&poll_response, sizeof(poll_response))));
      return;
    }
  }

  // REJ S-Frames will still result in forwarding the acknowledgment via
  // ReceiveSeqNumCallback after this call, per "PassToTx" actions for "Recv
  // REJ" events in Core Spec v5.0, Vol 3, Part A, Sec 8.6.5.9–11.
  if (sframe.function() == SupervisoryFunction::Reject) {
    if (range_retransmit_set_callback_) {
      range_retransmit_set_callback_(sframe.is_poll_request());
    }
  }

  if (!self.is_alive()) {
    return;
  }

  // SREJ S-Frames will still result in forwarding the acknowledgment via
  // ReceiveSeqNumCallback after this call. The "Recv SREJ" events in Core Spec
  // v5.0, Vol 3, Part A, Sec 8.6.5.9–11 call for different actions ("PassToTx"
  // vs "PassToTxFbit") but we always pass both receive seq and poll response
  // because the TxEngine has other behavior that branch on the same bit.
  if (sframe.function() == SupervisoryFunction::SelectiveReject) {
    if (single_retransmit_set_callback_) {
      single_retransmit_set_callback_(sframe.is_poll_request());
    }
  }
}

void Engine::AdvanceSeqNum() {
  ++next_seqnum_;
  if (next_seqnum_ > EnhancedControlField::kMaxSeqNum) {
    next_seqnum_ = 0;
  }
}

void Engine::OnFailure() {
  failed_ = true;
  if (connection_failure_callback_) {
    auto cb = std::move(connection_failure_callback_);
    cb();
  }
}

}  // namespace bt::l2cap::internal
