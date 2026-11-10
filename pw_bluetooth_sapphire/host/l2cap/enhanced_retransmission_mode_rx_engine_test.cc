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

#include <pw_bytes/endian.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/l2cap/fragmenter.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::l2cap::internal {
namespace {

constexpr hci_spec::ConnectionHandle kTestHandle = 0x0001;
constexpr ChannelId kTestChannelId = 0x0001;
constexpr uint8_t kExtendedControlPBitMask = 0b0001'0000;
constexpr uint8_t kExtendedControlFBitMask = 0b1000'0000;
constexpr uint8_t kExtendedControlReceiverReadyBits = 0b0000'0000;
constexpr uint8_t kExtendedControlReceiverNotReadyBits = 0b0000'1000;
constexpr uint8_t kExtendedControlRejectBits = 0b0000'0100;
constexpr uint8_t kExtendedControlSelectiveRejectBits = 0b0000'1100;

using Engine = EnhancedRetransmissionModeRxEngine;

void NoOpTxCallback(ByteBufferPtr) {}
void NoOpFailureCallback() {}

struct RxEngineTestFixture {
  explicit RxEngineTestFixture(
      uint16_t max_rx_sdu_size = kDefaultMTU,
      uint16_t max_rx_pdu_payload_size = kMaxInboundPduPayloadSize)
      : engine(
            max_rx_sdu_size,
            max_rx_pdu_payload_size,
            [this](ByteBufferPtr) { tx_called = true; },
            [this] { ++failure_count; }) {
    engine.set_ack_seq_num_callback([this](uint8_t) { ack_seq_called = true; });
    engine.set_receive_seq_num_callback([this](uint8_t seq_num, bool) {
      receive_seq_num_called = true;
      last_receive_seq_num = seq_num;
    });
  }

  RxEngineTestFixture(const RxEngineTestFixture&) = delete;
  RxEngineTestFixture& operator=(const RxEngineTestFixture&) = delete;
  RxEngineTestFixture(RxEngineTestFixture&&) = delete;
  RxEngineTestFixture& operator=(RxEngineTestFixture&&) = delete;

  ByteBufferPtr ProcessPayload(const ByteBuffer& frame,
                               FrameCheckSequenceOption fcs_option =
                                   FrameCheckSequenceOption::kIncludeFcs) {
    return engine.ProcessPdu(
        Fragmenter(kTestHandle).BuildFrame(kTestChannelId, frame, fcs_option));
  }

  bool tx_called = false;
  size_t failure_count = 0;
  bool ack_seq_called = false;
  bool receive_seq_num_called = false;
  std::optional<uint8_t> last_receive_seq_num;
  Engine engine;
};

DynamicByteBuffer MakeUnsegmentedIFrame(size_t payload_size,
                                        uint8_t tx_seq = 0,
                                        uint8_t receive_seq_num = 0,
                                        uint8_t fill_byte = 'a') {
  SimpleInformationFrameHeader header(tx_seq);
  header.set_receive_seq_num(receive_seq_num);
  DynamicByteBuffer frame(sizeof(header) + payload_size);
  frame.Fill(fill_byte);
  frame.WriteObj(header);
  return frame;
}

DynamicByteBuffer MakeStartOfSduIFrame(uint16_t sdu_len,
                                       size_t payload_size = 0,
                                       uint8_t tx_seq = 0) {
  SimpleStartOfSduFrameHeader header(tx_seq);
  header.sdu_len = pw::bytes::ConvertOrderTo(cpp20::endian::little, sdu_len);
  DynamicByteBuffer frame(sizeof(header) + payload_size);
  frame.Fill('a');
  frame.WriteObj(header);
  return frame;
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduImmediatelyReturnsDataForUnsegmentedSdu) {
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer payload(0, 0, 'h', 'e', 'l', 'l', 'o');
  const ByteBufferPtr sdu =
      Engine(kDefaultMTU,
             kMaxInboundPduPayloadSize,
             NoOpTxCallback,
             NoOpFailureCallback)
          .ProcessPdu(Fragmenter(kTestHandle)
                          .BuildFrame(kTestChannelId,
                                      payload,
                                      FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(sdu);
  EXPECT_TRUE(ContainersEqual(StaticByteBuffer('h', 'e', 'l', 'l', 'o'), *sdu));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduCanHandleZeroBytePayload) {
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer payload(0, 0);
  const ByteBufferPtr sdu =
      Engine(kDefaultMTU,
             kMaxInboundPduPayloadSize,
             NoOpTxCallback,
             NoOpFailureCallback)
          .ProcessPdu(Fragmenter(kTestHandle)
                          .BuildFrame(kTestChannelId,
                                      payload,
                                      FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(sdu);
  EXPECT_EQ(0u, sdu->size());
}

TEST(EnhancedRetransmissionModeRxEngineTest, ProcessPduCanHandleZeroBytePdu) {
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer payload{0, 0};

  RxEngineTestFixture fixture;

  // FCS footer is entirely omitted.
  EXPECT_FALSE(
      fixture.ProcessPayload(payload, FrameCheckSequenceOption::kNoFcs));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.tx_called);
  EXPECT_FALSE(fixture.receive_seq_num_called);
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduCanHandleIncompleteFcsFooter) {
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  // No payload is present and only one byte of the FCS footer is present.
  const StaticByteBuffer payload{0, 0, 0};

  RxEngineTestFixture fixture;

  EXPECT_FALSE(
      fixture.ProcessPayload(payload, FrameCheckSequenceOption::kNoFcs));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.tx_called);
  EXPECT_FALSE(fixture.receive_seq_num_called);
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduDoesNotGenerateSduForOutOfSequencePdu) {
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer payload(  //
      1 << 1,                      // TxSeq = 1, R=0
      0,                           // SAR and ReqSeq
      'h',
      'e',
      'l',
      'l',
      'o');
  EXPECT_FALSE(
      Engine(kDefaultMTU,
             kMaxInboundPduPayloadSize,
             NoOpTxCallback,
             NoOpFailureCallback)
          .ProcessPdu(Fragmenter(kTestHandle)
                          .BuildFrame(kTestChannelId,
                                      payload,
                                      FrameCheckSequenceOption::kIncludeFcs)));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduAdvancesSequenceNumberOnInSequenceFrame) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  // Send with sequence 0.
  {
    const StaticByteBuffer payload(  //
        0 << 1,                      // TxSeq=0, R=0
        0,                           // SAR and ReqSeq
        'h',
        'e',
        'l',
        'l',
        'o');
    ASSERT_TRUE(rx_engine.ProcessPdu(
        Fragmenter(kTestHandle)
            .BuildFrame(kTestChannelId,
                        payload,
                        FrameCheckSequenceOption::kIncludeFcs)));
  }

  // Send with sequence 1.
  {
    const StaticByteBuffer payload(  //
        1 << 1,                      // TxSeq=1, R=0
        0,                           // SAR and ReqSeq
        'h',
        'e',
        'l',
        'l',
        'o');
    ASSERT_TRUE(rx_engine.ProcessPdu(
        Fragmenter(kTestHandle)
            .BuildFrame(kTestChannelId,
                        payload,
                        FrameCheckSequenceOption::kIncludeFcs)));
  }

  // Send with sequence 2.
  {
    const StaticByteBuffer payload(  //
        2 << 1,                      // TxSeq=2, R=0
        0,                           // SAR and ReqSeq
        'h',
        'e',
        'l',
        'l',
        'o');
    EXPECT_TRUE(rx_engine.ProcessPdu(
        Fragmenter(kTestHandle)
            .BuildFrame(kTestChannelId,
                        payload,
                        FrameCheckSequenceOption::kIncludeFcs)));
  }
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRollsOverSequenceNumber) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);
  StaticByteBuffer payload(  //
      0 << 1,                // TxSeq=0, R=0
      0,                     // SAR and ReqSeq
      'h',
      'e',
      'l',
      'l',
      'o');
  for (size_t i = 0; i < 64; ++i) {
    payload[0] = static_cast<uint8_t>(i << 1);  // Set TxSeq
    ASSERT_TRUE(rx_engine.ProcessPdu(
        Fragmenter(kTestHandle)
            .BuildFrame(kTestChannelId,
                        payload,
                        FrameCheckSequenceOption::kIncludeFcs)))
        << " (i=" << i << ")";
  }

  // Per Core Spec v5, Vol 3, Part A, Sec 8.3, the sequence number should now
  // roll over to 0.
  payload[0] = 0 << 1;
  EXPECT_TRUE(rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(
              kTestChannelId, payload, FrameCheckSequenceOption::kIncludeFcs)));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduDoesNotAdvanceSequenceNumberForOutOfSequencePdu) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);
  const StaticByteBuffer out_of_seq(  //
      1 << 1,                         // TxSeq=1, R=0
      0,                              // SAR and ReqSeq
      'h',
      'e',
      'l',
      'l',
      'o');
  ASSERT_FALSE(rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      out_of_seq,
                      FrameCheckSequenceOption::kIncludeFcs)));

  const StaticByteBuffer in_seq(  //
      0 << 1,                     // TxSeq=0, R=0
      0,                          // SAR and ReqSeq
      'h',
      'e',
      'l',
      'l',
      'o');
  EXPECT_TRUE(rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(
              kTestChannelId, in_seq, FrameCheckSequenceOption::kIncludeFcs)));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduImmediatelyAcksUnsegmentedSdu) {
  size_t n_acks = 0;
  ByteBufferPtr outbound_ack;
  auto tx_callback = [&](auto pdu) {
    outbound_ack = std::move(pdu);
    ++n_acks;
  };

  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer payload(0, 0, 'h', 'e', 'l', 'l', 'o');
  ASSERT_TRUE(
      Engine(kDefaultMTU,
             kMaxInboundPduPayloadSize,
             tx_callback,
             NoOpFailureCallback)
          .ProcessPdu(Fragmenter(kTestHandle)
                          .BuildFrame(kTestChannelId,
                                      payload,
                                      FrameCheckSequenceOption::kIncludeFcs)));
  EXPECT_EQ(1u, n_acks);
  ASSERT_TRUE(outbound_ack);
  ASSERT_EQ(sizeof(SimpleReceiverReadyFrame), outbound_ack->size());

  auto ack_frame =
      *reinterpret_cast<const SimpleReceiverReadyFrame*>(outbound_ack->data());
  EXPECT_EQ(SupervisoryFunction::ReceiverReady, ack_frame.function());
  EXPECT_EQ(1u, ack_frame.receive_seq_num());
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduSendsCorrectReqSeqOnRollover) {
  size_t n_acks = 0;
  ByteBufferPtr last_ack;
  auto tx_callback = [&](auto pdu) {
    last_ack = std::move(pdu);
    ++n_acks;
  };

  Engine rx_engine(
      kDefaultMTU, kMaxInboundPduPayloadSize, tx_callback, NoOpFailureCallback);
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  for (size_t i = 0; i < 64; ++i) {
    const StaticByteBuffer payload(i << 1, 0, 'h', 'e', 'l', 'l', 'o');
    ASSERT_TRUE(rx_engine.ProcessPdu(
        Fragmenter(kTestHandle)
            .BuildFrame(kTestChannelId,
                        payload,
                        FrameCheckSequenceOption::kIncludeFcs)))
        << " (i=" << i << ")";
  }
  EXPECT_EQ(64u, n_acks);
  ASSERT_TRUE(last_ack);
  ASSERT_EQ(sizeof(SimpleReceiverReadyFrame), last_ack->size());

  auto ack_frame =
      *reinterpret_cast<const SimpleReceiverReadyFrame*>(last_ack->data());
  EXPECT_EQ(SupervisoryFunction::ReceiverReady, ack_frame.function());
  EXPECT_EQ(0u, ack_frame.receive_seq_num());
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduDoesNotAckOutOfSequenceFrame) {
  size_t n_acks = 0;
  ByteBufferPtr outbound_ack;
  auto tx_callback = [&](auto pdu) {
    outbound_ack = std::move(pdu);
    ++n_acks;
  };

  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer payload(1, 0, 'h', 'e', 'l', 'l', 'o');

  // Per Core Spec, v5, Vol 3, Part A, Sec 8.4.7.1, receipt of an
  // out-of-sequence frame should cause us to transmit a Reject frame. We assume
  // that we should _not_ also transmit a ReceiverReady frame.
  //
  // TODO(fxbug.dev/42055218): Revise this test when we start sending Reject
  // frames.
  ASSERT_FALSE(
      Engine(kDefaultMTU,
             kMaxInboundPduPayloadSize,
             tx_callback,
             NoOpFailureCallback)
          .ProcessPdu(Fragmenter(kTestHandle)
                          .BuildFrame(kTestChannelId,
                                      payload,
                                      FrameCheckSequenceOption::kIncludeFcs)));
  EXPECT_EQ(0u, n_acks);
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRespondsToReceiverReadyPollRequest) {
  size_t n_outbound_frames = 0;
  ByteBufferPtr last_outbound_frame;
  auto tx_callback = [&](auto pdu) {
    last_outbound_frame = std::move(pdu);
    ++n_outbound_frames;
  };

  Engine rx_engine(
      kDefaultMTU, kMaxInboundPduPayloadSize, tx_callback, NoOpFailureCallback);
  // Send an I-frame to advance the receiver's sequence number.
  // See Core Spec, v5, Vol 3, Part A, Table 3.2 for the first two bytes.
  const StaticByteBuffer info_frame(0, 0, 'h', 'e', 'l', 'l', 'o');
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       info_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_EQ(1u, n_outbound_frames);

  // Now send a ReceiverReady poll request. See Core Spec, v5, Vol 3, Part A,
  // Table 3.2 and Table 3.5 for frame format.
  const StaticByteBuffer receiver_ready_poll_request(
      0b1 | kExtendedControlPBitMask, 0);
  auto local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_ready_poll_request,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverReady frame.
  EXPECT_EQ(2u, n_outbound_frames);
  ASSERT_TRUE(last_outbound_frame);
  ASSERT_EQ(sizeof(SimpleSupervisoryFrame), last_outbound_frame->size());

  auto sframe = *reinterpret_cast<const SimpleSupervisoryFrame*>(
      last_outbound_frame->data());
  EXPECT_EQ(SupervisoryFunction::ReceiverReady, sframe.function());
  EXPECT_EQ(1u, sframe.receive_seq_num());
  EXPECT_TRUE(sframe.is_poll_response());
  EXPECT_FALSE(sframe.is_poll_request());
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduCallsReceiveSeqNumCallback) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  std::optional<uint8_t> receive_seq_num;
  std::optional<bool> receive_is_poll_response;
  auto receive_seq_num_callback = [&receive_seq_num, &receive_is_poll_response](
                                      uint8_t seq_num, bool is_poll_response) {
    receive_seq_num = seq_num;
    receive_is_poll_response = is_poll_response;
  };
  rx_engine.set_receive_seq_num_callback(receive_seq_num_callback);

  // Send an I-frame containing an acknowledgment up to the 3rd frame that we
  // transmitted. See Core Spec, v5, Vol 3, Part A, Section 3.3.2, Table 3.2 for
  // the first two bytes.
  StaticByteBuffer info_frame(0, 3, 'h', 'e', 'l', 'l', 'o');
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       info_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(receive_seq_num.has_value());
  EXPECT_EQ(3, receive_seq_num.value());
  ASSERT_TRUE(receive_is_poll_response.has_value());
  EXPECT_FALSE(receive_is_poll_response.value());

  receive_is_poll_response.reset();

  // Same as above but the 'F' bit is set.
  info_frame[0] |= kExtendedControlFBitMask;
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       info_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(receive_is_poll_response.has_value());
  EXPECT_TRUE(receive_is_poll_response.value());

  receive_seq_num.reset();
  receive_is_poll_response.reset();

  // Send an S-frame containing an acknowledgment up to the 4th frame that we
  // transmitted. F is set. See Core Spec, v5, Vol 3, Part A, Section 3.3.2,
  // Table 3.2 for the frame format.
  StaticByteBuffer receiver_ready(0b1 | kExtendedControlFBitMask, 4);
  auto local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(receive_seq_num.has_value());
  EXPECT_EQ(4, receive_seq_num.value());
  ASSERT_TRUE(receive_is_poll_response.has_value());
  EXPECT_TRUE(receive_is_poll_response.value());

  receive_is_poll_response.reset();

  // Same as above but the 'F' bit is clear.
  receiver_ready[0] &= ~kExtendedControlFBitMask;
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       receiver_ready,
                                       FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(receive_is_poll_response.has_value());
  EXPECT_FALSE(receive_is_poll_response.value());
}

TEST(EnhancedRetransmissionModeRxEngineTest, ProcessPduCallsAckSeqNumCallback) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  std::optional<uint8_t> ack_seq_num;
  auto ack_seq_num_callback = [&ack_seq_num](uint8_t seq_num) {
    ack_seq_num = seq_num;
  };
  rx_engine.set_ack_seq_num_callback(ack_seq_num_callback);

  // Send an I-frame containing a sequence number for the first frame the
  // receiver has sent. See Core Spec, v5, Vol 3, Part A, Section 3.3.2,
  // Table 3.2 for the first two bytes.
  StaticByteBuffer info_frame(0, 0, 'h', 'e', 'l', 'l', 'o');
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       info_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  ASSERT_TRUE(ack_seq_num.has_value());

  // We should now expect the next (second) frame to have a sequence number
  // of 1.
  EXPECT_EQ(1, ack_seq_num.value());
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduCallsRemoteBusySetCallbackOnReceiverNotReady) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  bool receive_seq_num_called = false;
  rx_engine.set_receive_seq_num_callback(
      [&](uint8_t, bool) { receive_seq_num_called = true; });

  bool remote_busy_set_called = false;
  auto remote_busy_set_callback = [&receive_seq_num_called,
                                   &remote_busy_set_called] {
    // RemoteBusy state should be updated before ReceiveSeqNum to immediately
    // suppress retransmissions.
    EXPECT_FALSE(receive_seq_num_called);
    remote_busy_set_called = true;
  };
  rx_engine.set_remote_busy_set_callback(remote_busy_set_callback);

  const StaticByteBuffer receiver_not_ready(
      0b1 | kExtendedControlReceiverNotReadyBits, 0);
  auto local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_not_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverNotReady frame.
  EXPECT_TRUE(remote_busy_set_called);
  EXPECT_TRUE(receive_seq_num_called);

  remote_busy_set_called = false;
  local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_not_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverNotReady frame.

  // Second RNR should not invoke the callback.
  EXPECT_FALSE(remote_busy_set_called);
}

TEST(
    EnhancedRetransmissionModeRxEngineTest,
    ProcessPduCallsRemoteBusySetCallbackOnReceiverNotReadyAfterReceiverReadyClearedBusy) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  const StaticByteBuffer receiver_not_ready(
      0b1 | kExtendedControlReceiverNotReadyBits, 0);
  auto local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_not_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverNotReady frame.

  // This RR should clear RemoteBusy.
  const StaticByteBuffer receiver_ready(0b1, 0);
  local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverReady frame.

  bool remote_busy_set_called = false;
  auto remote_busy_set_callback = [&remote_busy_set_called] {
    remote_busy_set_called = true;
  };
  rx_engine.set_remote_busy_set_callback(remote_busy_set_callback);

  // Receive a second RNR.
  local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_not_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverNotReady frame.

  // Second RNR should invoke the callback because it's setting RemoteBusy once
  // again.
  EXPECT_TRUE(remote_busy_set_called);
}

// Test parameter is a bitmask to the Extended Control Field.
class ExtendedControlFieldBitsTest : public testing::TestWithParam<uint16_t> {};

TEST_P(
    ExtendedControlFieldBitsTest,
    ProcessPduCallsRemoteBusyClearedCallbackOnNonRnrSFrameAfterReceiverNotReady) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  bool receive_seq_num_called = false;
  int remote_busy_set_calls = 0;
  rx_engine.set_remote_busy_set_callback([&] { remote_busy_set_calls++; });

  int remote_busy_cleared_calls = 0;
  auto remote_busy_cleared_callback = [&receive_seq_num_called,
                                       &remote_busy_cleared_calls] {
    // RemoteBusy state should be updated before ReceiveSeqNum to immediately
    // resume retransmissions.
    EXPECT_FALSE(receive_seq_num_called);
    remote_busy_cleared_calls++;
  };
  rx_engine.set_remote_busy_cleared_callback(remote_busy_cleared_callback);

  const StaticByteBuffer receiver_not_ready(
      0b1 | kExtendedControlReceiverNotReadyBits, 0);
  auto local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      receiver_not_ready,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in a ReceiverNotReady frame.

  EXPECT_EQ(1, remote_busy_set_calls);
  EXPECT_EQ(0, remote_busy_cleared_calls);

  // The RNR invokes this callback but we only care about the callback ordering
  // of the next frame.
  rx_engine.set_receive_seq_num_callback(
      [&](uint8_t, bool) { receive_seq_num_called = true; });

  // This non-RNR S-Frame should clear RemoteBusy.
  const uint16_t control_bits_to_set = GetParam();
  const StaticByteBuffer non_rnr_s_frame(0b1 | LowerBits(control_bits_to_set),
                                         0);
  local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      non_rnr_s_frame,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in an S-Frame.

  EXPECT_EQ(1, remote_busy_set_calls);
  EXPECT_EQ(1, remote_busy_cleared_calls);
  EXPECT_TRUE(receive_seq_num_called);

  // Receive a second non-RNR.
  local_sdu = rx_engine.ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      non_rnr_s_frame,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_FALSE(local_sdu);  // No payload in an S-Frame.

  EXPECT_EQ(1, remote_busy_set_calls);
  EXPECT_EQ(1, remote_busy_cleared_calls);

  // Second S-Frame shouldn't invoke either callback because RemoteBusy remains
  // cleared.
}

INSTANTIATE_TEST_SUITE_P(EnhancedRetransmissionModeRxEngineTestNonRnrSFrames,
                         ExtendedControlFieldBitsTest,
                         testing::Values(kExtendedControlReceiverReadyBits,
                                         kExtendedControlRejectBits,
                                         kExtendedControlSelectiveRejectBits));

TEST(
    EnhancedRetransmissionModeRxEngineTest,
    ProcessPduCallsRangeRetransmitSetCallbackThenReceiveSeqNumCallbackOnReject) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  std::optional<bool> receive_is_poll_request;
  rx_engine.set_range_retransmit_set_callback(
      [&](bool is_poll_request) { receive_is_poll_request = is_poll_request; });

  std::optional<uint8_t> receive_seq_num;
  std::optional<bool> receive_is_poll_response;
  auto receive_seq_num_callback = [&](uint8_t seq_num, bool is_poll_response) {
    // RangeRetransmitCallback should be called before ReceiveSeqNumCallback.
    EXPECT_TRUE(receive_is_poll_request.has_value());
    receive_seq_num = seq_num;
    receive_is_poll_response = is_poll_response;
  };
  rx_engine.set_receive_seq_num_callback(receive_seq_num_callback);

  // Send a REJ S-frame containing acknowledgment up to the 3rd frame that we
  // transmitted. See Core Spec, v5, Vol 3, Part A, Section 3.3.2, Table 3.2 for
  // the first two bytes.
  auto rej_frame = StaticByteBuffer(0b1 | kExtendedControlRejectBits, 3);
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       rej_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_TRUE(receive_seq_num.has_value());
  EXPECT_TRUE(receive_is_poll_response.has_value());
}

TEST(
    EnhancedRetransmissionModeRxEngineTest,
    ProcessPduCallsSingleRetransmitSetCallbackThenReceiveSeqNumCallbackOnSelectiveReject) {
  Engine rx_engine(kDefaultMTU,
                   kMaxInboundPduPayloadSize,
                   NoOpTxCallback,
                   NoOpFailureCallback);

  std::optional<bool> receive_is_poll_request;
  rx_engine.set_single_retransmit_set_callback(
      [&](bool is_poll_request) { receive_is_poll_request = is_poll_request; });

  std::optional<uint8_t> receive_seq_num;
  std::optional<bool> receive_is_poll_response;
  auto receive_seq_num_callback = [&](uint8_t seq_num, bool is_poll_response) {
    // SingleRetransmitCallback should be called before ReceiveSeqNumCallback.
    EXPECT_TRUE(receive_is_poll_request.has_value());
    receive_seq_num = seq_num;
    receive_is_poll_response = is_poll_response;
  };
  rx_engine.set_receive_seq_num_callback(receive_seq_num_callback);

  // Send an SREJ S-frame containing acknowledgment up to the 3rd frame that we
  // transmitted. See Core Spec, v5, Vol 3, Part A, Section 3.3.2, Table 3.2 for
  // the first two bytes.
  auto srej_frame =
      StaticByteBuffer(0b1 | kExtendedControlSelectiveRejectBits, 3);
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       srej_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_TRUE(receive_seq_num.has_value());
  EXPECT_TRUE(receive_is_poll_response.has_value());
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduWithPollResponseAndPollRequestClosesChannel) {
  bool connection_failed = false;
  bool receive_seq_num_called = false;
  Engine rx_engine(kDefaultMTU, kMaxInboundPduPayloadSize, NoOpTxCallback, [&] {
    connection_failed = true;
  });
  rx_engine.set_receive_seq_num_callback(
      [&](uint8_t, bool) { receive_seq_num_called = true; });

  // Send an RR S-frame with both poll request and response bits set.
  // See Core Spec, v5, Vol 3, Part A, Section 3.3.2, Table 3.2 for field
  // definitions.
  auto rr_frame = StaticByteBuffer(
      0b1 | kExtendedControlFBitMask | kExtendedControlPBitMask, 0);
  rx_engine.ProcessPdu(Fragmenter(kTestHandle)
                           .BuildFrame(kTestChannelId,
                                       rr_frame,
                                       FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_TRUE(connection_failed);
  EXPECT_FALSE(receive_seq_num_called);
}

TEST(EnhancedRetransmissionModeRxEngineTest, DestructEngineInAckCallback) {
  std::unique_ptr<Engine> rx_engine;
  auto tx_callback = [](auto) {};
  auto ack_callback = [&](uint8_t) { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(
      kDefaultMTU, kMaxInboundPduPayloadSize, tx_callback, NoOpFailureCallback);
  rx_engine->set_ack_seq_num_callback(ack_callback);

  // Send an I-frame to trigger ack_seq_num_callback.
  const StaticByteBuffer info_frame(0, 0, 'h', 'e', 'l', 'l', 'o');
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        info_frame,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     DestructEngineInRemoteBusySetCallback) {
  std::unique_ptr<Engine> rx_engine;
  auto remote_busy_set_callback = [&] { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(kDefaultMTU,
                                       kMaxInboundPduPayloadSize,
                                       NoOpTxCallback,
                                       NoOpFailureCallback);
  rx_engine->set_remote_busy_set_callback(remote_busy_set_callback);

  // Send RNR S-frame to trigger remote_busy_set_callback.
  const StaticByteBuffer receiver_not_ready(
      0b1 | kExtendedControlReceiverNotReadyBits, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_not_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     DestructEngineInRemoteBusyClearedCallback) {
  std::unique_ptr<Engine> rx_engine;
  auto remote_busy_cleared_callback = [&] { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(kDefaultMTU,
                                       kMaxInboundPduPayloadSize,
                                       NoOpTxCallback,
                                       NoOpFailureCallback);
  rx_engine->set_remote_busy_cleared_callback(remote_busy_cleared_callback);

  // First set remote busy.
  const StaticByteBuffer receiver_not_ready(
      0b1 | kExtendedControlReceiverNotReadyBits, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_not_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Now clear it with RR.
  const StaticByteBuffer receiver_ready(0b1, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     DestructEngineInRangeRetransmitSetCallback) {
  std::unique_ptr<Engine> rx_engine;
  auto range_retransmit_set_callback = [&](bool) { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(kDefaultMTU,
                                       kMaxInboundPduPayloadSize,
                                       NoOpTxCallback,
                                       NoOpFailureCallback);
  rx_engine->set_range_retransmit_set_callback(range_retransmit_set_callback);

  // Send REJ S-frame to trigger range_retransmit_set_callback.
  auto rej_frame = StaticByteBuffer(0b1 | kExtendedControlRejectBits, 3);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        rej_frame,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     DestructEngineInSingleRetransmitSetCallback) {
  std::unique_ptr<Engine> rx_engine;
  auto single_retransmit_set_callback = [&](bool) { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(kDefaultMTU,
                                       kMaxInboundPduPayloadSize,
                                       NoOpTxCallback,
                                       NoOpFailureCallback);
  rx_engine->set_single_retransmit_set_callback(single_retransmit_set_callback);

  // Send SREJ S-frame to trigger single_retransmit_set_callback.
  auto srej_frame =
      StaticByteBuffer(0b1 | kExtendedControlSelectiveRejectBits, 3);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        srej_frame,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

TEST(EnhancedRetransmissionModeRxEngineTest,
     DestructEngineInConnectionFailureCallback) {
  std::unique_ptr<Engine> rx_engine;
  auto connection_failure_cb = [&] { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(kDefaultMTU,
                                       kMaxInboundPduPayloadSize,
                                       NoOpTxCallback,
                                       connection_failure_cb);

  // Send an S-frame with both poll request and response bits set.
  auto rr_frame = StaticByteBuffer(
      0b1 | kExtendedControlFBitMask | kExtendedControlPBitMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        rr_frame,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

// Verifies that frames shorter than an `EnhancedControlField` close the channel
// without invoking outbound or `PassToTx` callbacks.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRejectsFrameShorterThanEnhancedControlField) {
  for (size_t len = 0; len < sizeof(EnhancedControlField); ++len) {
    SCOPED_TRACE(len);
    RxEngineTestFixture fixture;

    DynamicByteBuffer truncated_frame(len);
    EXPECT_FALSE(fixture.ProcessPayload(truncated_frame,
                                        FrameCheckSequenceOption::kNoFcs));
    EXPECT_EQ(1u, fixture.failure_count);
    EXPECT_FALSE(fixture.tx_called);
    EXPECT_FALSE(fixture.receive_seq_num_called);
  }
}

// Verifies that an S-frame longer than a supervisory frame plus FCS closes the
// channel without invoking `PassToTx`.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRejectsSupervisoryFrameWithInvalidLength) {
  RxEngineTestFixture fixture;
  SimpleReceiverReadyFrame sframe;
  StaticByteBuffer<sizeof(sframe) + 1> long_sframe;
  long_sframe.Fill(0);
  long_sframe.WriteObj(sframe);
  EXPECT_FALSE(fixture.ProcessPayload(long_sframe));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.receive_seq_num_called);
}

// Verifies that destroying the engine from the connection failure callback
// while rejecting an S-frame with an invalid length is safe.
TEST(EnhancedRetransmissionModeRxEngineTest,
     DestructEngineInConnectionFailureCallbackForInvalidSupervisoryFrame) {
  std::unique_ptr<Engine> rx_engine;
  auto connection_failure_cb = [&] { rx_engine.reset(); };
  rx_engine = std::make_unique<Engine>(kDefaultMTU,
                                       kMaxInboundPduPayloadSize,
                                       NoOpTxCallback,
                                       connection_failure_cb);

  // RR S-frame with a trailing payload byte.
  auto long_rr_frame = StaticByteBuffer(0b1, 0, 0);
  EXPECT_FALSE(rx_engine->ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      long_rr_frame,
                      FrameCheckSequenceOption::kIncludeFcs)));
  EXPECT_FALSE(rx_engine);
}

// Verifies `max_rx_sdu_size` handling for unsegmented I-frames: out-of-sequence
// frames exceeding `max_rx_sdu_size` are ignored while forwarding `ReqSeq`,
// in-sequence frames at `max_rx_sdu_size` succeed, and in-sequence frames
// exceeding `max_rx_sdu_size` close the channel and suppress further PDUs.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduEnforcesMaxRxSduSizeForInSequenceUnsegmentedIFrame) {
  constexpr uint16_t kMtu = kMinACLMTU;
  constexpr uint8_t kExpectedReceiveSeqNum = 5;
  RxEngineTestFixture fixture(kMtu);

  // Out-of-sequence (`tx_seq = 1` when expecting `0`) oversized I-frame does
  // not close the channel and still forwards `ReqSeq` via `PassToTx`.
  EXPECT_FALSE(fixture.ProcessPayload(
      MakeUnsegmentedIFrame(kMtu + 1, /*tx_seq=*/1, kExpectedReceiveSeqNum)));
  EXPECT_EQ(0u, fixture.failure_count);
  EXPECT_EQ(kExpectedReceiveSeqNum, fixture.last_receive_seq_num);

  // In-sequence (`tx_seq = 0`) I-frame at `max_rx_sdu_size` is accepted and
  // advances expected `tx_seq` to `1`.
  ByteBufferPtr sdu =
      fixture.ProcessPayload(MakeUnsegmentedIFrame(kMtu, /*tx_seq=*/0));
  ASSERT_TRUE(sdu);
  EXPECT_EQ(kMtu, sdu->size());
  EXPECT_EQ(0u, fixture.failure_count);

  // In-sequence (`tx_seq = 1`) I-frame exceeding `max_rx_sdu_size` closes the
  // channel without acknowledging or forwarding `ReqSeq`.
  fixture.tx_called = false;
  fixture.ack_seq_called = false;
  fixture.receive_seq_num_called = false;
  EXPECT_FALSE(
      fixture.ProcessPayload(MakeUnsegmentedIFrame(kMtu + 1, /*tx_seq=*/1)));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.tx_called);
  EXPECT_FALSE(fixture.ack_seq_called);
  EXPECT_FALSE(fixture.receive_seq_num_called);

  // Subsequent valid frames must be ignored once the engine has failed.
  EXPECT_FALSE(fixture.ProcessPayload(
      MakeUnsegmentedIFrame(/*payload_size=*/2, /*tx_seq=*/1)));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.tx_called);
  EXPECT_FALSE(fixture.receive_seq_num_called);
}

// Verifies that an I-frame whose information payload exceeds
// `max_rx_pdu_payload_size` closes the channel before sequence-number checking,
// whereas an I-frame at `max_rx_pdu_payload_size` succeeds.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduEnforcesMaxRxPduPayloadSizeBeforeSequenceCheck) {
  constexpr uint16_t kMps = 64;
  constexpr uint16_t kMtu = 128;
  constexpr uint8_t kOutOfSeqTxSeq = 1;

  // Unsegmented I-frame equal to MPS succeeds.
  {
    RxEngineTestFixture fixture(kMtu, kMps);
    ByteBufferPtr sdu = fixture.ProcessPayload(MakeUnsegmentedIFrame(kMps));
    ASSERT_TRUE(sdu);
    EXPECT_EQ(kMps, sdu->size());
    EXPECT_EQ(0u, fixture.failure_count);
  }

  // Out-of-sequence unsegmented I-frame exceeding MPS closes the channel
  // without invoking `PassToTx`.
  {
    RxEngineTestFixture fixture(kMtu, kMps);
    EXPECT_FALSE(fixture.ProcessPayload(
        MakeUnsegmentedIFrame(kMps + 1, kOutOfSeqTxSeq)));
    EXPECT_EQ(1u, fixture.failure_count);
    EXPECT_FALSE(fixture.receive_seq_num_called);
  }

  // Out-of-sequence Start-of-SDU I-frame at MPS does not close the channel,
  // while one exceeding MPS does.
  {
    RxEngineTestFixture fixture(kMtu, kMps);
    EXPECT_FALSE(fixture.ProcessPayload(
        MakeStartOfSduIFrame(kMtu, kMps, kOutOfSeqTxSeq)));
    EXPECT_EQ(0u, fixture.failure_count);

    EXPECT_FALSE(fixture.ProcessPayload(
        MakeStartOfSduIFrame(kMtu, kMps + 1, kOutOfSeqTxSeq)));
    EXPECT_EQ(1u, fixture.failure_count);
  }
}

// Verifies that a truncated Start-of-SDU I-frame missing its complete SDU
// length field is ignored when out of sequence and closes the channel when
// received in sequence.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRejectsTruncatedStartOfSduFrameWhenInSequence) {
  constexpr size_t kTruncatedHeaderLen =
      sizeof(SimpleStartOfSduFrameHeader) - 1;
  RxEngineTestFixture fixture;

  const DynamicByteBuffer out_of_seq =
      MakeStartOfSduIFrame(kDefaultMTU, /*payload_size=*/0, /*tx_seq=*/1);
  EXPECT_FALSE(fixture.ProcessPayload(
      BufferView(out_of_seq.data(), kTruncatedHeaderLen)));
  EXPECT_EQ(0u, fixture.failure_count);
  EXPECT_TRUE(fixture.receive_seq_num_called);

  fixture.receive_seq_num_called = false;
  const DynamicByteBuffer in_seq =
      MakeStartOfSduIFrame(kDefaultMTU, /*payload_size=*/0, /*tx_seq=*/0);
  EXPECT_FALSE(
      fixture.ProcessPayload(BufferView(in_seq.data(), kTruncatedHeaderLen)));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.receive_seq_num_called);
}

// Verifies that a Start-of-SDU I-frame whose `sdu_len` exceeds
// `max_rx_sdu_size` closes the channel when received in sequence, but not when
// out of sequence.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRejectsStartOfSduExceedingMaxRxSduSizeWhenInSequence) {
  constexpr uint16_t kMtu = kMinACLMTU;
  RxEngineTestFixture fixture(kMtu);

  // Out-of-sequence Start-of-SDU exceeding MTU does not close the channel.
  EXPECT_FALSE(fixture.ProcessPayload(
      MakeStartOfSduIFrame(kMtu + 1, /*payload_size=*/0, /*tx_seq=*/1)));
  EXPECT_EQ(0u, fixture.failure_count);

  // In-sequence Start-of-SDU at MTU does not close the channel.
  EXPECT_FALSE(fixture.ProcessPayload(
      MakeStartOfSduIFrame(kMtu, /*payload_size=*/0, /*tx_seq=*/0)));
  EXPECT_EQ(0u, fixture.failure_count);

  // In-sequence Start-of-SDU exceeding MTU closes the channel.
  fixture.receive_seq_num_called = false;
  EXPECT_FALSE(fixture.ProcessPayload(
      MakeStartOfSduIFrame(kMtu + 1, /*payload_size=*/0, /*tx_seq=*/0)));
  EXPECT_EQ(1u, fixture.failure_count);
  EXPECT_FALSE(fixture.receive_seq_num_called);
}

// Verifies that Continuation and End-of-SDU I-frames received in sequence
// without a preceding Start-of-SDU close the channel.
TEST(EnhancedRetransmissionModeRxEngineTest,
     ProcessPduRejectsUnexpectedContinuationOrEndOfSduFrame) {
  for (auto status :
       {SegmentationStatus::MiddleSegment, SegmentationStatus::LastSegment}) {
    RxEngineTestFixture fixture;

    SimpleInformationFrameHeader header(/*tx_seq=*/0);
    header.set_segmentation_status(status);
    StaticByteBuffer<sizeof(header) + 1> buf;
    buf.WriteObj(header);
    buf[sizeof(header)] = 'z';

    EXPECT_FALSE(fixture.ProcessPayload(buf));
    EXPECT_EQ(1u, fixture.failure_count);
    EXPECT_FALSE(fixture.receive_seq_num_called);
  }
}

}  // namespace
}  // namespace bt::l2cap::internal
