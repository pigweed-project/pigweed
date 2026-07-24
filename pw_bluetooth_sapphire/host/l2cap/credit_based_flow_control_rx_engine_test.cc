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

#include "pw_bluetooth_sapphire/internal/host/l2cap/credit_based_flow_control_rx_engine.h"

#include "pw_bluetooth_sapphire/internal/host/l2cap/fragmenter.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::l2cap::internal {
namespace {

using Engine = CreditBasedFlowControlRxEngine;

class CreditBasedFlowControlRxEngineTest : public ::testing::Test {
 protected:
  static constexpr hci_spec::ConnectionHandle kTestHandle = 0x0001;
  static constexpr ChannelId kTestChannelId = 0x0001;

  Engine& engine() { return *engine_; }
  size_t failure_callback_count() { return failure_callback_count_; }
  PDU ToPdu(const ByteBuffer& buffer) {
    return Fragmenter(kTestHandle)
        .BuildFrame(kTestChannelId, buffer, FrameCheckSequenceOption::kNoFcs);
  }

  ByteBufferPtr ProcessPayload(const ByteBuffer& buffer) {
    return engine().ProcessPdu(ToPdu(buffer));
  }

  uint64_t total_credits_returned() { return total_credits_returned_; }

 private:
  size_t failure_callback_count_ = 0;
  uint64_t total_credits_returned_ = 0;
  std::unique_ptr<Engine> engine_ = std::make_unique<Engine>(
      /*max_mtu=*/1000,
      /*max_mps=*/1000,
      /*initial_credits=*/10,
      [this] { ++failure_callback_count_; },
      [this](uint16_t credits) { total_credits_returned_ += credits; });
};

TEST_F(CreditBasedFlowControlRxEngineTest, SmallUnsegmentedSdu) {
  // clang-format off
  const StaticByteBuffer payload(
      // SDU size field (LE u16)
      4, 0,
      // Payload
      't', 'e', 's', 't'
  );
  // clang-format on

  const ByteBufferPtr sdu = ProcessPayload(payload);

  ASSERT_TRUE(sdu);
  EXPECT_TRUE(ContainersEqual(StaticByteBuffer('t', 'e', 's', 't'), *sdu));
  EXPECT_EQ(0u, failure_callback_count());

  EXPECT_EQ(0u, total_credits_returned());
  engine().AcknowledgeRead();
  EXPECT_EQ(1u, total_credits_returned());
}

TEST_F(CreditBasedFlowControlRxEngineTest, LargeUnsegmentedSdu) {
  // clang-format off
  const StaticByteBuffer payload(
      // SDU size field (LE u16)
      58, 0,
      // Payload
      'L', 'o', 'r', 'e', 'm', ' ', 'i', 'p', 's', 'u', 'm', ' ', 'd', 'o', 'l',
      'o', 'r', ' ', 's', 'i', 't', ' ', 'a', 'm', 'e', 't', ',', ' ', 'c', 'o',
      'n', 's', 'e', 'c', 't', 'e', 't', 'u', 'r', ' ', 'a', 'd', 'i', 'p', 'i',
      's', 'c', 'i', 'n', 'g', ' ', 'e', 'l', 'i', 't', '.', ' ', 'S'
  );
  // clang-format on

  const ByteBufferPtr sdu = ProcessPayload(payload);
  ASSERT_TRUE(sdu);
  EXPECT_TRUE(ContainersEqual(payload.view(2), *sdu));
  EXPECT_EQ(0u, failure_callback_count());

  EXPECT_EQ(0u, total_credits_returned());
  engine().AcknowledgeRead();
  EXPECT_EQ(1u, total_credits_returned());
}

TEST_F(CreditBasedFlowControlRxEngineTest, SduSegmentedIntoManySmallPdus) {
  EXPECT_TRUE(engine().IsQueueEmpty());
  // clang-format off
  EXPECT_FALSE(ProcessPayload(StaticByteBuffer(
      // SDU size field (LE u16)
      16, 0,
      // First four bytes of payload
      't', 'e', 's', 't'
  )));
  // clang-format on
  EXPECT_FALSE(engine().IsQueueEmpty());

  EXPECT_FALSE(ProcessPayload(StaticByteBuffer('i', 'n', 'g', ' ')));
  EXPECT_FALSE(ProcessPayload(StaticByteBuffer('f', 'o', 'r', ' ')));
  const ByteBufferPtr sdu =
      ProcessPayload(StaticByteBuffer('b', 'u', 'g', 's'));
  EXPECT_TRUE(engine().IsQueueEmpty());

  // clang-format off
  StaticByteBuffer expected(
      't', 'e', 's', 't', 'i', 'n', 'g', ' ',
      'f', 'o', 'r', ' ', 'b', 'u', 'g', 's'
  );
  // clang-format on

  ASSERT_TRUE(sdu);
  EXPECT_TRUE(ContainersEqual(expected, *sdu));
  EXPECT_EQ(0u, failure_callback_count());

  EXPECT_EQ(0u, total_credits_returned());
  engine().AcknowledgeRead();
  EXPECT_EQ(4u, total_credits_returned());
}

TEST_F(CreditBasedFlowControlRxEngineTest, FailSduSmallerThanPayload) {
  // clang-format off
  const StaticByteBuffer payload(
      // SDU size field (LE u16)
      4, 0,
      // Payload
      'f', 'a', 'i', 'l', 'i', 'u', 'r', 'e'
  );
  // clang-format on

  EXPECT_FALSE(ProcessPayload(payload));
  EXPECT_EQ(1u, failure_callback_count());
}

TEST_F(CreditBasedFlowControlRxEngineTest, FailSduSmallerThanPayloadSegmented) {
  // clang-format off
  const StaticByteBuffer payload(
      // SDU size field (LE u16)
      5, 0,
      // Payload
      'f', 'a', 'i', 'l'
  );
  // clang-format on

  EXPECT_FALSE(ProcessPayload(payload));
  EXPECT_EQ(0u, failure_callback_count());
  EXPECT_FALSE(ProcessPayload(StaticByteBuffer('i', 'u', 'r', 'e')));
  EXPECT_EQ(1u, failure_callback_count());
}

TEST_F(CreditBasedFlowControlRxEngineTest, InitialFrameMissingSduSize) {
  const ByteBufferPtr sdu = ProcessPayload(StaticByteBuffer(0));
  EXPECT_FALSE(sdu);
  EXPECT_EQ(1u, failure_callback_count());
}

// FOOOOOOOOOOOOOOOOOOOOOO

TEST(CreditBasedFlowControlRxEngineStandaloneTest,
     FailureCallbackSynchronouslyDestroysEngine) {
  std::unique_ptr<CreditBasedFlowControlRxEngine> rx_engine;

  auto failure_cb = [&] { rx_engine.reset(); };

  auto return_credits_cb = [](uint16_t) {};

  rx_engine = std::make_unique<CreditBasedFlowControlRxEngine>(
      /*max_mtu=*/1000,
      /*max_mps=*/1000,
      /*initial_credits=*/10,
      std::move(failure_cb),
      std::move(return_credits_cb));

  // Deliver a PDU that triggers FailureCallback (e.g. length < 2)
  StaticByteBuffer pdu_bytes(0);
  PDU pdu = Fragmenter(0x0001).BuildFrame(
      0x0001, pdu_bytes, FrameCheckSequenceOption::kNoFcs);

  rx_engine->ProcessPdu(std::move(pdu));
}

TEST(CreditBasedFlowControlRxEngineStandaloneTest,
     AcknowledgeReadSynchronouslyDestroysEngine) {
  std::unique_ptr<CreditBasedFlowControlRxEngine> rx_engine;

  auto failure_cb = [] {};

  auto return_credits_cb = [&](uint16_t) { rx_engine.reset(); };

  rx_engine = std::make_unique<CreditBasedFlowControlRxEngine>(
      /*max_mtu=*/1000,
      /*max_mps=*/1000,
      /*initial_credits=*/10,
      std::move(failure_cb),
      std::move(return_credits_cb));

  // Process a valid complete SDU to queue up unacked read credits.
  // Small unsegmented SDU: SDU size field = 4, Payload = "test"
  StaticByteBuffer payload(4, 0, 't', 'e', 's', 't');
  PDU pdu = Fragmenter(0x0001).BuildFrame(
      0x0001, payload, FrameCheckSequenceOption::kNoFcs);

  ByteBufferPtr sdu = rx_engine->ProcessPdu(std::move(pdu));
  ASSERT_TRUE(sdu);

  // Calling AcknowledgeRead invokes return_credits_cb, which synchronously
  // resets rx_engine.
  rx_engine->AcknowledgeRead();
}

TEST_F(CreditBasedFlowControlRxEngineTest, SduExceedingMtuFails) {
  // Send a PDU indicating an SDU size of 2000 bytes, which exceeds the
  // negotiated MTU (1000). SDU size field: 2000 (0x07D0) -> 0xD0, 0x07 in
  // little endian.
  StaticByteBuffer payload(0xD0,
                           0x07,  // SDU size field (2000)
                           't',
                           'e',
                           's',
                           't');

  ProcessPayload(payload);

  // The PDU must be rejected and trigger the failure callback because it
  // exceeds the MTU.
  EXPECT_EQ(1u, failure_callback_count());
}

TEST_F(CreditBasedFlowControlRxEngineTest, PduExceedingMpsFails) {
  // Negotiated MPS is 1000.
  // Send a PDU with length 1005 bytes (exceeding MPS).
  DynamicByteBuffer payload(1005);
  payload[0] = 0xE8;
  payload[1] = 0x03;  // SDU size = 1000

  EXPECT_FALSE(ProcessPayload(payload));
  EXPECT_EQ(1u, failure_callback_count());
}

TEST_F(CreditBasedFlowControlRxEngineTest, OutOfCreditsFails) {
  // Initial credits is 10.
  // Send 10 valid PDUs. They should succeed.
  const StaticByteBuffer payload(4, 0, 't', 'e', 's', 't');

  for (size_t i = 0; i < 10; ++i) {
    auto sdu = ProcessPayload(payload);
    ASSERT_TRUE(sdu);
    EXPECT_EQ(0u, failure_callback_count());
  }

  // Send the 11th PDU. This should fail because we have 0 credits remaining.
  auto sdu = ProcessPayload(payload);
  EXPECT_FALSE(sdu);
  EXPECT_EQ(1u, failure_callback_count());

  // Acknowledge the first read to return credits.
  engine().AcknowledgeRead();
  EXPECT_EQ(1u, total_credits_returned());

  // The peer should now be able to send packets again.
  auto sdu2 = ProcessPayload(payload);
  ASSERT_TRUE(sdu2);
  EXPECT_EQ(1u, failure_callback_count());
}

}  // namespace
}  // namespace bt::l2cap::internal
