
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

#include "pw_allocator/testing.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_span/cast.h"
#include "pw_sync/mutex.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy::internal {

namespace {

constexpr uint16_t kLocalCid = 0x60;
constexpr uint16_t kMtu = 7;
constexpr uint16_t kMps = 5;
constexpr uint16_t kCredits = 4;
constexpr size_t kAllocatorSize = 500;

class CreditBasedFlowControlRxEngineTest : public ::testing::Test {
 public:
  void SetUp() override {
    ConnectionOrientedChannelConfig config{
        .cid = kLocalCid, .mtu = kMtu, .mps = kMps, .credits = kCredits};
    Function<Status(uint16_t)> replenish_rx_credits_fn =
        [this](uint16_t amount) -> Status {
      replenished_credits_ += amount;
      return OkStatus();
    };

    engine_.emplace(
        config, multibuf_allocator_, std::move(replenish_rx_credits_fn));
  }

  void TearDown() override { engine_.reset(); }

  CreditBasedFlowControlRxEngine& engine() { return engine_.value(); }

  uint16_t replenished_credits() const { return replenished_credits_; }

  multibuf::MultiBufAllocator& multibuf_allocator() {
    return multibuf_allocator_;
  }

 private:
  uint16_t replenished_credits_ = 0;

  // Use libc allocators so msan can detect use after frees.
  std::array<std::byte, kAllocatorSize> packet_buffer_{};
  pw::multibuf::SimpleAllocator multibuf_allocator_{
      /*data_area=*/packet_buffer_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

  std::optional<CreditBasedFlowControlRxEngine> engine_;
};

TEST_F(CreditBasedFlowControlRxEngineTest,
       ReceiveSegmentedSduAndThenCompleteSdu) {
  const std::array<uint8_t, 7> kExpectedSdu0 = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  std::array<uint8_t, 9> pdu_0 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x07,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x00,
                                  0x01,
                                  0x02};
  std::array<uint8_t, 8> pdu_1 = {// L2cap B-Frame:
                                  0x04,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                         // Payload:
                                  0x03,
                                  0x04,
                                  0x05,
                                  0x06};
  RxEngine::HandlePduFromControllerReturnValue result_0 =
      engine().HandlePduFromController(pdu_0);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(result_0));
  EXPECT_EQ(replenished_credits(), 0u);

  RxEngine::HandlePduFromControllerReturnValue result_1 =
      engine().HandlePduFromController(pdu_1);
  ASSERT_TRUE(std::holds_alternative<multibuf::MultiBuf>(result_1));
  EXPECT_EQ(replenished_credits(), 2u);

  auto contiguous = std::get<multibuf::MultiBuf>(result_1).ContiguousSpan();
  pw::span<uint8_t> sdu_0 = span_cast<uint8_t>(*contiguous);
  EXPECT_TRUE(std::equal(
      sdu_0.begin(), sdu_0.end(), kExpectedSdu0.begin(), kExpectedSdu0.end()));

  const std::array<uint8_t, 3> kExpectedSdu1 = {0x07, 0x08, 0x09};
  std::array<uint8_t, 9> pdu_2 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x03,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x07,
                                  0x08,
                                  0x09};

  RxEngine::HandlePduFromControllerReturnValue result_2 =
      engine().HandlePduFromController(pdu_2);
  ASSERT_TRUE(std::holds_alternative<multibuf::MultiBuf>(result_2));
  contiguous = std::get<multibuf::MultiBuf>(result_2).ContiguousSpan();
  pw::span<uint8_t> sdu_1 = span_cast<uint8_t>(*contiguous);
  EXPECT_TRUE(std::equal(
      sdu_1.begin(), sdu_1.end(), kExpectedSdu1.begin(), kExpectedSdu1.end()));

  EXPECT_EQ(replenished_credits(), 2u);
}

TEST_F(CreditBasedFlowControlRxEngineTest, SumOfPayloadsExceedsSduLength) {
  std::array<uint8_t, 9> pdu_0 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x04,
                                  0x00,  // SDU length (too small!)
                                         // Payload:
                                  0x00,
                                  0x01,
                                  0x02};
  std::array<uint8_t, 8> pdu_1 = {// L2cap B-Frame:
                                  0x04,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                         // Payload:
                                  0x03,
                                  0x04,
                                  0x05,
                                  0x06};
  RxEngine::HandlePduFromControllerReturnValue result_0 =
      engine().HandlePduFromController(pdu_0);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(result_0));

  RxEngine::HandlePduFromControllerReturnValue result_1 =
      engine().HandlePduFromController(pdu_1);
  ASSERT_TRUE(std::holds_alternative<L2capChannelEvent>(result_1));
  EXPECT_EQ(std::get<L2capChannelEvent>(result_1),
            L2capChannelEvent::kRxInvalid);
}

TEST_F(CreditBasedFlowControlRxEngineTest, PayloadExceedsMps) {
  std::array<uint8_t, 12> pdu = {// L2cap K-Frame:
                                 0x08,
                                 0x00,  // PDU length
                                 0x60,
                                 0x00,  // Local Channel ID
                                 0x06,
                                 0x00,  // SDU length
                                        // Payload:
                                 0x00,
                                 0x01,
                                 0x02,
                                 0x03,
                                 0x04,
                                 0x05};
  RxEngine::HandlePduFromControllerReturnValue result =
      engine().HandlePduFromController(pdu);
  ASSERT_TRUE(std::holds_alternative<L2capChannelEvent>(result));
  EXPECT_EQ(std::get<L2capChannelEvent>(result), L2capChannelEvent::kRxInvalid);
}

TEST_F(CreditBasedFlowControlRxEngineTest, SduLengthExceedsMtu) {
  std::array<uint8_t, 8> pdu = {// L2cap K-Frame:
                                0x04,
                                0x00,  // PDU length
                                0x60,
                                0x00,  // Local Channel ID
                                0x08,
                                0x00,  // SDU length
                                       // Payload:
                                0x00,
                                0x01};
  RxEngine::HandlePduFromControllerReturnValue result =
      engine().HandlePduFromController(pdu);
  ASSERT_TRUE(std::holds_alternative<L2capChannelEvent>(result));
  EXPECT_EQ(std::get<L2capChannelEvent>(result), L2capChannelEvent::kRxInvalid);
}

TEST_F(CreditBasedFlowControlRxEngineTest, BufferTooSmallForKFrame) {
  std::array<uint8_t, 8> pdu = {
      // L2cap K-Frame:
      0x04,
      0x00  // PDU length
  };
  RxEngine::HandlePduFromControllerReturnValue result =
      engine().HandlePduFromController(pdu);
  ASSERT_TRUE(std::holds_alternative<L2capChannelEvent>(result));
  EXPECT_EQ(std::get<L2capChannelEvent>(result), L2capChannelEvent::kRxInvalid);
}

TEST_F(CreditBasedFlowControlRxEngineTest, OutOfMemory) {
  std::optional<multibuf::MultiBuf> large_buffer =
      multibuf_allocator().AllocateContiguous(kAllocatorSize);
  ASSERT_TRUE(large_buffer);
  std::array<uint8_t, 8> pdu = {// L2cap K-Frame:
                                0x04,
                                0x00,  // PDU length
                                0x60,
                                0x00,  // Local Channel ID
                                0x02,
                                0x00,  // SDU length
                                       // Payload:
                                0x00,
                                0x01};
  RxEngine::HandlePduFromControllerReturnValue result =
      engine().HandlePduFromController(pdu);
  ASSERT_TRUE(std::holds_alternative<L2capChannelEvent>(result));
  EXPECT_EQ(std::get<L2capChannelEvent>(result),
            L2capChannelEvent::kRxOutOfMemory);
}

TEST_F(CreditBasedFlowControlRxEngineTest, AddCredits) {
  PW_TEST_ASSERT_OK(engine().AddRxCredits(10 - kCredits));

  std::array<uint8_t, 9> pdu_0 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x03,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x07,
                                  0x08,
                                  0x09};

  RxEngine::HandlePduFromControllerReturnValue result_0 =
      engine().HandlePduFromController(pdu_0);
  ASSERT_TRUE(std::holds_alternative<multibuf::MultiBuf>(result_0));
  EXPECT_EQ(replenished_credits(), 0u);

  std::array<uint8_t, 9> pdu_1 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x03,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x09,
                                  0x09,
                                  0x09};
  RxEngine::HandlePduFromControllerReturnValue result_1 =
      engine().HandlePduFromController(pdu_1);
  ASSERT_TRUE(std::holds_alternative<multibuf::MultiBuf>(result_1));
  // The threshold should not be exceeded
  EXPECT_EQ(replenished_credits(), 0u);

  std::array<uint8_t, 9> pdu_2 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x03,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x08,
                                  0x09,
                                  0x08};
  RxEngine::HandlePduFromControllerReturnValue result_2 =
      engine().HandlePduFromController(pdu_2);
  ASSERT_TRUE(std::holds_alternative<multibuf::MultiBuf>(result_2));
  // The threshold should be exceeded
  EXPECT_EQ(replenished_credits(), 3u);
}

TEST_F(CreditBasedFlowControlRxEngineTest, Reset) {
  // Send first PDU of a segmented SDU.
  std::array<uint8_t, 9> pdu_0 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x07,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x00,
                                  0x01,
                                  0x02};
  RxEngine::HandlePduFromControllerReturnValue result_0 =
      engine().HandlePduFromController(pdu_0);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(result_0));

  // Reset the engine.
  engine().Reset();

  // Send a new first PDU of a different SDU.
  const std::array<uint8_t, 3> kExpectedSdu = {0x0a, 0x0b, 0x0c};
  std::array<uint8_t, 9> pdu_1 = {// L2cap K-Frame:
                                  0x05,
                                  0x00,  // PDU length
                                  0x60,
                                  0x00,  // Local Channel ID
                                  0x03,
                                  0x00,  // SDU length
                                         // Payload:
                                  0x0a,
                                  0x0b,
                                  0x0c};
  RxEngine::HandlePduFromControllerReturnValue result_1 =
      engine().HandlePduFromController(pdu_1);
  // It should be complete because SDU length is 3 and we sent 3 bytes.
  ASSERT_TRUE(std::holds_alternative<multibuf::MultiBuf>(result_1));
  auto contiguous = std::get<multibuf::MultiBuf>(result_1).ContiguousSpan();
  pw::span<uint8_t> sdu = span_cast<uint8_t>(*contiguous);
  EXPECT_TRUE(std::equal(
      sdu.begin(), sdu.end(), kExpectedSdu.begin(), kExpectedSdu.end()));
}

}  // namespace

}  // namespace pw::bluetooth::proxy::internal
