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

#pragma once
#include <lib/fit/function.h>

#include <list>

#include "pw_bluetooth_sapphire/internal/host/common/smart_task.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/tx_engine.h"

namespace bt::l2cap::internal {

// Implements the sender-side functionality of L2CAP Enhanced Retransmission
// Mode. See Bluetooth Core Spec v5.0, Volume 3, Part A, Sec 2.4, "Modes of
// Operation".
//
// THREAD-SAFETY: This class may is _not_ thread-safe. In particular:
// * the class assumes that some other party ensures that QueueSdu() is not
//   invoked concurrently with the destructor, and
// * the class assumes that all calls to QueueSdu occur on a single thread,
//   for the entire lifetime of an object.
class EnhancedRetransmissionModeTxEngine final : public TxEngine {
 public:
  using ConnectionFailureCallback = fit::function<void()>;

  // Create a transmit engine.
  //
  // The engine will invoke channel->SendFrame() when a PDU is ready for
  // transmission; see tx_engine.h for further detail.
  //
  // The engine will invoke |connection_failure_callback| when a fatal error
  // occurs on this connection. This callback _may_ occur synchronously. For
  // example, a call to UpdateAckSeq() may synchronously invoke
  // |connection_failure_callback|.
  EnhancedRetransmissionModeTxEngine(
      ChannelId channel_id,
      uint16_t max_tx_sdu_size,
      uint8_t max_transmissions,
      uint8_t n_frames_in_tx_window,
      TxChannel& channel,
      ConnectionFailureCallback connection_failure_callback,
      pw::async::Dispatcher& dispatcher);
  ~EnhancedRetransmissionModeTxEngine() override = default;

  void NotifySduQueued() override;

  bool AddCredits(uint16_t credits) override;

  bool IsQueueEmpty() override { return pending_pdus_.empty(); }

  // Updates the Engine's knowledge of the last frame acknowledged by our peer.
  // The value of |is_poll_response| should reflect the 'F' bit in header of the
  // frame which led to this call.
  //
  // * This _may_ trigger retransmission of previously transmitted data.
  // * This _may_ cause the (initial) transmission of queued data.
  void UpdateAckSeq(uint8_t new_seq, bool is_poll_response);

  // Updates the Engine's knowledge of the next frame we expect to receive from
  // our peer.
  void UpdateReqSeq(uint8_t new_seq);

  // Informs the Engine that the peer is able to receive frames.
  void ClearRemoteBusy();

  // Informs the Engine that the peer is unable to receive additional frames at
  // this time.
  void SetRemoteBusy();

  // Requests that the next UpdateAckSeq always retransmit the frame identified
  // by its |new_seq| parameter, even if it's not a poll response. This path
  // fulfills the requirements of receiving the SREJ function.
  //
  // Only one of Set{Single,Range}Retransmit can be called before each
  // UpdateAckSeq.
  //
  // |is_poll_request| should be set if the request to retransmit has its P bit
  // set and will cause the first retransmitted frame to have its
  // |is_poll_response| bit (F bit) set per the Retransmit-Requested-I-frame
  // action in Core Spec v5.0 Vol 3, Part A, Sec 8.6.5.6.
  //
  // If |is_poll_request| is false, UpdateAckSeq will be inhibited from dropping
  // the unacked packets from ExpectedAckSeq to |new_seq| because this action is
  // used to selectively retransmit missing frames that precede frames that the
  // peer has already received.
  //
  // ClearRemoteBusy should be called if necessary because RemoteBusy is cleared
  // as the first action to take when receiving an SREJ per Core Spec v5.0 Vol
  // 3, Part A, Sec 8.6.5.9–11.
  void SetSingleRetransmit(bool is_poll_request);

  // Requests that the next UpdateAckSeq always retransmit starting at its
  // |new_seq| parameter, even if it's not a poll response. This path fulfills
  // the requirements of receiving the REJ function.
  //
  // Only one of Set{Single,Range}Retransmit can be called before each
  // UpdateAckSeq.
  //
  // |is_poll_request| should be set if the request to retransmit has its P bit
  // set and will cause the first retransmitted frame to have its
  // |is_poll_response| bit (F bit) set per the Retransmit-I-frames action in
  // Core Spec v5.0 Vol 3, Part A, Sec 8.6.5.6.
  //
  // ClearRemoteBusy should be called if necessary because RemoteBusy is cleared
  // as the first action to take when receiving a REJ per Core Spec v5.0 Vol 3,
  // Part A, Sec 8.6.5.9–11.
  void SetRangeRetransmit(bool is_poll_request);

  // Transmits data that has been queued, but which has never been previously
  // sent to our peer. The transmissions are subject to remote-busy and transmit
  // window constraints.
  void MaybeSendQueuedData();

 private:
  struct PendingPdu {
    PendingPdu(DynamicByteBuffer buf_in)
        : buf(std::move(buf_in)), tx_count(0) {}
    DynamicByteBuffer buf;
    uint8_t tx_count;
  };

  // State of a request from a peer to retransmit a frame of unacked data (SREJ
  // per Core Spec v5.0, Vol 3, Part A, Sec 8.6.1.4).
  struct SingleRetransmitRequest {
    // True if the request to retransmit has its P bit set.
    bool is_poll_request;
  };

  // State of a request from a peer to retransmit a range of unacked data (REJ
  // per Core Spec v5.0, Vol 3, Part A, Sec 8.6.1.2).
  struct RangeRetransmitRequest {
    // True if the request to retransmit has its P bit set.
    bool is_poll_request;
  };

  // Actions to take with a new AckSeq from the peer.
  enum class UpdateAckSeqAction {
    kConsumeAckSeq,        // Done with the received AckSeq.
    kDiscardAcknowledged,  // Discard frames prior to AckSeq then send pending
                           // frames.
  };

  // Called from UpdateAckSeq to check if a SingleRetransmitRequest is pending.
  // Retransmits if necessary and returns the action to take with |new_seq|.
  //
  // This call clears |single_request_| if set, so it is not idempotent.
  UpdateAckSeqAction ProcessSingleRetransmitRequest(uint8_t new_seq,
                                                    bool is_poll_response);

  // Starts the receiver ready poll timer. If already running, the existing
  // timer is cancelled, and a new timer is started.
  // Notes:
  // * The specification refers to this as the "retransmission timer". However,
  //   the expiry of this timer doesn't immediately trigger
  //   retransmission. Rather, the timer expiration triggers us asking the
  //   receiver to acknowledge all previously received data. See
  //   "RetransTimer-Expires", in Core Spec v5.0, Volume 3, Part A, Table 8.4.
  // * Replacing the existing timer is required per Core Spec v5.0, Volume 3,
  //   Part A, Section 8.6.5.6, "Start-RetransTimer".
  void StartReceiverReadyPollTimer();

  // Starts the monitor timer.  If already running, the existing timer is
  // cancelled, and a new timer is started.
  //
  // Note that replacing the existing timer is required per Core Spec v5.0,
  // Volume 3, Part A, Section 8.6.5.6, "Start-MonitorTimer".
  void StartMonitorTimer();

  void SendReceiverReadyPoll();

  // Return and consume the next sequence number.
  uint8_t GetNextTxSeq();

  // Returns the number of frames that have been transmitted but not yet
  // acknowledged.
  uint8_t NumUnackedFrames();

  void SendPdu(PendingPdu* pdu);

  // Retransmits frames from |pending_pdus_|. Invokes
  // |connection_failure_callback_| on error and returns false. Cancels
  // |monitor_task_| if it's running.
  //
  // If |only_with_seq| is set, then only the unacked frame with that TxSeq will
  // be retransmitted.
  //
  // If |set_is_poll_response| is true, then the first frame to be sent will
  // have its |is_poll_response| field set.
  //
  // Notes:
  // * The caller must ensure that |!remote_is_busy_|.
  // * When return value is an error, |this| may be invalid.
  [[nodiscard]] bool RetransmitUnackedData(std::optional<uint8_t> only_with_seq,
                                           bool set_is_poll_response);

  // Process a new SDU from the channel queue.
  void ProcessSdu(ByteBufferPtr sdu);

  pw::async::Dispatcher& pw_dispatcher_;

  const uint8_t max_transmissions_;
  const uint8_t n_frames_in_tx_window_;

  // Invoked when the connection encounters a fatal error.
  ConnectionFailureCallback connection_failure_callback_;

  // The sequence number we expect in the next acknowledgement from our peer.
  //
  // We assume that the Extended Window Size option is _not_ enabled. In such
  // cases, the sequence number is a 6-bit counter that wraps on overflow. See
  // Core Spec v5.0, Vol 3, Part A, Secs 5.7 and 8.3.
  uint8_t expected_ack_seq_;

  // The sequence number we will use for the next new outbound I-frame.
  //
  // We assume that the Extended Window Size option is _not_ enabled. In such
  // cases, the sequence number is a 6-bit counter that wraps on overflow. See
  // Core Spec v5.0, Vol 3, Part A, Secs 5.7 and 8.3.
  uint8_t next_tx_seq_;

  // The sequence number of the "newest" transmitted frame.
  //
  // This sequence number is updated when a new frame is transmitted. This
  // excludes cases where a frame is queued but not transmitted (due to transmit
  // window limitations), and cases where a frame is retransmitted.
  //
  // This value is useful for determining the number of frames than are
  // in-flight to our peer (frames that have been transmitted but not
  // acknowledged).
  //
  // We assume that the Extended Window Size option is _not_ enabled. In such
  // cases, the sequence number is a 6-bit counter that wraps on overflow. See
  // Core Spec v5.0, Vol 3, Part A, Secs 5.7 and 8.3.
  uint8_t last_tx_seq_;  // (AKA TxSeq)

  // The sequence number we expect for the next packet sent _to_ us.
  //
  // We assume that the Extended Window Size option is _not_ enabled. In such
  // cases, the sequence number is a 6-bit counter that wraps on overflow. See
  // Core Spec v5.0, Vol 3, Part A, Secs 5.7 and 8.3.
  uint8_t req_seqnum_;

  // Filled by SetSingleRetransmit and cleared by UpdateAckSeq.
  std::optional<SingleRetransmitRequest> single_request_;

  // Filled by SetRangeRetransmit and cleared by UpdateAckSeq.
  std::optional<RangeRetransmitRequest> range_request_;

  // Set to AckSeq of the most recent frame retransmitted after sending a poll
  // request but before receiving a poll response. Corresponds to the
  // SrejActioned and SrejSaveReqSeq variables defined in Core Spec v5.0, Vol 3,
  // Part A, Sec 8.6.5.3.
  std::optional<uint8_t> retransmitted_single_during_poll_;

  // True if we retransmitted a range of unacked data after sending a poll
  // request but before receiving a poll response. Corresponds to the
  // RejActioned variable defined in Core Spec v5.0 Vol 3, Part A, Sec 8.6.5.3.
  bool retransmitted_range_during_poll_;

  uint8_t n_receiver_ready_polls_sent_;
  bool remote_is_busy_;
  std::list<PendingPdu> pending_pdus_;
  SmartTask receiver_ready_poll_task_{pw_dispatcher_};
  SmartTask monitor_task_{pw_dispatcher_};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(EnhancedRetransmissionModeTxEngine);
};

}  // namespace bt::l2cap::internal
