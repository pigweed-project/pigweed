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

#include <pw_async/fake_dispatcher_fixture.h>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_tx_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/frame_headers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::l2cap::internal {
namespace {

constexpr ChannelId kTestChannelId = 0x0001;

using TxEngine = EnhancedRetransmissionModeTxEngine;

class EnhancedRetransmissionModeTxEngineTest
    : public pw::async::test::FakeDispatcherFixture {
 public:
  EnhancedRetransmissionModeTxEngineTest()
      : kDefaultPayload('h', 'e', 'l', 'l', 'o') {}

 protected:
  // The default values are provided for use by tests which don't depend on the
  // specific value a given parameter. This should make the tests easier to
  // read, because the reader can focus on only the non-defaulted parameter
  // values.
  static constexpr auto kDefaultMTU = bt::l2cap::kDefaultMTU;
  static constexpr size_t kDefaultMaxTransmissions = 1;
  static constexpr size_t kDefaultTxWindow = 63;

  const StaticByteBuffer<5> kDefaultPayload;

  void VerifyIsReceiverReadyPollFrame(ByteBuffer* buf) {
    ASSERT_TRUE(buf);
    ASSERT_EQ(sizeof(SimpleSupervisoryFrame), buf->size());

    const auto sframe = buf->To<SimpleSupervisoryFrame>();
    EXPECT_EQ(SupervisoryFunction::ReceiverReady, sframe.function());
    EXPECT_TRUE(sframe.is_poll_request());
  }

  // Helper to queue an SDU to the channel and notify the engine.
  void QueueSdu(TxEngine& engine, ByteBufferPtr sdu) {
    channel_.QueueSdu(std::move(sdu));
    engine.NotifySduQueued();
  }

  FakeTxChannel& channel() { return channel_; }

 private:
  FakeTxChannel channel_;
};

void NoOpFailureCallback() {}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduTransmitsMinimalSizedSdu) {
  ByteBufferPtr last_pdu;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });

  constexpr size_t kMtu = 10;
  const StaticByteBuffer payload(1);
  TxEngine tx_engine(kTestChannelId,
                     kMtu,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
  EXPECT_EQ(1u, n_pdus);
  ASSERT_TRUE(last_pdu);

  // See Core Spec v5.0, Volume 3, Part A, Table 3.2.
  const StaticByteBuffer expected_pdu(0,   // Final Bit, TxSeq, MustBeZeroBit
                                      0,   // SAR bits, ReqSeq
                                      1);  // Payload
  EXPECT_TRUE(ContainersEqual(expected_pdu, *last_pdu));
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduTransmitsMaximalSizedSdu) {
  ByteBufferPtr last_pdu;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });

  constexpr size_t kMtu = 1;
  const StaticByteBuffer payload(1);
  TxEngine tx_engine(kTestChannelId,
                     kMtu,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
  EXPECT_EQ(1u, n_pdus);
  ASSERT_TRUE(last_pdu);

  // See Core Spec v5.0, Volume 3, Part A, Table 3.2.
  const StaticByteBuffer expected_pdu(0,   // Final Bit, TxSeq, MustBeZeroBit
                                      0,   // SAR bits, ReqSeq
                                      1);  // Payload
  EXPECT_TRUE(ContainersEqual(expected_pdu, *last_pdu));
}

TEST_F(EnhancedRetransmissionModeTxEngineTest, QueueSduSurvivesOversizedSdu) {
  // TODO(fxbug.dev/42054330): Update this test when we add support for
  // segmentation.
  constexpr size_t kMtu = 1;
  TxEngine tx_engine(kTestChannelId,
                     kMtu,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  QueueSdu(tx_engine,
           std::make_unique<DynamicByteBuffer>(StaticByteBuffer(1, 2)));
}

TEST_F(EnhancedRetransmissionModeTxEngineTest, QueueSduSurvivesZeroByteSdu) {
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest, QueueSduAdvancesSequenceNumber) {
  const StaticByteBuffer payload(1);
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  {
    // See Core Spec v5.0, Volume 3, Part A, Table 3.2.
    const StaticByteBuffer expected_pdu(0,   // Final Bit, TxSeq, MustBeZeroBit
                                        0,   // SAR bits, ReqSeq
                                        1);  // Payload

    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
    ASSERT_TRUE(last_pdu);
    EXPECT_TRUE(ContainersEqual(expected_pdu, *last_pdu));
  }

  {
    // See Core Spec v5.0, Volume 3, Part A, Table 3.2.
    const StaticByteBuffer expected_pdu(
        1 << 1,  // Final Bit, TxSeq=1, MustBeZeroBit
        0,       // SAR bits, ReqSeq
        1);      // Payload
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
    ASSERT_TRUE(last_pdu);
    EXPECT_TRUE(ContainersEqual(expected_pdu, *last_pdu));
  }

  {
    // See Core Spec v5.0, Volume 3, Part A, Table 3.2.
    const StaticByteBuffer expected_pdu(
        2 << 1,  // Final Bit, TxSeq=2, MustBeZeroBit
        0,       // SAR bits, ReqSeq
        1);      // Payload
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
    ASSERT_TRUE(last_pdu);
    EXPECT_TRUE(ContainersEqual(expected_pdu, *last_pdu));
  }
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduRollsOverSequenceNumber) {
  constexpr size_t kTxWindow = 63;  // Max possible value
  const StaticByteBuffer payload(1);
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  constexpr size_t kMaxSeq = 64;
  for (size_t i = 0; i < kMaxSeq; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
    tx_engine.UpdateAckSeq((i + 1) % kMaxSeq, /*is_poll_response=*/false);
  }

  // See Core Spec v5.0, Volume 3, Part A, Table 3.2.
  const StaticByteBuffer expected_pdu(
      0,   // Final Bit, TxSeq (rolls over from 63 to 0), MustBeZeroBit
      0,   // SAR bits, ReqSeq
      1);  // Payload
  last_pdu = nullptr;
  // Free up space for more transmissions. We need room for the 64th frame from
  // above (since the TxWindow is 63), and the new 0th frame. Hence we
  // acknowledge original frames 0 and 1.
  tx_engine.UpdateAckSeq(2, /*is_poll_response=*/false);
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(payload));
  ASSERT_TRUE(last_pdu);
  EXPECT_TRUE(ContainersEqual(expected_pdu, *last_pdu));
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduDoesNotTransmitBeyondTxWindow) {
  constexpr size_t kTxWindow = 1;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(1u, n_pdus);

  n_pdus = 0;
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  EXPECT_EQ(0u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduDoesNotTransmitBeyondTxWindowEvenIfQueueWrapsSequenceNumbers) {
  constexpr size_t kTxWindow = 1;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(1u, n_pdus);

  constexpr size_t kMaxSeq = 64;
  n_pdus = 0;
  for (size_t i = 0; i < kMaxSeq; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
    ASSERT_EQ(0u, n_pdus);
  }
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineTransmitsReceiverReadyPollAfterTimeout) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  SCOPED_TRACE("");
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineTransmitsReceiverReadyPollOnlyOnceAfterTimeout) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  SCOPED_TRACE("");
  RETURN_IF_FATAL(VerifyIsReceiverReadyPollFrame(last_pdu.get()));
  last_pdu = nullptr;

  // Note: This value is chosen to be at least as long as
  // kReceiverReadyPollTimerDuration, but shorter than kMonitorTimerDuration.
  EXPECT_FALSE(RunFor(std::chrono::seconds(2)));  // No tasks were run.
  EXPECT_FALSE(last_pdu);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineAdvancesReceiverReadyPollTimeoutOnNewTransmission) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  ASSERT_FALSE(RunFor(std::chrono::seconds(1)));  // No events should fire.
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  last_pdu = nullptr;

  ASSERT_FALSE(
      RunFor(std::chrono::seconds(1)));  // Original timeout should not fire.
  ASSERT_TRUE(RunFor(std::chrono::seconds(1)));  // New timeout should fire.
  SCOPED_TRACE("");
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       ReceiverReadyPollIncludesRequestSequenceNumber) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  tx_engine.UpdateReqSeq(1);
  RunUntilIdle();
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  last_pdu = nullptr;

  SCOPED_TRACE("");
  EXPECT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_NO_FATAL_FAILURE(VerifyIsReceiverReadyPollFrame(last_pdu.get()));
  EXPECT_EQ(1u, last_pdu->To<SimpleSupervisoryFrame>().receive_seq_num());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       AckOfOnlyOutstandingFrameCancelsReceiverReadyPollTimeout) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();

  EXPECT_FALSE(RunFor(std::chrono::seconds(2)));  // No tasks were run.
  EXPECT_FALSE(last_pdu);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       AckOfAllOutstandingFramesCancelsReceiverReadyPollTimeout) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  tx_engine.UpdateAckSeq(3, /*is_poll_response=*/false);
  RunUntilIdle();

  EXPECT_FALSE(RunFor(std::chrono::seconds(2)));  // No tasks were run.
  EXPECT_FALSE(last_pdu);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       PartialAckDoesNotCancelReceiverReadyPollTimeout) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();

  // See Core Spec v5.0, Volume 3, Part A, Sec 8.6.5.6, under heading
  // Process-ReqSeq. We should only Stop-RetransTimer if UnackedFrames is 0.
  SCOPED_TRACE("");
  EXPECT_TRUE(RunFor(std::chrono::seconds(2)));
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       NewTransmissionAfterAckedFrameReArmsReceiverReadyPollTimeout) {
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  // Send a frame, and get the ACK.
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();

  // Send a new frame.
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  last_pdu = nullptr;

  // Having earlier received an ACK for the previous frame should not have left
  // around any state that would prevent us from sending a receiver-ready poll
  // for the second frame.
  SCOPED_TRACE("");
  EXPECT_TRUE(RunFor(std::chrono::seconds(2)));
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineRetransmitsReceiverReadyPollAfterMonitorTimeout) {
  constexpr size_t kMaxTransmissions = 2;  // Allow retransmission
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // First the receiver_ready_poll_task_ fires.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  // Then the monitor_task_ fires.
  EXPECT_TRUE(RunFor(std::chrono::seconds(12)));
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineDoesNotRetransmitReceiverReadyPollAfterMonitorTimeoutWhenRetransmissionsAreDisabled) {
  constexpr size_t kMaxTransmissions = 1;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // First the receiver_ready_poll_task_ fires.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_TRUE(last_pdu);
  last_pdu = nullptr;

  // Run the event loop long enough for the monitor task to fire again. Because
  // kMaxTransmissions == 1, the ReceiverReadyPoll should not be retransmitted.
  RunFor(std::chrono::seconds(13));
  EXPECT_FALSE(last_pdu);
}

// See Core Spec v5.0, Volume 3, Part A, Sec 5.4, Table 8.6.5.8, for the row
// with "Recv ReqSeqAndFbit" and "F = 1".
TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineStopsPollingReceiverReadyFromMonitorTaskAfterReceivingFinalUpdateForAckSeq) {
  constexpr size_t kMaxTransmissions = 3;  // Allow multiple retransmissions
  ByteBufferPtr last_pdu;
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));   // receiver_ready_poll_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/true);
  EXPECT_FALSE(RunFor(std::chrono::seconds(13)));  // No other tasks.
}

// See Core Spec v5.0, Volume 3, Part A, Sec 5.4, Table 8.6.5.8, for the row
// with "Recv ReqSeqAndFbit" and "F = 0".
TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineContinuesPollingReceiverReadyFromMonitorTaskAfterReceivingNonFinalUpdateForAckSeq) {
  constexpr size_t kMaxTransmissions = 2;  // Allow retransmissions
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));   // receiver_ready_poll_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  EXPECT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineRetransmitsReceiverReadyPollAfterMultipleMonitorTimeouts) {
  constexpr size_t kMaxTransmissions = 3;  // Allow multiple retransmissions
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));   // receiver_ready_poll_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  ASSERT_FALSE(
      RunFor(std::chrono::seconds(2)));  // RR-poll task does _not_ fire
  last_pdu = nullptr;

  EXPECT_TRUE(RunFor(std::chrono::seconds(10)));  // monitor_task_ again
  VerifyIsReceiverReadyPollFrame(last_pdu.get());
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineRetransmitsReceiverReadyPollIndefinitelyAfterMonitorTimeoutWhenMaxTransmitsIsZero) {
  constexpr size_t kMaxTransmissions = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // First the receiver_ready_poll_task_ fires.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  EXPECT_TRUE(last_pdu);
  last_pdu = nullptr;

  // Then the monitor_task_ fires.
  EXPECT_TRUE(RunFor(std::chrono::seconds(12)));
  EXPECT_TRUE(last_pdu);
  last_pdu = nullptr;

  // And the monitor_task_ fires again.
  EXPECT_TRUE(RunFor(std::chrono::seconds(12)));
  EXPECT_TRUE(last_pdu);
  last_pdu = nullptr;

  // And the monitor_task_ fires yet again.
  EXPECT_TRUE(RunFor(std::chrono::seconds(12)));
  EXPECT_TRUE(last_pdu);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineStopsTransmittingReceiverReadyPollAfterMaxTransmits) {
  constexpr size_t kMaxTransmissions = 2;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));   // receiver_ready_poll_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  last_pdu = nullptr;

  EXPECT_FALSE(RunFor(std::chrono::seconds(13)));
  EXPECT_FALSE(last_pdu);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineClosesChannelAfterMaxTransmitsOfReceiverReadyPoll) {
  constexpr size_t kMaxTransmissions = 2;
  bool connection_failed = false;
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));   // receiver_ready_poll_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  EXPECT_TRUE(connection_failed);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineClosesChannelAfterMaxTransmitsOfReceiverReadyPollEvenIfRetransmissionsAreDisabled) {
  constexpr size_t kMaxTransmissions = 1;
  bool connection_failed = false;
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));   // receiver_ready_poll_task_
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));  // monitor_task_
  EXPECT_TRUE(connection_failed);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineClosesChannelAfterMaxTransmitsOfIFrame) {
  constexpr size_t kMaxTransmissions = 2;
  size_t num_info_frames_sent = 0;
  bool connection_failed = false;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame()) {
      ++num_info_frames_sent;
    }
  });
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(1u, num_info_frames_sent);

  // Not having received an acknowledgement after 2 seconds,
  // receiver_ready_poll_task_ will fire, and cause us to send a
  // ReceiverReadyPoll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // The peer indicates that it has not received any frames. This causes us to
  // retransmit the frame.
  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  EXPECT_EQ(2u, num_info_frames_sent);

  // Not having received an acknowledgement after 2 seconds,
  // receiver_ready_poll_task_ will fire, and cause us to send another
  // ReceiverReadyPoll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // The connection should remain open, to allow the peer time to respond to our
  // poll, and acknowledge the outstanding frame.
  EXPECT_FALSE(connection_failed);

  // The peer again indicates that it has not received any frames.
  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);

  // Because we've exhausted kMaxTransmissions, the connection will be closed.
  EXPECT_TRUE(connection_failed);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineExhaustsAllRetransmissionsOfIFrameBeforeClosingChannel) {
  constexpr size_t kMaxTransmissions = 255;
  size_t num_info_frames_sent = 0;
  bool connection_failed = false;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame()) {
      ++num_info_frames_sent;
    }
  });
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  for (size_t i = 0; i < kMaxTransmissions; ++i) {
    // Not having received an acknowledgement after 2 seconds,
    // receiver_ready_poll_task_ will fire, and cause us to send a
    // ReceiverReadyPoll.
    ASSERT_TRUE(RunFor(std::chrono::seconds(2))) << "(i=" << i << ")";

    // The connection should remain open, to allow the peer time to respond to
    // our poll, and acknowledge the outstanding frame.
    EXPECT_FALSE(connection_failed);

    // The peer indicates that it has not received any frames.
    tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  }

  // The connection is closed, and we've exhausted kMaxTransmissions.
  EXPECT_TRUE(connection_failed);
  EXPECT_EQ(255u, num_info_frames_sent);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineRetransmitsIFrameIndefinitelyWhenMaxTransmitsIsZero) {
  constexpr size_t kMaxTransmissions = 0;
  constexpr size_t kTxWindow = 2;
  size_t num_info_frames_sent = 0;
  bool connection_failed = false;
  ByteBufferPtr last_tx_frame = nullptr;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame()) {
      ++num_info_frames_sent;
      last_tx_frame = std::move(pdu);
    }
  });
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  EXPECT_EQ(1u, num_info_frames_sent);

  // This test should trigger enough transmissions to overflow an 8-bit transmit
  // counter.
  const auto kNumTransmissions =
      size_t{2} * std::numeric_limits<uint8_t>::max();

  // Retransmissions after the initial one triggered by QueueSdu
  for (size_t i = 0; i < kNumTransmissions - 1; ++i) {
    // Not having received an acknowledgement after 2 seconds,
    // receiver_ready_poll_task_ will fire, and cause us to send a
    // ReceiverReadyPoll.
    ASSERT_TRUE(RunFor(std::chrono::seconds(2))) << "(i=" << i << ")";

    // The connection should remain open, to allow the peer time to respond to
    // our poll, and acknowledge the outstanding frame.
    EXPECT_FALSE(connection_failed);

    // The peer indicates that it has not received any frames.
    tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  }

  // The connection is still open and we have transmitted more times than the
  // greatest possible finite MaxTransmit option (which is 8-bit).
  EXPECT_FALSE(connection_failed);
  EXPECT_EQ(kNumTransmissions, num_info_frames_sent);

  // Check that the outbound frame's transmit count hasn't overflowed to zero by
  // transmitting a different payload.
  ASSERT_TRUE(ContainersEqual(
      kDefaultPayload, last_tx_frame->view(sizeof(EnhancedControlField))));
  num_info_frames_sent = 0;

  // If the first frame's transmit count had overflowed to zero, it would get
  // sent out together with this new SDU because it would be indistinguishable
  // from "never transmitted."
  QueueSdu(tx_engine,
           std::make_unique<DynamicByteBuffer>(StaticByteBuffer('@')));
  EXPECT_EQ(1u, num_info_frames_sent);
  EXPECT_EQ('@', (*last_tx_frame)[sizeof(EnhancedControlField)]);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineClosesChannelAfterMaxTransmitsOfIFrameEvenIfRetransmissionsAreDisabled) {
  constexpr size_t kMaxTransmissions = 1;
  bool connection_failed = false;
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // Not having received an acknowledgement after 2 seconds,
  // receiver_ready_poll_task_ will fire, and cause us to send a
  // ReceiverReadyPoll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // The connection should remain open, to allow the peer time to respond to our
  // poll, and acknowledge the outstanding frame.
  EXPECT_FALSE(connection_failed);

  // The peer indicates that it has not received any frames.
  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  EXPECT_TRUE(connection_failed);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineRetransmitsMissingFrameOnPollResponse) {
  constexpr size_t kMaxTransmissions = 2;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) { last_pdu = std::move(pdu); });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  last_pdu = nullptr;

  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));  // receiver_ready_poll_task_
  last_pdu = nullptr;

  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  ASSERT_TRUE(
      last_pdu->To<EnhancedControlField>().designates_information_frame());
  EXPECT_EQ(0u, last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineRetransmitsAllMissingFramesOnPollResponse) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 63;
  size_t n_pdus = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  // Send a TxWindow's worth of frames.
  for (size_t i = 0; i < kTxWindow; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, and clear out accumulated callback
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  n_pdus = 0;
  last_pdu = nullptr;

  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  EXPECT_EQ(kTxWindow, n_pdus);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  ASSERT_TRUE(
      last_pdu->To<EnhancedControlField>().designates_information_frame());
  EXPECT_EQ(kTxWindow - 1,
            last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineRetransmitsAllMissingFramesOnPollResponseWithWrappedSequenceNumber) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 63;
  size_t n_pdus = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  // Send a TxWindow's worth of frames.
  for (size_t i = 0; i < kTxWindow; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }

  // Acknowledge the first 32 of these frames (with sequence numbers 0...31).
  tx_engine.UpdateAckSeq(32, /*is_poll_response=*/false);

  // Queue 32 new frames (with sequence numbers 63, 0 ... 30).
  for (size_t i = 0; i < 32; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, and then clear out accumulated callback
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  n_pdus = 0;
  last_pdu = nullptr;

  // Repeat the earlier acknowledgement. This indicates that the peer has
  // not received frame 32.
  tx_engine.UpdateAckSeq(32, /*is_poll_response=*/true);

  // We expect to retransmit frames 32...63, and then 0...30. That's 63 frames
  // in total.
  EXPECT_EQ(63u, n_pdus);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  ASSERT_TRUE(
      last_pdu->To<EnhancedControlField>().designates_information_frame());
  EXPECT_EQ(30u, last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineProperlyHandlesPartialAckWithWrappedSequenceNumber) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 63;
  size_t n_pdus = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  // Send a TxWindow's worth of frames.
  for (size_t i = 0; i < kTxWindow; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }

  // Acknowledge the first 62 of these frames (with sequence numbers 0...61).
  tx_engine.UpdateAckSeq(62, /*is_poll_response=*/false);

  // Queue 62 new frames. These frames have sequence numebrs 63, 0...60.
  // (Sequence number 62 was used when we queued the first batch of frames
  // above.)
  for (size_t i = 0; i < 62; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, and then clear out accumulated callback
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  n_pdus = 0;
  last_pdu = nullptr;

  // Acknowledge an additional 5 frames (with sequence numbers 62, 63, 0, 1, 2).
  tx_engine.UpdateAckSeq(3, /*is_poll_response=*/true);

  // Verify that all unacknowledged frames are retransmitted.
  EXPECT_EQ(58u, n_pdus);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  ASSERT_TRUE(
      last_pdu->To<EnhancedControlField>().designates_information_frame());
  EXPECT_EQ(60u, last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineDoesNotRetransmitFramesBeyondTxWindow) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 32;
  size_t n_pdus = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  // Queue two TxWindow's worth of frames. These have sequence numbers 0...63.
  for (size_t i = 0; i < 2 * kTxWindow; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, and clear out accumulated callback
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  n_pdus = 0;
  last_pdu = nullptr;

  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  EXPECT_EQ(kTxWindow, n_pdus);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  EXPECT_EQ(kTxWindow - 1,
            last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineDoesNotRetransmitFramesBeyondTxWindowWhenWindowWraps) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 48;
  size_t n_pdus = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  // Queue one TxWindow's worth of frames. This advances the sequence numbers,
  // so that further transmissions can wrap.
  for (size_t i = 0; i < 48; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  tx_engine.UpdateAckSeq(48, /*is_poll_response=*/false);
  RunUntilIdle();

  // Queue another TxWindow's worth of frames. These have sequence
  // numbers 48..63, and 0..31. These _should_ be retransmitted at the next
  // UpdateAckSeq() call.
  for (size_t i = 0; i < 48; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();

  // Queue a few more frames, with sequence numbers 32..39. These should _not_
  // be retransmitted at the next UpdateAckSeq() call.
  for (size_t i = 0; i < 8; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, and clear out accumulated callback
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  n_pdus = 0;
  last_pdu = nullptr;

  // Report that the peer has not received frame 48. This should trigger
  // retranmissions of unacknowledged frames within the TxWindow.
  tx_engine.UpdateAckSeq(48, /*is_poll_response=*/true);

  // We expect to retransmit frames 48..63 and 0..31. The other frames are
  // beyond the transmit window.
  EXPECT_EQ(48u, n_pdus);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  ASSERT_TRUE(
      last_pdu->To<EnhancedControlField>().designates_information_frame());
  EXPECT_EQ(31u, last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineDoesNotRetransmitPreviouslyAckedFramesOnPollResponse) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 2;
  size_t n_pdus = 0;
  ByteBufferPtr last_pdu;
  channel().HandleSendFrame([&](auto pdu) {
    ++n_pdus;
    last_pdu = std::move(pdu);
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, and clear out accumulated callback
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  n_pdus = 0;
  last_pdu = nullptr;

  constexpr size_t kPollResponseReqSeq = 1;
  tx_engine.UpdateAckSeq(kPollResponseReqSeq, /*is_poll_response=*/true);
  EXPECT_EQ(kTxWindow - kPollResponseReqSeq, n_pdus);
  ASSERT_TRUE(last_pdu);
  ASSERT_GE(last_pdu->size(), sizeof(SimpleInformationFrameHeader));
  ASSERT_TRUE(
      last_pdu->To<EnhancedControlField>().designates_information_frame());
  EXPECT_EQ(1, last_pdu->To<SimpleInformationFrameHeader>().tx_seq());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       AckOfFrameWithNoneOutstandingClosesChannel) {
  bool connection_failed = false;
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kDefaultMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  EXPECT_TRUE(connection_failed);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       AckOfMoreFramesThanAreOutstandingClosesChannel) {
  bool connection_failed = false;
  TxEngine tx_engine(
      kTestChannelId,
      kDefaultMTU,
      kDefaultMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] { connection_failed = true; },
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  tx_engine.UpdateAckSeq(2, /*is_poll_response=*/true);
  EXPECT_TRUE(connection_failed);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineDoesNotCrashOnSpuriousAckAfterValidAck) {
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/true);
  tx_engine.UpdateAckSeq(2, /*is_poll_response=*/true);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       EngineDoesNotCrashOnSpuriousAckBeforeAnyDataHasBeenSent) {
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  for (uint8_t i = 0; i <= EnhancedControlField::kMaxSeqNum; ++i) {
    tx_engine.UpdateAckSeq(i, /*is_poll_response=*/true);
  }
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduDoesNotTransmitFramesWhenRemoteIsBusy) {
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.SetRemoteBusy();
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  EXPECT_EQ(0u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       UpdateAckSeqTransmitsQueuedDataWhenPossible) {
  constexpr size_t kTxWindow = 1;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(1u, n_pdus);

  n_pdus = 0;
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();
  EXPECT_EQ(1u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       UpdateAckSeqTransmissionOfQueuedDataRespectsTxWindow) {
  constexpr size_t kTxWindow = 1;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(1u, n_pdus);

  n_pdus = 0;
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();
  EXPECT_EQ(1u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       NonFinalUpdateAckSeqDoesNotTransmitQueuedFramesWhenRemoteIsBusy) {
  constexpr size_t kTxWindow = 1;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(1u, n_pdus);

  n_pdus = 0;
  tx_engine.SetRemoteBusy();
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();
  EXPECT_EQ(0u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       FinalUpdateAckSeqDoesNotTransmitQueuedFramesWhenRemoteIsBusy) {
  constexpr size_t kTxWindow = 1;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(1u, n_pdus);

  n_pdus = 0;
  tx_engine.SetRemoteBusy();
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/true);
  RunUntilIdle();
  EXPECT_EQ(0u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       MaybeSendQueuedDataTransmitsAllQueuedFramesWithinTxWindow) {
  constexpr size_t kTxWindow = 63;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.SetRemoteBusy();
  for (size_t i = 0; i < kTxWindow; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();
  ASSERT_EQ(0u, n_pdus);

  tx_engine.ClearRemoteBusy();
  tx_engine.MaybeSendQueuedData();
  RunUntilIdle();
  EXPECT_EQ(kTxWindow, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       MaybeSendQueuedDataDoesNotTransmitBeyondTxWindow) {
  constexpr size_t kTxWindow = 32;
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.SetRemoteBusy();
  for (size_t i = 0; i < kTxWindow + 1; ++i) {
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }
  RunUntilIdle();
  ASSERT_EQ(0u, n_pdus);

  tx_engine.ClearRemoteBusy();
  tx_engine.MaybeSendQueuedData();
  RunUntilIdle();
  EXPECT_EQ(kTxWindow, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       MaybeSendQueuedDataRespectsRemoteBusy) {
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.SetRemoteBusy();
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(0u, n_pdus);

  tx_engine.MaybeSendQueuedData();
  RunUntilIdle();
  EXPECT_EQ(0u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       MaybeSendQueuedDataDoesNotCrashWhenCalledWithoutPendingPdus) {
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  tx_engine.MaybeSendQueuedData();
  RunUntilIdle();
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduCanSendMoreFramesAfterClearingRemoteBusy) {
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.SetRemoteBusy();
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(0u, n_pdus);

  tx_engine.ClearRemoteBusy();
  tx_engine.MaybeSendQueuedData();
  RunUntilIdle();
  ASSERT_EQ(1u, n_pdus);
  n_pdus = 0;

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  EXPECT_EQ(1u, n_pdus);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduMaintainsSduOrderingAfterClearRemoteBusy) {
  std::vector<uint8_t> pdu_seq_numbers;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      pdu_seq_numbers.push_back(
          pdu->To<SimpleInformationFrameHeader>().tx_seq());
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.SetRemoteBusy();
  QueueSdu(tx_engine,
           std::make_unique<DynamicByteBuffer>(kDefaultPayload));  // seq=0
  RunUntilIdle();
  ASSERT_TRUE(pdu_seq_numbers.empty());

  tx_engine.ClearRemoteBusy();
  QueueSdu(tx_engine,
           std::make_unique<DynamicByteBuffer>(kDefaultPayload));  // seq=1
  RunUntilIdle();

  // This requirement isn't in the specification directly. But it seems
  // necessary given that we can sometimes exit the remote-busy condition
  // without transmitting the queued data. See Core Spec v5.0, Volume 3, Part A,
  // Table 8.7, row for "Recv RR (P=1)" for an example of such an operation.
  EXPECT_EQ((std::vector<uint8_t>{0, 1}), pdu_seq_numbers);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       UpdateAckSeqRetransmitsUnackedFramesBeforeTransmittingQueuedFrames) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 63;
  std::vector<uint8_t> pdu_seq_numbers;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      pdu_seq_numbers.push_back(
          pdu->To<SimpleInformationFrameHeader>().tx_seq());
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());
  EXPECT_TRUE(tx_engine.IsQueueEmpty());

  // Send out two frames.
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_EQ(2u, pdu_seq_numbers.size());
  EXPECT_EQ((std::vector{uint8_t{0}, uint8_t{1}}), pdu_seq_numbers);
  EXPECT_FALSE(tx_engine.IsQueueEmpty());
  pdu_seq_numbers.clear();

  // Indicate the remote is busy, and queue a third frame.
  tx_engine.SetRemoteBusy();
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  ASSERT_TRUE(pdu_seq_numbers.empty());

  // Clear the busy condition, and then report the new ack sequence number.
  // Because the ack only acknowledges the first frame, the second frame should
  // be retransmitted. And that retransmission should come before the (initial)
  // transmission of the third frame.
  tx_engine.ClearRemoteBusy();
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/true);
  RunUntilIdle();
  EXPECT_EQ((std::vector{uint8_t{1}, uint8_t{2}}), pdu_seq_numbers);
  EXPECT_FALSE(tx_engine.IsQueueEmpty());

  tx_engine.UpdateAckSeq(3, /*is_poll_response=*/false);
  RunUntilIdle();
  EXPECT_TRUE(tx_engine.IsQueueEmpty());
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       QueueSduDoesNotTransmitNewFrameWhenEngineIsAwaitingPollResponse) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 3;
  std::vector<uint8_t> pdu_seq_numbers;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      pdu_seq_numbers.push_back(
          pdu->To<SimpleInformationFrameHeader>().tx_seq());
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire. This moves the engine into the 'WAIT_F'
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Queue a new frame.
  pdu_seq_numbers.clear();
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  EXPECT_EQ(std::vector<uint8_t>(), pdu_seq_numbers);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    NonFinalUpdateAckSeqDoesNotTransmitNewFrameWhenEngineIsAwaitingPollResponse) {
  constexpr size_t kMaxTransmissions = 2;
  constexpr size_t kTxWindow = 1;
  std::vector<uint8_t> pdu_seq_numbers;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      pdu_seq_numbers.push_back(
          pdu->To<SimpleInformationFrameHeader>().tx_seq());
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire. This moves the engine into the 'WAIT_F'
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Acknowledge the first frame, making room for the transmission of the second
  // frame.
  pdu_seq_numbers.clear();
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/false);
  RunUntilIdle();

  // Because we're still in the WAIT_F state, the second frame should _not_ be
  // transmitted.
  EXPECT_EQ(pdu_seq_numbers.end(),
            std::find(pdu_seq_numbers.begin(), pdu_seq_numbers.end(), 1));
}

// Note: to make the most of this test, the unit tests should be built with
// ASAN.
TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineDoesNotCrashIfExhaustionOfMaxTransmitForIFrameCausesEngineDestruction) {
  constexpr size_t kMaxTransmissions = 1;
  constexpr size_t kTxWindow = 2;
  bool connection_failed = false;
  std::unique_ptr<TxEngine> tx_engine = std::make_unique<TxEngine>(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kTxWindow,
      channel(),
      [&] {
        connection_failed = true;
        tx_engine.reset();
      },
      dispatcher());

  // Queue three SDUs, of which two should be transmitted immediately.
  QueueSdu(*tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(*tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(*tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire. This moves the engine into the 'WAIT_F'
  // state.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Acknowledge the first frame, making room for the transmission of the third
  // frame. If the code is buggy, we may encounter use-after-free errors here.
  tx_engine->UpdateAckSeq(1, /*is_poll_response=*/true);
  RunUntilIdle();

  // Because we only allow one transmission, the connection should have failed.
  EXPECT_TRUE(connection_failed);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    EngineDoesNotCrashIfExhaustionOfMaxTransmitForReceiverReadyPollCausesEngineDestruction) {
  constexpr size_t kMaxTransmissions = 1;
  bool connection_failed = false;
  std::unique_ptr<TxEngine> tx_engine = std::make_unique<TxEngine>(
      kTestChannelId,
      kDefaultMTU,
      kMaxTransmissions,
      kDefaultTxWindow,
      channel(),
      [&] {
        connection_failed = true;
        tx_engine.reset();
      },
      dispatcher());

  QueueSdu(*tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Let monitor_task_ fire, to attempt retransmission of the poll. The
  // retransmission should fail, because we have exhausted kMaxTransmissions. If
  // the code is buggy, we may encounter use-after-free errors here.
  ASSERT_TRUE(RunFor(std::chrono::seconds(12)));

  EXPECT_TRUE(connection_failed);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       TransmissionOfPduIncludesRequestSeqNum) {
  uint8_t outbound_req_seq = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      outbound_req_seq =
          pdu->To<SimpleInformationFrameHeader>().receive_seq_num();
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  tx_engine.UpdateReqSeq(5);
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  EXPECT_EQ(5u, outbound_req_seq);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       DeferredTransmissionOfPduIncludesCurrentRequestSeqNum) {
  constexpr size_t kTxWindow = 1;
  uint8_t outbound_req_seq = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      outbound_req_seq =
          pdu->To<SimpleInformationFrameHeader>().receive_seq_num();
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kDefaultMaxTransmissions,
                     kTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();

  tx_engine.UpdateReqSeq(5);
  tx_engine.UpdateAckSeq(1, /*is_poll_response=*/true);  // Peer acks first PDU.
  RunUntilIdle();

  // The second PDU should have been transmitted with ReqSeq = 5.
  EXPECT_EQ(5u, outbound_req_seq);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       RetransmissionOfPduIncludesCurrentSeqNum) {
  constexpr size_t kMaxTransmissions = 2;
  uint8_t outbound_req_seq = 0;
  size_t n_info_frames = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
      outbound_req_seq =
          pdu->To<SimpleInformationFrameHeader>().receive_seq_num();
    }
  });
  TxEngine tx_engine(kTestChannelId,
                     kDefaultMTU,
                     kMaxTransmissions,
                     kDefaultTxWindow,
                     channel(),
                     NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  RunUntilIdle();
  EXPECT_EQ(0u, outbound_req_seq);

  // The receive engine reports that it has received new data from our peer.
  tx_engine.UpdateReqSeq(10);

  // Let receiver_ready_poll_task_ fire. This triggers us to query if our peer
  // has received our data.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Our peer indicates that it has not received our data.
  tx_engine.UpdateAckSeq(0, /*is_poll_response=*/true);
  RunUntilIdle();

  // Our retransmission should include our current request sequence number.
  EXPECT_EQ(2u, n_info_frames);
  EXPECT_EQ(10u, outbound_req_seq);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       PollTaskLoopsEvenWhenRemoteIsBusy) {
  size_t n_pdus = 0;
  channel().HandleSendFrame([&](auto) { ++n_pdus; });
  bool failure = false;
  auto connection_failure_callback = [&] { failure = true; };
  TxEngine tx_engine(
      /*channel_id=*/kTestChannelId,
      /*max_tx_sdu_size=*/kDefaultMTU,
      /*max_transmissions=*/2,
      /*n_frames_in_tx_window=*/kDefaultTxWindow,
      /*channel=*/channel(),
      /*connection_failure_callback=*/connection_failure_callback,
      dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(1u, n_pdus);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_EQ(2u, n_pdus);

  // Receive a "set RemoteBusy" in the poll response (RNR with F=1).
  n_pdus = 0;
  tx_engine.SetRemoteBusy();
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/true);
  ASSERT_EQ(0u, n_pdus);

  // No polls should have been sent out and the monitor task should not have
  // fired.
  RunFor(std::chrono::seconds(12));
  EXPECT_EQ(0u, n_pdus);
  EXPECT_FALSE(failure);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       SetRangeRetransmitCausesUpdateAckSeqToRetransmit) {
  size_t n_info_frames = 0;
  std::optional<uint8_t> tx_seq;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
      tx_seq = pdu->To<SimpleInformationFrameHeader>().tx_seq();
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(3u, n_info_frames);

  // Request a retransmission of unacked data starting with TxSeq=1 (so the
  // second and third frames).
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetRangeRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  EXPECT_EQ(2u, n_info_frames);
  EXPECT_EQ(2u, tx_seq.value_or(0));

  // Subsequent UpdateAckSeqs do not perform retransmission.
  n_info_frames = 0;
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  EXPECT_EQ(0u, n_info_frames);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       SetRangeRetransmitWithPollRequestTriggersPollResponseOnFirstIFrame) {
  size_t n_info_frames = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      // We send two I-Frames to start, then retransmit both but the third frame
      // is in response to a poll request so its and only its poll response bit
      // should be set.
      SCOPED_TRACE(n_info_frames);
      EXPECT_EQ(n_info_frames == 2,
                pdu->To<EnhancedControlField>().is_poll_response());
      ++n_info_frames;
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/3,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(2u, n_info_frames);

  // Request a retransmission of unacked data starting with TxSeq=0 (all
  // frames).
  tx_engine.SetRangeRetransmit(/*is_poll_request=*/true);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  EXPECT_EQ(4u, n_info_frames);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    SetRangeRetransmitAfterPollTaskSuppressesSubsequentRetransmitByPollResponse) {
  size_t n_info_frames = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(1u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Request a retransmission of unacked data starting with TxSeq=0.
  tx_engine.SetRangeRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  ASSERT_EQ(2u, n_info_frames);

  // Don't acknowledge data at TxSeq=0 again in the poll response, which _would_
  // retransmit them if they hadn't already been retransmitted.
  n_info_frames = 0;
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/true);
  EXPECT_EQ(0u, n_info_frames);

  // And further non-acknowledgments that don't call for retransmission
  // shouldn't cause retransmission.
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  EXPECT_EQ(0u, n_info_frames);

  // Or further acknowledgments, for that matter.
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  EXPECT_EQ(0u, n_info_frames);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    SetRangeRetransmitWithPollResponseSetDoesNotSuppressSubsequentRetransmissions) {
  size_t n_info_frames = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(1u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Request a retransmission of unacked data starting with TxSeq=0 that also
  // acknowledges the poll.
  tx_engine.SetRangeRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/true);
  ASSERT_EQ(2u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Don't acknowledge data at TxSeq=0 again in the next poll response, which
  // causes another retransmission.
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/true);
  EXPECT_EQ(3u, n_info_frames);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    SetRangeRetransmitSuppressesRetransmissionsByRetransmitRangeWithPollResponse) {
  size_t n_info_frames = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(1u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  // Request a retransmission of unacked data starting with TxSeq=0.
  tx_engine.SetRangeRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  ASSERT_EQ(2u, n_info_frames);

  // Request retransmission _and_ don't acknowledge data at TxSeq=0, which
  // _would_ retransmit them if they hadn't already been retransmitted.
  n_info_frames = 0;
  tx_engine.SetRangeRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/true);
  EXPECT_EQ(0u, n_info_frames);

  // And further non-acknowledgments that don't call for retransmission
  // shouldn't cause retransmission.
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  EXPECT_EQ(0u, n_info_frames);

  // Or further acknowledgments, for that matter.
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  EXPECT_EQ(0u, n_info_frames);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       SetSingleRetransmitCausesUpdateAckSeqToRetransmit) {
  size_t n_info_frames = 0;
  std::optional<uint8_t> tx_seq;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
      tx_seq = pdu->To<SimpleInformationFrameHeader>().tx_seq();
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(3u, n_info_frames);

  // Request a retransmission of unacked data with TxSeq=1 (so only the second
  // frame).
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  EXPECT_EQ(1u, n_info_frames);
  EXPECT_EQ(1u, tx_seq.value_or(0));

  // Subsequent UpdateAckSeqs do not perform retransmission.
  n_info_frames = 0;
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  EXPECT_EQ(0u, n_info_frames);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       SetSingleRetransmitWithPollRequestTriggersPollResponseOnIFrame) {
  size_t n_info_frames = 0;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      // We send two I-Frames to start, then retransmit both but the third frame
      // is in response to a poll request so its and only its poll response bit
      // should be set.
      SCOPED_TRACE(n_info_frames);
      EXPECT_EQ(n_info_frames == 2,
                pdu->To<EnhancedControlField>().is_poll_response());
      ++n_info_frames;
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/3,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(2u, n_info_frames);

  // Request a retransmission of unacked data with TxSeq=0 (first frame).
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/true);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  EXPECT_EQ(3u, n_info_frames);

  // Request a retransmission of unacked data with TxSeq=1 (second frame).
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/false);
  EXPECT_EQ(4u, n_info_frames);
}

TEST_F(EnhancedRetransmissionModeTxEngineTest,
       SetSingleRetransmitOnlyAcksIFramesIfPollRequestIsSet) {
  size_t n_info_frames = 0;
  std::optional<uint8_t> tx_seq;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField) &&
        pdu->To<EnhancedControlField>().designates_information_frame() &&
        pdu->size() >= sizeof(SimpleInformationFrameHeader)) {
      ++n_info_frames;
      tx_seq = pdu->To<SimpleInformationFrameHeader>().tx_seq();
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/2,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  // TxWindow prevents third I-Frame from going out.
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(2u, n_info_frames);

  // Request a retransmission of unacked data with TxSeq=1, not a poll request.
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);

  // Only the second I-Frame is retransmitted.
  EXPECT_EQ(1u, n_info_frames);
  EXPECT_EQ(1u, tx_seq.value_or(0));

  // Request a retransmission of unacked data with TxSeq=1, this time a poll
  // request.
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/true);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);

  // The first frame is acked, the second I-Frame is retransmitted, and the
  // third frame (which was pending because of TxWindow) is transmitted for the
  // first time.
  EXPECT_EQ(2u, n_info_frames);
  EXPECT_EQ(2u, tx_seq.value_or(0));
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    SetSingleRetransmitDuringPollRequestThenAgainWithPollResponseAndDifferentAckSeqBothCauseRetransmit) {
  size_t n_info_frames = 0;
  size_t n_supervisory_frames = 0;
  std::optional<uint8_t> tx_seq;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField)) {
      if (pdu->To<EnhancedControlField>().designates_information_frame()) {
        ++n_info_frames;
        tx_seq = pdu->To<SimpleInformationFrameHeader>().tx_seq();
      } else if (pdu->To<EnhancedControlField>()
                     .designates_supervisory_frame()) {
        ++n_supervisory_frames;
      }
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(2u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_EQ(1U, n_supervisory_frames);

  // Request a retransmission of unacked data with TxSeq=1, not a poll response.
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);

  // The second I-Frame is retransmitted.
  EXPECT_EQ(1u, n_info_frames);
  EXPECT_EQ(1u, tx_seq.value_or(0));

  // Request a retransmission of unacked data with TxSeq=0, this time a poll
  // response.
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(0u, /*is_poll_response=*/true);

  // The first I-Frame is retransmitted.
  EXPECT_EQ(1u, n_info_frames);
  EXPECT_EQ(0u, tx_seq.value_or(1));

  // Test that the second poll request stopped the MonitorTimer by checking that
  // the ReceiverReady poll was sent.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  EXPECT_EQ(2U, n_supervisory_frames);
}

TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    SetSingleRetransmitDuringPollRequestSuppressesSingleRetransmitPollResponseWithSameAckSeq) {
  size_t n_info_frames = 0;
  size_t n_supervisory_frames = 0;
  std::optional<uint8_t> tx_seq;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField)) {
      if (pdu->To<EnhancedControlField>().designates_information_frame()) {
        ++n_info_frames;
        tx_seq = pdu->To<SimpleInformationFrameHeader>().tx_seq();
      } else if (pdu->To<EnhancedControlField>()
                     .designates_supervisory_frame()) {
        ++n_supervisory_frames;
      }
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(2u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_EQ(1U, n_supervisory_frames);

  // Request a retransmission of unacked data with TxSeq=1, not a poll response.
  n_info_frames = 0;
  tx_seq.reset();
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);

  // The second I-Frame is retransmitted.
  EXPECT_EQ(1u, n_info_frames);
  EXPECT_EQ(1u, tx_seq.value_or(0));

  // Request a retransmission of unacked data with TxSeq=1, this time a poll
  // response.
  n_info_frames = 0;
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/true);
  EXPECT_EQ(0u, n_info_frames);  // No duplicate retransmission occurs.

  // Test that the second poll request stopped the MonitorTimer by checking that
  // the ReceiverReady poll was sent.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  EXPECT_EQ(2U, n_supervisory_frames);
}

// This is a possible bug introduced by implementing SrejActioned to the letter
// per Core Spec v5.{0–2} Vol 3, Part A, Sec 8.6.5.9–11.
TEST_F(
    EnhancedRetransmissionModeTxEngineTest,
    SetSingleRetransmitDuringPollRequestSuppressesRetransmissionErroneously) {
  size_t n_info_frames = 0;
  size_t n_supervisory_frames = 0;
  std::optional<uint8_t> tx_seq;
  channel().HandleSendFrame([&](ByteBufferPtr pdu) {
    if (pdu && pdu->size() >= sizeof(EnhancedControlField)) {
      if (pdu->To<EnhancedControlField>().designates_information_frame()) {
        ++n_info_frames;
        tx_seq = pdu->To<SimpleInformationFrameHeader>().tx_seq();
      } else if (pdu->To<EnhancedControlField>()
                     .designates_supervisory_frame()) {
        ++n_supervisory_frames;
      }
    }
  });
  TxEngine tx_engine(/*channel_id=*/kTestChannelId,
                     /*max_tx_sdu_size=*/kDefaultMTU,
                     /*max_transmissions=*/4,
                     /*n_frames_in_tx_window=*/kDefaultTxWindow,
                     /*channel=*/channel(),
                     /*connection_failure_callback=*/NoOpFailureCallback,
                     dispatcher());

  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  ASSERT_EQ(2u, n_info_frames);

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));
  ASSERT_EQ(1U, n_supervisory_frames);

  // Request a retransmission of unacked data with TxSeq=1, not a poll response.
  n_info_frames = 0;
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/false);
  ASSERT_EQ(1u, n_info_frames);

  // Ack the first frame and complete the poll.
  n_info_frames = 0;
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/true);
  ASSERT_EQ(1u, n_info_frames);  // Retransmission is not suppressed.

  // Send and acknowledge enough frames to roll over AckSeq.
  n_info_frames = 0;
  for (uint8_t i = 0; i < EnhancedControlField::kMaxSeqNum + 1; i++) {
    const uint8_t ack_seq =
        (tx_seq.value() + 1) %
        (static_cast<uint8_t>(EnhancedControlField::kMaxSeqNum) + 1);
    tx_engine.UpdateAckSeq(ack_seq, /*is_poll_response=*/false);
    QueueSdu(tx_engine, std::make_unique<DynamicByteBuffer>(kDefaultPayload));
  }

  // Now there's a new frame with TxSeq=1.
  ASSERT_EQ(uintmax_t{EnhancedControlField::kMaxSeqNum} + 1, n_info_frames);
  ASSERT_EQ(1u, tx_seq.value());

  // Let receiver_ready_poll_task_ fire, to transmit the poll.
  ASSERT_TRUE(RunFor(std::chrono::seconds(2)));

  n_info_frames = 0;
  tx_engine.SetSingleRetransmit(/*is_poll_request=*/false);
  tx_engine.UpdateAckSeq(1u, /*is_poll_response=*/true);

  // Even though this is a different frame than the first retransmission in this
  // test, the latter's SrejActioned state suppresses this retransmission.
  EXPECT_EQ(0u, n_info_frames);
}
// Verify that the queue does not grow unbounded when the peer signals it is
// busy (RNR). SDUs should be queued up to the cap and then rejected/dropped.
TEST_F(EnhancedRetransmissionModeTxEngineTest, RnrStallBoundsPendingPdus) {
  size_t pdus_sent = 0;
  channel().HandleSendFrame([&](ByteBufferPtr) { ++pdus_sent; });

  bool failure_called = false;
  TxEngine engine(
      kTestChannelId,
      kDefaultMTU,
      /*max_transmissions=*/1,
      kDefaultTxWindow,
      channel(),
      /*connection_failure_callback=*/
      [&] { failure_called = true; },
      dispatcher());

  engine.SetRemoteBusy();
  engine.UpdateAckSeq(/*new_seq=*/0, /*is_poll_response=*/false);

  constexpr size_t kMaxPendingPdus = 2 * kDefaultTxWindow;
  constexpr size_t kCount = static_cast<size_t>(kDefaultTxMaxQueuedCount) * 10;
  for (size_t i = 0; i < kCount; ++i) {
    channel().QueueSdu(
        std::make_unique<DynamicByteBuffer>(StaticByteBuffer(0x42)));
    engine.NotifySduQueued();
    size_t expected_queue_size =
        (i < kMaxPendingPdus) ? 0 : (i - kMaxPendingPdus + 1);
    ASSERT_EQ(expected_queue_size, channel().queue_size())
        << "Unexpected queue size at SDU #" << i;
  }

  RunFor(std::chrono::hours(24));

  EXPECT_EQ(0u, pdus_sent);
  EXPECT_FALSE(engine.IsQueueEmpty());
  EXPECT_FALSE(failure_called);
  EXPECT_EQ(kCount - kMaxPendingPdus, channel().queue_size());
}

// Verify that if max transmissions is set to 0, the monitor task does not
// loop forever and cause unbounded queue growth.
TEST_F(EnhancedRetransmissionModeTxEngineTest,
       MaxTransmitZeroBoundsPendingPdus) {
  size_t pdus_sent = 0;
  channel().HandleSendFrame([&](ByteBufferPtr) { ++pdus_sent; });

  bool failure_called = false;
  TxEngine engine(
      kTestChannelId,
      kDefaultMTU,
      /*max_transmissions=*/0,
      /*n_frames_in_tx_window=*/1,
      channel(),
      [&] { failure_called = true; },
      dispatcher());

  channel().QueueSdu(
      std::make_unique<DynamicByteBuffer>(StaticByteBuffer(0x01)));
  engine.NotifySduQueued();
  ASSERT_EQ(1u, pdus_sent);
  pdus_sent = 0;

  RunFor(std::chrono::seconds(3));
  ASSERT_GE(pdus_sent, 1u);

  constexpr size_t kCount = 2000;
  for (size_t i = 0; i < kCount; ++i) {
    channel().QueueSdu(
        std::make_unique<DynamicByteBuffer>(StaticByteBuffer(0x02)));
    engine.NotifySduQueued();
    size_t expected_queue_size = (i == 0) ? 0 : i;
    ASSERT_EQ(expected_queue_size, channel().queue_size())
        << "Unexpected queue size at SDU #" << i;
  }

  RunFor(std::chrono::hours(24));

  EXPECT_FALSE(engine.IsQueueEmpty());
  EXPECT_FALSE(failure_called);
  EXPECT_EQ(kCount - 1, channel().queue_size());
}

// Verify that when SDUs are buffered in ChannelImpl due to pending_pdus_ being
// at capacity (such as during RNR stall), clearing RNR or receiving ACKs
// automatically pulls and transmits waiting SDUs from the channel queue.
TEST_F(EnhancedRetransmissionModeTxEngineTest,
       ClearRemoteBusyAndAckSeqDequeueWaitingSdus) {
  size_t pdus_sent = 0;
  channel().HandleSendFrame([&](ByteBufferPtr) { ++pdus_sent; });

  bool failure_called = false;
  TxEngine engine(
      kTestChannelId,
      kDefaultMTU,
      /*max_transmissions=*/1,
      /*n_frames_in_tx_window=*/2,
      channel(),
      /*connection_failure_callback=*/
      [&] { failure_called = true; },
      dispatcher());

  engine.SetRemoteBusy();
  engine.UpdateAckSeq(/*new_seq=*/0, /*is_poll_response=*/false);

  // Queue SDUs beyond kMaxPendingPdus (2 * 2 = 4).
  for (size_t i = 0; i < 6; ++i) {
    channel().QueueSdu(
        std::make_unique<DynamicByteBuffer>(StaticByteBuffer(0x42)));
    engine.NotifySduQueued();
  }
  // Because remote is busy, no PDUs sent, but 4 are in pending_pdus_ and 2 left
  // waiting in channel queue.
  EXPECT_EQ(0u, pdus_sent);
  EXPECT_EQ(2u, channel().queue_size());

  // When remote busy is cleared and UpdateAckSeq is processed (as RxEngine does
  // upon receiving supervisory frames), pending_pdus_ should be sent up to tx
  // window (2).
  engine.ClearRemoteBusy();
  engine.UpdateAckSeq(/*new_seq=*/0, /*is_poll_response=*/false);
  EXPECT_EQ(2u, pdus_sent);
  // Still 2 SDUs waiting in channel().queue_size() because pending_pdus_ is at
  // max capacity (4).
  EXPECT_EQ(2u, channel().queue_size());

  // Acknowledging the 2 sent frames frees space in pending_pdus_, which should
  // automatically pull and transmit the 2 waiting SDUs from the channel queue.
  engine.UpdateAckSeq(/*new_seq=*/2, /*is_poll_response=*/false);
  EXPECT_EQ(4u, pdus_sent);
  EXPECT_EQ(0u, channel().queue_size());
  EXPECT_FALSE(failure_called);
}

}  // namespace
}  // namespace bt::l2cap::internal
