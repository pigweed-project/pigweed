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

#include <gmock/gmock.h>
#include <pw_async/fake_dispatcher_fixture.h>

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_tx_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fragmenter.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::l2cap::internal {
namespace {

class EnhancedRetransmissionModeEnginesTest
    : public pw::async::test::FakeDispatcherFixture {
 protected:
  void TxProcessSdu(TxEngine& engine, ByteBufferPtr sdu) {
    channel_.QueueSdu(std::move(sdu));
    engine.NotifySduQueued();
  }

  FakeTxChannel& channel() { return channel_; }

 private:
  FakeTxChannel channel_;
};

constexpr size_t kMaxTransmissions = 2;
constexpr size_t kTxWindow = 63;

constexpr hci_spec::ConnectionHandle kTestHandle = 0x0001;
constexpr ChannelId kTestChannelId = 0x0001;
constexpr uint8_t kExtendedControlFBitMask = 0b1000'0000;
constexpr uint8_t kExtendedControlPBitMask = 0b0001'0000;
constexpr uint8_t kExtendedControlRejFunctionMask = 0b0000'0100;
constexpr uint8_t kExtendedControlReceiverNotReadyFunctionMask = 0b0000'1000;
constexpr uint8_t kExtendedControlSrejFunctionMask = 0b0000'1100;

void NoOpFailureCallback() {}

TEST_F(EnhancedRetransmissionModeEnginesTest, MakeLinkedERTMEngines) {
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  EXPECT_TRUE(rx_engine);
  EXPECT_TRUE(tx_engine);
}

// This test that TxEngine sends I-Frames whose acknowledgement sequence numbers
// match the receive sequence number in RxEngine. This also tests that peer
// acknowledgement restarts our outbound data that was paused by hitting
// TxWindow. This mirrors the L2CAP Test Specification L2CAP/ERM/BV-06-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       OutboundInformationFramesAcknowledgeReceivedInformationFrames) {
  int tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    ASSERT_TRUE(pdu);
    // Unlike the Test Spec's sequence diagram, we respond to the peer's
    // I-Frame with a Receiver Ready (which is always allowed), so our
    // subsequent I-Frame is actually the third outbound.
    if (tx_count == 0) {
      ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_information_frame());
      const auto& header = pdu->To<SimpleInformationFrameHeader>();
      EXPECT_EQ(0, header.tx_seq());
      EXPECT_EQ(0, header.receive_seq_num());
    } else if (tx_count == 2) {
      ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_information_frame());
      const auto& header = pdu->To<SimpleInformationFrameHeader>();
      EXPECT_EQ(1, header.tx_seq());

      // Acknowledges the I-Frame from the peer.
      EXPECT_EQ(1, header.receive_seq_num());
    }
    tx_count++;
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  /*n_frames_in_tx_window=*/1,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(
                   StaticByteBuffer{'p', 'i', 'n', 'g'}));
  EXPECT_EQ(1, tx_count);

  // Receive an I-frame containing an acknowledgment including the frame that we
  // transmitted. See Core Spec, v5, Vol 3, Part A, Section 3.3.2, I-Frame
  // Enhanced Control Field.
  StaticByteBuffer info_frame(0, 1, 'p', 'o', 'n', 'g');
  EXPECT_TRUE(rx_engine->ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      info_frame,
                      FrameCheckSequenceOption::kIncludeFcs)));
  EXPECT_EQ(2, tx_count);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(
                   StaticByteBuffer{'b', 'y', 'e', 'e'}));
  EXPECT_EQ(3, tx_count);
}

// This test simulates a peer's non-response to our S-Frame poll request which
// causes us to raise a channel error after the monitor timer expires. This
// mirrors L2CAP Test Specification v5.0.2 L2CAP/ERM/BV-11-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       SignalFailureAfterMonitorTimerExpiry) {
  int tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    ASSERT_TRUE(pdu);
    // Unlike the Test Spec's sequence diagram, we respond to the peer's I-Frame
    // with a Receiver Ready (which is always allowed), so our subsequent
    // I-Frame is actually the third outbound.
    if (tx_count == 0) {
      ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_information_frame());
      const auto& header = pdu->To<SimpleInformationFrameHeader>();
      EXPECT_EQ(0, header.tx_seq());
      EXPECT_EQ(0, header.receive_seq_num());
    } else if (tx_count == 1) {
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      ASSERT_TRUE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
    }
    tx_count++;
  });
  bool failure_cb_called = false;
  auto failure_cb = [&failure_cb_called] { failure_cb_called = true; };
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  /*max_transmissions=*/1,
                                                  kTxWindow,
                                                  channel(),
                                                  failure_cb,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(
                   StaticByteBuffer{'p', 'i', 'n', 'g'}));
  EXPECT_EQ(1, tx_count);

  // Send a poll request after timer expiry waiting for peer acknowledgment.
  RETURN_IF_FATAL(RunFor(kErtmReceiverReadyPollTimerDuration));
  EXPECT_EQ(2, tx_count);

  // Monitor Timer expires without a response from the peer, signaling a channel
  // failure.
  EXPECT_FALSE(failure_cb_called);
  RETURN_IF_FATAL(RunFor(kErtmMonitorTimerDuration));
  EXPECT_TRUE(failure_cb_called);
}

// This test simulates non-acknowledgment of an I-Frame that the local host
// sends which causes us to meet the MaxTransmit that the peer specified,
// raising a local error for the channel. This mirrors L2CAP Test Specification
// v5.0.2 L2CAP/ERM/BV-12-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       SignalFailureAfterMaxTransmitExhausted) {
  int tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    ASSERT_TRUE(pdu);
    // Unlike the Test Spec's sequence diagram, we respond to the peer's I-Frame
    // with a Receiver Ready (which is always allowed), so our subsequent
    // I-Frame is actually the third outbound.
    if (tx_count == 0) {
      ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_information_frame());
      const auto& header = pdu->To<SimpleInformationFrameHeader>();
      EXPECT_EQ(0, header.tx_seq());
      EXPECT_EQ(0, header.receive_seq_num());
    } else if (tx_count == 1) {
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      ASSERT_TRUE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
    }
    tx_count++;
  });
  bool failure_cb_called = false;
  auto failure_cb = [&failure_cb_called] { failure_cb_called = true; };
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  /*max_transmissions=*/1,
                                                  kTxWindow,
                                                  channel(),
                                                  failure_cb,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(
                   StaticByteBuffer{'p', 'i', 'n', 'g'}));
  EXPECT_EQ(1, tx_count);

  // Send a poll request after timer expiry waiting for peer acknowledgment.
  RETURN_IF_FATAL(RunFor(kErtmReceiverReadyPollTimerDuration));
  EXPECT_EQ(2, tx_count);

  // Peer response doesn't acknowledge the I-Frame's TxSeq and we already used
  // up MaxTransmit, signaling a channel failure.
  EXPECT_FALSE(failure_cb_called);

  // Receive an S-frame containing an acknowledgment not including the frame
  // that we transmitted. F is set. See Core Spec, v5, Vol 3, Part A,
  // Section 3.3.2, S-Frame Enhanced Control Field.
  StaticByteBuffer receiver_ready(0b1 | kExtendedControlFBitMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_TRUE(failure_cb_called);
}

// This tests the integration of receiving Reject frames with triggering
// retransmission of requested unacknowledged I-Frames. This mirrors the L2CAP
// Test Specification v5.0.2 L2CAP/ERM/BV-13-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       RetransmitAfterReceivingRejectSFrame) {
  int tx_count = 0;
  int iframe_0_tx_count = 0;
  int iframe_1_tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    tx_count++;
    ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
    ASSERT_TRUE(pdu->To<EnhancedControlField>().designates_information_frame());
    const auto& header = pdu->To<SimpleInformationFrameHeader>();
    if (header.tx_seq() == 0) {
      iframe_0_tx_count++;
    } else if (header.tx_seq() == 1) {
      iframe_1_tx_count++;
    }
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('a')));
  EXPECT_EQ(1, tx_count);
  EXPECT_EQ(1, iframe_0_tx_count);
  EXPECT_EQ(0, iframe_1_tx_count);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('b')));
  EXPECT_EQ(2, tx_count);
  EXPECT_EQ(1, iframe_0_tx_count);
  EXPECT_EQ(1, iframe_1_tx_count);

  // Receive an S-frame containing a retransmission request starting at seq = 0.
  // See Core Spec v5.0, Vol 3, Part A, Section 3.3.2 S-Frame Enhanced Control
  // Field.
  StaticByteBuffer reject(0b1 | kExtendedControlRejFunctionMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        reject,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Check that this caused a retransmission of the two I-Frames.
  EXPECT_EQ(4, tx_count);
  EXPECT_EQ(2, iframe_0_tx_count);
  EXPECT_EQ(2, iframe_1_tx_count);
}

// This tests the integration of receiving Selective Reject frames that are poll
// requests, triggering retransmission as well as acknowledgment that frees up
// transmit window. This mirrors the L2CAP Test Specification v5.0.2
// L2CAP/ERM/BV-14-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       RetransmitAfterReceivingSelectiveRejectPollRequestSFrame) {
  int tx_count = 0;
  std::array<int, 4> iframe_tx_counts{};
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    tx_count++;
    ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
    ASSERT_TRUE(pdu->To<EnhancedControlField>().designates_information_frame());
    const auto& header = pdu->To<SimpleInformationFrameHeader>();
    ASSERT_GT(iframe_tx_counts.size(), header.tx_seq());
    iframe_tx_counts[header.tx_seq()]++;
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  /*n_frames_in_tx_window=*/3,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  for (int i = 0; i < 4; i++) {
    TxProcessSdu(
        *tx_engine,
        std::make_unique<DynamicByteBuffer>(StaticByteBuffer('a' + i)));
  }

  // TxWindow caps transmissions.
  EXPECT_EQ(3, tx_count);
  EXPECT_THAT(iframe_tx_counts, testing::ElementsAre(1, 1, 1, 0));

  // Receive an S-frame containing a poll request retransmission request for seq
  // = 1. See Core Spec v5.0, Vol 3, Part A, Section 3.3.2 S-Frame Enhanced
  // Control Field.
  StaticByteBuffer selective_reject(
      0b1 | kExtendedControlSrejFunctionMask | kExtendedControlPBitMask, 1);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        selective_reject,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Check that this caused a retransmission of I-Frames 1 and 3 because the
  // poll request acknowledged I-Frame 0, freeing up space in the transmit
  // window.
  EXPECT_EQ(5, tx_count);
  EXPECT_THAT(iframe_tx_counts, testing::ElementsAre(1, 2, 1, 1));
}

// This tests the integration of receiving Selective Reject frames that are not
// poll requests, triggering retransmission only. This mirrors the L2CAP Test
// Specification v5.0.2 L2CAP/ERM/BV-15-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       RetransmitAfterReceivingSelectiveRejectSFrame) {
  int tx_count = 0;
  std::array<int, 4> iframe_tx_counts{};
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    tx_count++;
    ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
    ASSERT_TRUE(pdu->To<EnhancedControlField>().designates_information_frame());
    const auto& header = pdu->To<SimpleInformationFrameHeader>();
    ASSERT_GT(iframe_tx_counts.size(), header.tx_seq());
    iframe_tx_counts[header.tx_seq()]++;
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  /*n_frames_in_tx_window=*/3,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  for (int i = 0; i < 4; i++) {
    TxProcessSdu(
        *tx_engine,
        std::make_unique<DynamicByteBuffer>(StaticByteBuffer('a' + i)));
  }

  // TxWindow caps transmissions.
  EXPECT_EQ(3, tx_count);
  EXPECT_THAT(iframe_tx_counts, testing::ElementsAre(1, 1, 1, 0));

  // Receive an S-frame containing a retransmission request for seq = 1. See
  // Core Spec v5.0, Vol 3, Part A, Section 3.3.2 S-Frame Enhanced Control
  // Field.
  StaticByteBuffer selective_reject(0b1 | kExtendedControlSrejFunctionMask, 1);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        selective_reject,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Check that this caused a retransmission of I-Frame 1 only.
  EXPECT_EQ(4, tx_count);
  EXPECT_THAT(iframe_tx_counts, testing::ElementsAre(1, 2, 1, 0));

  // Receive an S-frame containing an acknowledgment up to seq = 3.
  StaticByteBuffer ack_3(0b1, 3);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        ack_3,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Initial transmission of I-Frame 3 after the acknowledgment freed up
  // transmit window capacity.
  EXPECT_EQ(5, tx_count);
  EXPECT_THAT(iframe_tx_counts, testing::ElementsAre(1, 2, 1, 1));

  // Receive an S-frame containing an acknowledgment up to seq = 3.
  StaticByteBuffer ack_4(0b1, 4);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        ack_4,
                                        FrameCheckSequenceOption::kIncludeFcs));

  EXPECT_EQ(5, tx_count);
}

// This tests the integration of receiving an acknowledgment sequence with
// triggering retransmission of unacknowledged I-Frames. This mirrors the L2CAP
// Test Specification L2CAP/ERM/BV-18-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       RetransmitAfterPollResponseDoesNotAcknowledgeSentFrames) {
  DynamicByteBuffer info_frame;
  int tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    // The first packet is the I-Frame containing the data that we sent.
    // The second packet is the S-Frame polling for the peer after the
    // Retransmission Timer expires. It is not checked to keep the tests less
    // fragile, as the first two packets are already covered by
    // EnhancedRetransmissionModeTxEngineTest.
    if (tx_count == 0) {
      info_frame = DynamicByteBuffer(*pdu);
    } else if (tx_count == 2) {
      EXPECT_TRUE(ContainersEqual(info_frame, *pdu));
    }
    tx_count++;
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer{'a'}));
  EXPECT_EQ(1, tx_count);

  RunFor(kErtmReceiverReadyPollTimerDuration);
  EXPECT_EQ(2, tx_count);

  // Receive an S-frame containing an acknowledgment not including the frame
  // that we transmitted. F is set. See Core Spec, v5, Vol 3, Part A,
  // Section 3.3.2, S-Frame Enhanced Control Field.
  StaticByteBuffer receiver_ready(0b1 | kExtendedControlFBitMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Check that this caused a retransmission of the initial I-Frame.
  EXPECT_EQ(3, tx_count);
}

// This test simulates the peer declaring that it is busy by sending the
// ReceiverNotReady S-Frame, which prevents us from retransmitting an
// unacknowledged outbound I-Frame. This mirrors L2CAP Test Specification v5.0.2
// L2CAP/ERM/BV-20-C.
TEST_F(EnhancedRetransmissionModeEnginesTest,
       DoNotRetransmitAfterReceivingReceiverNotReadyPollResponse) {
  int tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    ASSERT_TRUE(pdu);
    // Unlike the Test Spec's sequence diagram, we respond to the peer's I-Frame
    // with a Receiver Ready (which is always allowed), so our subsequent
    // I-Frame is actually the third outbound.
    if (tx_count == 0) {
      ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_information_frame());
      const auto& header = pdu->To<SimpleInformationFrameHeader>();
      EXPECT_EQ(0, header.tx_seq());
      EXPECT_EQ(0, header.receive_seq_num());
    } else if (tx_count == 1) {
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      ASSERT_TRUE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
    }
    tx_count++;
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(
                   StaticByteBuffer{'p', 'i', 'n', 'g'}));
  EXPECT_EQ(1, tx_count);

  // Send a poll request after timer expiry waiting for peer acknowledgment.
  RETURN_IF_FATAL(RunFor(kErtmReceiverReadyPollTimerDuration));
  EXPECT_EQ(2, tx_count);

  // Receive an Receiver Not Ready containing an acknowledgment not including
  // the frame that we transmitted. F is set. See Core Spec, v5, Vol 3, Part A,
  // Section 3.3.2, S-Frame Enhanced Control Field.
  StaticByteBuffer receiver_not_ready(
      0b1 | kExtendedControlReceiverNotReadyFunctionMask |
          kExtendedControlFBitMask,
      0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_not_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // RNR sets our RemoteBusy flag, so we should not transmit anything.
  EXPECT_EQ(2, tx_count);
}
// This tests that explicitly a requested selective retransmission and another
// for the same I-Frame that is a poll response causes only one retransmission.
// This mirrors the L2CAP Test Specification v5.0.2 L2CAP/ERM/BI-03-C.
TEST_F(
    EnhancedRetransmissionModeEnginesTest,
    RetransmitOnlyOnceAfterReceivingDuplicateSelectiveRejectSFramesForSameIFrameDuringReceiverReadyPoll) {
  int tx_count = 0;
  int iframe_0_tx_count = 0;
  int iframe_1_tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    tx_count++;

    // After outbound I-Frames, expect an outbound poll request S-Frame.
    if (tx_count == 3) {
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      ASSERT_TRUE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
      return;
    }

    ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
    ASSERT_TRUE(pdu->To<EnhancedControlField>().designates_information_frame());
    const auto& header = pdu->To<SimpleInformationFrameHeader>();
    if (header.tx_seq() == 0) {
      iframe_0_tx_count++;
    } else if (header.tx_seq() == 1) {
      iframe_1_tx_count++;
    }
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('a')));
  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('b')));
  EXPECT_EQ(2, tx_count);
  EXPECT_EQ(1, iframe_0_tx_count);
  EXPECT_EQ(1, iframe_1_tx_count);

  RunFor(kErtmReceiverReadyPollTimerDuration);
  EXPECT_EQ(3, tx_count);

  // Receive an S-frame containing a retransmission request for I-Frame 0. See
  // Core Spec v5.0, Vol 3, Part A, Section 3.3.2 S-Frame Enhanced Control
  // Field.
  StaticByteBuffer selective_reject(0b1 | kExtendedControlSrejFunctionMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        selective_reject,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Receive an S-frame containing a retransmission request for I-Frame 0 and a
  // poll response.
  StaticByteBuffer selective_reject_f(
      0b1 | kExtendedControlSrejFunctionMask | kExtendedControlFBitMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        selective_reject_f,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Check that these two SREJs caused a retransmission of I-Frame 0 only once.
  EXPECT_EQ(4, tx_count);
  EXPECT_EQ(2, iframe_0_tx_count);
  EXPECT_EQ(1, iframe_1_tx_count);

  // Acknowledge all of the outbound I-Frames per the specification's sequence
  // diagram.
  StaticByteBuffer receiver_ready_2(0b1, 2);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready_2,
                                        FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_EQ(4, tx_count);
}

// This tests that explicitly requested retransmission and receiving a poll
// response that doesn't acknowledge all I-Frames results in only a single set
// of retransmissions. This mirrors the L2CAP Test Specification v5.0.2
// L2CAP/ERM/BI-04-C.
TEST_F(
    EnhancedRetransmissionModeEnginesTest,
    RetransmitAfterReceivingRejectSFrameAndReceiverReadyPollResponseNotAcknowledgingSentFrames) {
  int tx_count = 0;
  int iframe_0_tx_count = 0;
  int iframe_1_tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    tx_count++;

    // After outbound I-Frames, expect an outbound poll request S-Frame.
    if (tx_count == 3) {
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      ASSERT_TRUE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
      return;
    }

    ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
    ASSERT_TRUE(pdu->To<EnhancedControlField>().designates_information_frame());
    const auto& header = pdu->To<SimpleInformationFrameHeader>();
    if (header.tx_seq() == 0) {
      iframe_0_tx_count++;
    } else if (header.tx_seq() == 1) {
      iframe_1_tx_count++;
    }
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('a')));
  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('b')));
  EXPECT_EQ(2, tx_count);
  EXPECT_EQ(1, iframe_0_tx_count);
  EXPECT_EQ(1, iframe_1_tx_count);

  RunFor(kErtmReceiverReadyPollTimerDuration);
  EXPECT_EQ(3, tx_count);

  // Receive an S-frame containing a retransmission request starting at seq = 0.
  // See Core Spec v5.0, Vol 3, Part A, Section 3.3.2 S-Frame Enhanced Control
  // Field.
  StaticByteBuffer reject(0b1 | kExtendedControlRejFunctionMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        reject,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Receive an S-frame containing an acknowledgment not including the frames
  // that we transmitted. F is set. See Core Spec v5.0, Vol 3, Part A,
  // Section 3.3.2 S-Frame Enhanced Control Field.
  StaticByteBuffer receiver_ready_0(0b1 | kExtendedControlFBitMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready_0,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Check that this caused a retransmission of the two I-Frames only once each.
  EXPECT_EQ(5, tx_count);
  EXPECT_EQ(2, iframe_0_tx_count);
  EXPECT_EQ(2, iframe_1_tx_count);

  // Acknowledge all of the outbound I-Frames per the specification's sequence
  // diagram.
  StaticByteBuffer receiver_ready_2(0b1, 2);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready_2,
                                        FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_EQ(5, tx_count);
}

// This tests that explicitly requested retransmission and receiving a I-Frame
// that doesn't acknowledge all I-Frames results in only a single set of
// retransmissions. This mirrors the L2CAP Test Specification v5.0.2
// L2CAP/ERM/BI-05-C in which we—the IUT—perform the ALT1 path in the sequence
// diagram Figure 4.96.
TEST_F(
    EnhancedRetransmissionModeEnginesTest,
    RetransmitAfterReceivingRejectSFrameAndIFramePollResponseNotAcknowledgingSentFrames) {
  int tx_count = 0;
  int iframe_0_tx_count = 0;
  int iframe_1_tx_count = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    tx_count++;
    SCOPED_TRACE(tx_count);

    if (tx_count == 3) {
      // After outbound I-Frames (not retransmitted), expect an outbound poll
      // request S-Frame.
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      EXPECT_TRUE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
      return;
    } else if (tx_count == 6) {
      // After retransmitted I-Frames, expect an outbound ReceiverReady that
      // acknowledges the peer's I-Frame.
      ASSERT_EQ(sizeof(SimpleSupervisoryFrame), pdu->size());
      ASSERT_TRUE(
          pdu->To<EnhancedControlField>().designates_supervisory_frame());
      EXPECT_FALSE(pdu->To<SimpleSupervisoryFrame>().is_poll_request());
      EXPECT_EQ(1, pdu->To<SimpleSupervisoryFrame>().receive_seq_num());
      return;
    }

    ASSERT_LE(sizeof(SimpleInformationFrameHeader), pdu->size());
    ASSERT_TRUE(pdu->To<EnhancedControlField>().designates_information_frame());
    const auto& header = pdu->To<SimpleInformationFrameHeader>();
    EXPECT_EQ(0, header.receive_seq_num());
    if (header.tx_seq() == 0) {
      iframe_0_tx_count++;
    } else if (header.tx_seq() == 1) {
      iframe_1_tx_count++;
    }
  });
  auto [rx_engine, tx_engine] =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  NoOpFailureCallback,
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('a')));
  TxProcessSdu(*tx_engine,
               std::make_unique<DynamicByteBuffer>(StaticByteBuffer('b')));
  EXPECT_EQ(2, tx_count);
  EXPECT_EQ(1, iframe_0_tx_count);
  EXPECT_EQ(1, iframe_1_tx_count);

  RunFor(kErtmReceiverReadyPollTimerDuration);
  EXPECT_EQ(3, tx_count);

  // Receive an S-frame containing a retransmission request starting at seq = 0.
  // See Core Spec v5.0, Vol 3, Part A, Section 3.3.2 S-Frame Enhanced Control
  // Field.
  StaticByteBuffer reject(0b1 | kExtendedControlRejFunctionMask, 0);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        reject,
                                        FrameCheckSequenceOption::kIncludeFcs));

  // Receive an I-frame containing an acknowledgment not including the frames
  // that we transmitted. F is set. See Core Spec v5.0, Vol 3, Part A,
  // Section 3.3.2 I-Frame Enhanced Control Field.
  StaticByteBuffer inbound_iframe(kExtendedControlFBitMask, 0, 'A');
  auto inbound_pdu = rx_engine->ProcessPdu(
      Fragmenter(kTestHandle)
          .BuildFrame(kTestChannelId,
                      inbound_iframe,
                      FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_TRUE(inbound_pdu);

  // Check that this caused a retransmission of the two I-Frames only once each,
  // plus our acknowledgment of the peer I-Frame.
  EXPECT_EQ(6, tx_count);
  EXPECT_EQ(2, iframe_0_tx_count);
  EXPECT_EQ(2, iframe_1_tx_count);

  // Acknowledge all of the outbound I-Frames per the specification's sequence
  // diagram.
  StaticByteBuffer receiver_ready(0b1, 2);
  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready,
                                        FrameCheckSequenceOption::kIncludeFcs));
  EXPECT_EQ(6, tx_count);
}

TEST_F(
    EnhancedRetransmissionModeEnginesTest,
    ProcessSupervisoryFrameWithBothPollAndFinalBitsSetSynchronouslyDestroysEngines) {
  std::unique_ptr<RxEngine> rx_engine;
  std::unique_ptr<TxEngine> tx_engine;

  auto failure_cb = [&] {
    rx_engine.reset();
    tx_engine.reset();
  };

  std::tie(rx_engine, tx_engine) =
      MakeLinkedEnhancedRetransmissionModeEngines(kTestChannelId,
                                                  kDefaultMTU,
                                                  kMaxTransmissions,
                                                  kTxWindow,
                                                  channel(),
                                                  std::move(failure_cb),
                                                  dispatcher());
  ASSERT_TRUE(rx_engine);
  ASSERT_TRUE(tx_engine);

  // Supervisory frame (bit 0 = 1), function = RR (00), P = 1 (bit 4), F = 1
  // (bit 7). Raw byte value: 0b10010001 = 0x91. Second byte: 0x00 (ReqSeq = 0).
  StaticByteBuffer receiver_ready_poll_and_final(0x91, 0x00);

  rx_engine->ProcessPdu(Fragmenter(kTestHandle)
                            .BuildFrame(kTestChannelId,
                                        receiver_ready_poll_and_final,
                                        FrameCheckSequenceOption::kIncludeFcs));
}

}  // namespace
}  // namespace bt::l2cap::internal
