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

#include "pw_bluetooth_sapphire/internal/host/l2cap/enhanced_retransmission_mode_tx_engine.h"

#include <pw_assert/check.h>

#include <limits>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/frame_headers.h"

namespace bt::l2cap::internal {

using Engine = EnhancedRetransmissionModeTxEngine;

namespace {

// Returns the number of frames within the range from |low| to |high|, inclusive
// of |low|, but exclusive of |high|. Returns zero if |low == high|.
uint8_t NumFramesBetween(uint8_t low, uint8_t high) {
  if (high < low) {
    high += (EnhancedControlField::kMaxSeqNum + 1);
  }
  return high - low;
}

}  // namespace

Engine::EnhancedRetransmissionModeTxEngine(
    ChannelId channel_id,
    uint16_t max_tx_sdu_size,
    uint8_t max_transmissions,
    uint8_t n_frames_in_tx_window,
    TxChannel& channel,
    ConnectionFailureCallback connection_failure_callback,
    pw::async::Dispatcher& dispatcher)
    : TxEngine(channel_id, max_tx_sdu_size, channel),
      pw_dispatcher_(dispatcher),
      max_transmissions_(max_transmissions),
      n_frames_in_tx_window_(n_frames_in_tx_window),
      connection_failure_callback_(std::move(connection_failure_callback)),
      expected_ack_seq_(0),
      next_tx_seq_(0),
      last_tx_seq_(0),
      req_seqnum_(0),
      retransmitted_range_during_poll_(false),
      n_receiver_ready_polls_sent_(0),
      remote_is_busy_(false) {
  PW_DCHECK(n_frames_in_tx_window_);
  receiver_ready_poll_task_.set_function(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok()) {
          return;
        }
        SendReceiverReadyPoll();
        StartMonitorTimer();
      });
  monitor_task_.set_function(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (!status.ok()) {
          return;
        }
        if (max_transmissions_ == 0 ||
            n_receiver_ready_polls_sent_ < max_transmissions_) {
          SendReceiverReadyPoll();
          StartMonitorTimer();
        } else {
          connection_failure_callback_();  // May invalidate |self|.
        }
      });
}

void Engine::NotifySduQueued() {
  // Bound |pending_pdus_| to avoid unbounded heap growth when the peer stalls
  // us (RNR or MaxTransmit=0). Leaving the SDU in the channel queue lets
  // ChannelImpl::Send()'s |max_tx_queued| cap apply back-pressure to the
  // upper layer. 2x the tx window gives ample buffering above what can be
  // in-flight.
  const size_t kMaxPendingPdus =
      2u * static_cast<size_t>(n_frames_in_tx_window_);
  while (pending_pdus_.size() < kMaxPendingPdus) {
    std::optional<ByteBufferPtr> sdu = channel().GetNextQueuedSdu();
    if (!sdu) {
      break;
    }
    ProcessSdu(std::move(*sdu));
  }
}

bool Engine::AddCredits(uint16_t) {
  bt_log(WARN, "l2cap", "attempt to add credits to ERTM TxEngine");
  return false;
}

void Engine::ProcessSdu(ByteBufferPtr sdu) {
  PW_CHECK(sdu);
  // TODO(fxbug.dev/42054330): Add support for segmentation
  if (sdu->size() > max_tx_sdu_size()) {
    bt_log(INFO,
           "l2cap",
           "SDU size exceeds channel TxMTU (channel-id: %#.4x)",
           channel_id());
    return;
  }

  const auto seq_num = GetNextTxSeq();
  SimpleInformationFrameHeader header(seq_num);
  DynamicByteBuffer frame(sizeof(header) + sdu->size());
  auto body = frame.mutable_view(sizeof(header));
  frame.WriteObj(header);
  sdu->Copy(&body);

  // TODO(fxbug.dev/42086227): Limit the size of the queue.
  pending_pdus_.push_back(std::move(frame));
  MaybeSendQueuedData();
}

void Engine::UpdateAckSeq(uint8_t new_seq, bool is_poll_response) {
  // TODO(quiche): Reconsider this assertion if we allow reconfiguration of the
  // TX window.
  PW_DCHECK(NumUnackedFrames() <= n_frames_in_tx_window_,
            "(NumUnackedFrames() = %u, n_frames_in_tx_window_ = %u, "
            "expected_ack_seq_ = %u, last_tx_seq_ = %u)",
            NumUnackedFrames(),
            n_frames_in_tx_window_,
            expected_ack_seq_,
            last_tx_seq_);

  const auto n_frames_acked = NumFramesBetween(expected_ack_seq_, new_seq);
  if (n_frames_acked > NumUnackedFrames()) {
    // Peer acknowledgment of our outbound data (ReqSeq) exceeds the sequence
    // numbers of yet-acknowledged data that we've sent to that peer. See
    // conditions "With-Invalid-ReqSeq" and "With-Invalid-ReqSeq-Retrans" in
    // Core Spec v5.0 Vol 3 Part A Sec 8.6.5.5.
    bt_log(WARN,
           "l2cap",
           "Received acknowledgment for %hhu frames but only %hhu frames are "
           "pending",
           n_frames_acked,
           NumUnackedFrames());
    connection_failure_callback_();  // May invalidate |self|.
    return;
  }

  // Perform Stop-MonitorTimer when "F = 1," per Core Spec v5.0, Vol 3, Part A,
  // Sec 8.6.5.7–8.
  if (is_poll_response) {
    monitor_task_.Cancel();
  }

  PW_CHECK(!(range_request_.has_value() && single_request_.has_value()));
  if (ProcessSingleRetransmitRequest(new_seq, is_poll_response) ==
      UpdateAckSeqAction::kConsumeAckSeq) {
    return;
  }

  auto n_frames_to_discard = n_frames_acked;
  while (n_frames_to_discard) {
    PW_DCHECK(!pending_pdus_.empty());
    pending_pdus_.pop_front();
    --n_frames_to_discard;
  }

  expected_ack_seq_ = new_seq;
  if (expected_ack_seq_ == next_tx_seq_) {
    receiver_ready_poll_task_.Cancel();
  }

  const auto range_request = std::exchange(range_request_, std::nullopt);

  // RemoteBusy is cleared as the first action to take when receiving a REJ per
  // Core Spec v5.0 Vol 3, Part A, Sec 8.6.5.9–11, so their corresponding member
  // variables shouldn't be both set.
  PW_CHECK(!(range_request.has_value() && remote_is_busy_));
  bool should_retransmit = range_request.has_value();

  // This implements the logic for RejActioned in the Recv {I,RR,REJ} (F=1)
  // event for all of the receiver states (Core Spec v5.0 Vol 3, Part A,
  // Sec 8.6.5.9–11).
  if (is_poll_response) {
    if (retransmitted_range_during_poll_) {
      should_retransmit = false;
      retransmitted_range_during_poll_ = false;
    } else {
      should_retransmit = true;
    }
  }

  if (remote_is_busy_) {
    return;
  }

  // This implements the logic for PbitOutstanding in the Recv REJ (F=0) event
  // for all of the receiver states (Core Spec v5.0 Vol 3, Part A,
  // Sec 8.6.5.9–11).
  if (range_request.has_value() && !is_poll_response &&
      monitor_task_.is_pending()) {
    retransmitted_range_during_poll_ = true;
  }

  if (should_retransmit) {
    const bool set_is_poll_response =
        range_request.value_or(RangeRetransmitRequest{}).is_poll_request;
    if (!RetransmitUnackedData(std::nullopt, set_is_poll_response)) {
      return;
    }
  }

  NotifySduQueued();
  MaybeSendQueuedData();

  // TODO(quiche): Restart the receiver_ready_poll_task_, if there's any
  // remaining unacknowledged data.
}

void Engine::UpdateReqSeq(uint8_t new_seq) { req_seqnum_ = new_seq; }

void Engine::ClearRemoteBusy() {
  // TODO(quiche): Maybe clear backpressure on the Channel (subject to TxWindow
  // contraints).
  remote_is_busy_ = false;
}

void Engine::SetRemoteBusy() {
  // TODO(fxbug.dev/42086338): Signal backpressure to the Channel.
  remote_is_busy_ = true;
  receiver_ready_poll_task_.Cancel();
}

void Engine::SetSingleRetransmit(bool is_poll_request) {
  PW_CHECK(!single_request_.has_value());
  PW_CHECK(!range_request_.has_value());
  // Store SREJ state for UpdateAckSeq to handle.
  single_request_ = SingleRetransmitRequest{.is_poll_request = is_poll_request};
}

void Engine::SetRangeRetransmit(bool is_poll_request) {
  PW_CHECK(!single_request_.has_value());
  PW_CHECK(!range_request_.has_value());
  // Store REJ state for UpdateAckSeq to handle.
  range_request_ = RangeRetransmitRequest{.is_poll_request = is_poll_request};
}

void Engine::MaybeSendQueuedData() {
  if (remote_is_busy_ || monitor_task_.is_pending()) {
    return;
  }

  // Find the first PDU that has not already been transmitted (if any).
  // * This is not necessarily the first PDU, because that may have been
  //   transmited already, and is just pending acknowledgement.
  // * This is not necessarily the last PDU, because earlier PDUs may have been
  //   queued without having been sent over-the-air (due, e.g., to tx_window
  //   constraints).
  //
  // TODO(quiche): Consider if there's a way to do this that isn't O(n).
  auto it = std::find_if(
      pending_pdus_.begin(), pending_pdus_.end(), [](const auto& pending_pdu) {
        return pending_pdu.tx_count == 0;
      });

  while (it != pending_pdus_.end() &&
         NumUnackedFrames() < n_frames_in_tx_window_) {
    PW_DCHECK(it->tx_count == 0);
    SendPdu(&*it);
    last_tx_seq_ = it->buf.To<SimpleInformationFrameHeader>().tx_seq();
    ++it;
  }
}

Engine::UpdateAckSeqAction Engine::ProcessSingleRetransmitRequest(
    uint8_t new_seq, bool is_poll_response) {
  const auto single_request = std::exchange(single_request_, std::nullopt);
  PW_CHECK(!(single_request.has_value() && remote_is_busy_));
  if (!single_request.has_value()) {
    return UpdateAckSeqAction::kDiscardAcknowledged;
  }

  // This implements the logic for SrejActioned=TRUE in the Recv SREJ (P=0)
  // (F=1) event for all of the receiver states (Core Spec v5.0 Vol 3, Part A,
  // Sec 8.6.5.9–11).
  if (is_poll_response && retransmitted_single_during_poll_.has_value() &&
      new_seq == *retransmitted_single_during_poll_) {
    // Only a SREJ (F=1) with a matching AckSeq can clear this state, so if this
    // duplicate suppression isn't performed it sticks around even when we sent
    // the next poll request. AckSeq is modulo kMaxSeqNum + 1 so it'll roll over
    // and potentially suppress retransmission of a different (non-duplicate)
    // packet in a later poll request-response cycle. Unfortunately this isn't
    // fixed as of Core Spec v5.2 so it's implemented as written.
    retransmitted_single_during_poll_.reset();

    // Return early to suppress duplicate retransmission.
    return UpdateAckSeqAction::kConsumeAckSeq;
  }

  // This implements the logic for PbitOutstanding in the Recv SREJ (P=0) (F=0)
  // and Recv SREJ(P=1) events for all of the receiver states (Core Spec v5.0
  // Vol 3, Part A, Sec 8.6.5.9–11).
  if (!is_poll_response && monitor_task_.is_pending()) {
    retransmitted_single_during_poll_ = new_seq;
  }

  if (!RetransmitUnackedData(new_seq, single_request->is_poll_request)) {
    return UpdateAckSeqAction::kConsumeAckSeq;
  }

  // Only "single requests" that are poll requests acknowledge previous I-Frames
  // and cause initial transmission of queued SDUs, per Core Spec v5.0, Vol 3,
  // Part A, Sec 8.6.1.4.
  if (single_request->is_poll_request) {
    return UpdateAckSeqAction::kDiscardAcknowledged;
  }
  return UpdateAckSeqAction::kConsumeAckSeq;
}

void Engine::StartReceiverReadyPollTimer() {
  PW_DCHECK(!monitor_task_.is_pending());
  n_receiver_ready_polls_sent_ = 0;
  receiver_ready_poll_task_.Cancel();
  receiver_ready_poll_task_.PostAfter(kErtmReceiverReadyPollTimerDuration);
}

void Engine::StartMonitorTimer() {
  PW_DCHECK(!receiver_ready_poll_task_.is_pending());
  monitor_task_.Cancel();
  monitor_task_.PostAfter(kErtmMonitorTimerDuration);
}

void Engine::SendReceiverReadyPoll() {
  SimpleReceiverReadyFrame frame;
  frame.set_receive_seq_num(req_seqnum_);
  frame.set_is_poll_request();
  ++n_receiver_ready_polls_sent_;
  PW_CHECK(max_transmissions_ == 0 ||
               n_receiver_ready_polls_sent_ <= max_transmissions_,
           "(n_receiver_ready_polls_sent_ = %u, "
           "max_transmissions = %u)",
           n_receiver_ready_polls_sent_,
           max_transmissions_);
  channel().SendFrame(
      std::make_unique<DynamicByteBuffer>(BufferView(&frame, sizeof(frame))));
}

uint8_t Engine::GetNextTxSeq() {
  auto ret = next_tx_seq_;
  ++next_tx_seq_;
  if (next_tx_seq_ > EnhancedControlField::kMaxSeqNum) {
    next_tx_seq_ = 0;
  }
  return ret;
}

uint8_t Engine::NumUnackedFrames() {
  if (pending_pdus_.empty()) {
    // Initially, |ack_seqnum_ == last_tx_seq_ == 0|, but the number of
    // unacknowledged frames is 0, not 1.
    return 0;
  } else if (pending_pdus_.front().tx_count == 0) {
    // While we have some data queued, none of that data has been sent
    // over-the-air. This might happen, e.g., transiently in QueueSdu().
    return 0;
  } else {
    // Having ascertained that some data _is_ in flight, the number of frames in
    // flight is given by the expression below.
    return NumFramesBetween(
        expected_ack_seq_,
        last_tx_seq_ + 1  // Include frame with |last_tx_seq_| in count
    );
  }
}

void Engine::SendPdu(PendingPdu* pdu) {
  PW_DCHECK(pdu);
  pdu->buf.AsMutable<SimpleInformationFrameHeader>()->set_receive_seq_num(
      req_seqnum_);

  // Prevent tx_count from overflowing to zero, as that would be
  // indistinguishable from "never transmitted." This is only possible when
  // configured for infinite retransmissions, so there is no benefit to having
  // an accurate tx_count after each frame's initial transmission.
  if (pdu->tx_count != std::numeric_limits<decltype(pdu->tx_count)>::max()) {
    pdu->tx_count++;
  }
  StartReceiverReadyPollTimer();
  channel().SendFrame(std::make_unique<DynamicByteBuffer>(pdu->buf));
}

bool Engine::RetransmitUnackedData(std::optional<uint8_t> only_with_seq,
                                   bool set_is_poll_response) {
  // The receive engine should have cleared the remote busy condition before
  // calling any method that would cause us (the transmit engine) to retransmit
  // unacked data. See, e.g., Core Spec v5.0, Volume 3, Part A, Table 8.6, row
  // "Recv REJ (F=0)".
  PW_DCHECK(!remote_is_busy_);

  // Any peer actions that cause retransmission indicate the peer is alive. This
  // is in conflict with Core Spec v5.0, Vol 3, Part A, Sec 8.6.5.8, which only
  // stops the MonitorTimer when a poll response is received and omits what to
  // do when REJ or SREJ cause retransmission. However, this behavior of
  // canceling the MonitorTimer "early" is in line with Sequence Diagram
  // Fig 4.94, L2CAP Test Spec v5.0.2 Section 4.9.7.24, among others that show
  // REJ or SREJ causing the receiver to cancel its MonitorTimer. We follow the
  // latter behavior because (1) it's less ambiguous (2) it makes sense and (3)
  // we need to pass those tests.
  monitor_task_.Cancel();

  const auto n_to_send = NumUnackedFrames();
  PW_CHECK(n_to_send <= n_frames_in_tx_window_);
  PW_DCHECK(n_to_send <= pending_pdus_.size());

  auto cur_frame = pending_pdus_.begin();
  auto last_frame = std::next(cur_frame, n_to_send);
  for (; cur_frame != last_frame; cur_frame++) {
    PW_DCHECK(cur_frame != pending_pdus_.end());

    const auto control_field =
        cur_frame->buf.To<SimpleInformationFrameHeader>();
    if (only_with_seq.has_value() && control_field.tx_seq() != *only_with_seq) {
      continue;
    }

    // Core Spec v5.0, Vol 3, Part A, Sec 5.4: "In Enhanced Retransmission mode
    // a value of zero for MaxTransmit means infinite retransmissions."
    if (max_transmissions_ != 0 && cur_frame->tx_count >= max_transmissions_) {
      PW_CHECK(cur_frame->tx_count == max_transmissions_,
               "%hhu != %hhu",
               cur_frame->tx_count,
               max_transmissions_);
      connection_failure_callback_();
      return false;
    }

    if (set_is_poll_response) {
      cur_frame->buf.AsMutable<EnhancedControlField>()->set_is_poll_response();

      // Per "Retransmit-I-frames" of Core Spec v5.0 Vol 3, Part A, Sec 8.6.5.6,
      // "the F-bit of all other [than the first] unacknowledged I-frames sent
      // shall be 0," so clear this for subsequent iterations.
      set_is_poll_response = false;
    }

    // TODO(fxbug.dev/42087625): If the task is already running, we should not
    // restart it.
    SendPdu(&*cur_frame);
    *cur_frame->buf.AsMutable<EnhancedControlField>() = control_field;
  }

  return true;
}

}  // namespace bt::l2cap::internal
