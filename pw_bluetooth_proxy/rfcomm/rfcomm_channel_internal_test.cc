// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_proxy/rfcomm/internal/rfcomm_channel_internal.h"

#include <optional>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_manager.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy::rfcomm::internal {
namespace testing {

constexpr size_t kMaxTestPacketSize = 256;
constexpr size_t kMaxTestPacketCount = 30;

class MockChannelProxy : public ChannelProxy {
 public:
  const pw::Vector<pw::Vector<uint8_t, kMaxTestPacketSize>,
                   kMaxTestPacketCount>&
  written_payloads() const {
    return written_payloads_;
  }

  void set_next_write_status(Status status, bool return_buffer = true) {
    next_write_status_ = status;
    return_buffer_on_failure_ = return_buffer;
  }

 private:
  StatusWithMultiBuf DoWrite(multibuf::MultiBuf&& payload) override {
    if (!next_write_status_.ok()) {
      Status status_to_return = next_write_status_;
      next_write_status_ = OkStatus();  // Reset for next call
      if (return_buffer_on_failure_) {
        return {status_to_return, std::move(payload)};
      } else {
        return {status_to_return, std::nullopt};
      }
    }
    pw::Vector<uint8_t, kMaxTestPacketSize> data;
    data.resize(payload.size());
    auto bytes_copied = payload.CopyTo(as_writable_bytes(span(data)));
    if (!bytes_copied.ok()) {
      return {bytes_copied.status()};
    }
    written_payloads_.push_back(std::move(data));
    return {OkStatus()};
  }

  Status DoIsWriteAvailable() override { return OkStatus(); }

  Status DoSendAdditionalRxCredits(
      uint16_t /*additional_rx_credits*/) override {
    return OkStatus();
  }
  Status next_write_status_ = OkStatus();
  bool return_buffer_on_failure_ = true;
  pw::Vector<pw::Vector<uint8_t, kMaxTestPacketSize>, kMaxTestPacketCount>
      written_payloads_;
};

class MockL2capChannelManager final : public L2capChannelManagerInterface {
 public:
  MockL2capChannelManager() = default;

  // Triggers the event callback to simulate an L2CAP channel event.
  void TriggerL2capEvent(L2capChannelEvent event) {
    if (event_fn_) {
      event_fn_(event);
    }
  }

  MockChannelProxy* last_channel_proxy() { return last_channel_proxy_; }

 private:
  Result<UniquePtr<ChannelProxy>> DoInterceptCreditBasedFlowControlChannel(
      ConnectionHandle,
      ConnectionOrientedChannelConfig,
      ConnectionOrientedChannelConfig,
      MultiBufReceiveFunction&&,
      ChannelEventCallback&&) override {
    return Status::Unimplemented();
  }

  Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      ConnectionHandle /*connection_handle*/,
      uint16_t /*local_channel_id*/,
      uint16_t /*remote_channel_id*/,
      AclTransportType /*transport*/,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& /*payload_from_host_fn*/,
      ChannelEventCallback&& event_fn) override {
    payload_from_controller_fn_ = std::move(payload_from_controller_fn);
    event_fn_ = std::move(event_fn);
    auto proxy = allocator_.MakeUnique<MockChannelProxy>();
    last_channel_proxy_ = proxy.get();
    return proxy;
  }

  allocator::test::AllocatorForTest<1024> allocator_;
  BufferReceiveFunction payload_from_controller_fn_;
  ChannelEventCallback event_fn_;
  MockChannelProxy* last_channel_proxy_ = nullptr;
};

}  // namespace testing

class RfcommChannelTest : public ::testing::Test {
 protected:
  RfcommChannelTest()
      : l2cap_manager_(),
        rfcomm_manager_(l2cap_manager_, allocator::GetLibCAllocator()),
        channel_(
            multibuf_allocator_,
            l2cap_channel_for_test_,
            kConnectionHandle,
            kChannelNumber,
            RfcommDirection::kInitiator,
            true,
            kDefaultRxConfig,
            kDefaultTxConfig,
            kRfcommCrc,
            [this](multibuf::MultiBuf&& pdu) {
              last_received_pdu_.resize(pdu.size());
              std::ignore =
                  pdu.CopyTo(as_writable_bytes(span(last_received_pdu_)));
            },
            [this](RfcommEvent event) { last_event_ = event; }) {}

  static constexpr ConnectionHandle kConnectionHandle =
      static_cast<ConnectionHandle>(0x01);
  static constexpr uint8_t kChannelNumber = 2;
  static constexpr RfcommChannelConfig kDefaultRxConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 10};
  static constexpr RfcommChannelConfig kDefaultTxConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 10};
  static constexpr pw::checksum::Crc8 kRfcommCrc =
      pw::checksum::Crc8(0x07, 0xFF, true, true, 0xff);

  static constexpr size_t kDataSize = 2048;

  std::array<std::byte, kDataSize> buffer_{};
  multibuf::SimpleAllocator multibuf_allocator_{
      /*data_area=*/buffer_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

  testing::MockL2capChannelManager l2cap_manager_;
  RfcommManager rfcomm_manager_;
  testing::MockChannelProxy l2cap_channel_for_test_;
  RfcommChannelInternal channel_;
  pw::Vector<uint8_t, kDataSize> last_received_pdu_;
  std::optional<RfcommEvent> last_event_;
};

TEST_F(RfcommChannelTest, WriteSinglePacket) {
  const pw::Vector<uint8_t, 3> payload = {0, 0, 0};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf)).status, OkStatus());

  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 1u);
  const span<const uint8_t> written_payload =
      l2cap_channel_for_test_.written_payloads().front();

  ASSERT_FALSE(written_payload.empty());
  EXPECT_EQ(
      written_payload.size(),
      payload.size() + static_cast<size_t>(
                           emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));

  // Verify the header.
  // Address field: channel_number=2, D=1 (initiated by initiator), C/R=1 (from
  // initiator), EA=1
  const uint8_t expected_address =
      (kChannelNumber << 3) | (1 << 2) | (1 << 1) | 1;
  EXPECT_EQ(written_payload[0], expected_address);

  // Control field: UIH.
  EXPECT_EQ(
      written_payload[1],
      static_cast<uint8_t>(
          emboss::RfcommFrameType::UNNUMBERED_INFORMATION_WITH_HEADER_CHECK));

  // Length field: 3 bytes of info.
  const uint8_t expected_length =
      static_cast<uint8_t>((payload.size() << 1) | 1);
  EXPECT_EQ(written_payload[2], expected_length);

  // Verify the payload.
  const span<const uint8_t> written_data = written_payload.subspan(
      static_cast<size_t>(emboss::RfcommHeaderLength::WITH_LENGTH),
      payload.size());
  EXPECT_TRUE(std::equal(written_data.begin(),
                         written_data.end(),
                         payload.begin(),
                         payload.end()));
}

TEST_F(RfcommChannelTest, WriteFragmentedPacket) {
  pw::Vector<uint8_t, kDefaultTxConfig.max_frame_size + 1> payload;
  payload.resize(payload.capacity());
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf)).status, OkStatus());

  // Two packets should be sent.
  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 2u);

  const span<const uint8_t> written_payload =
      l2cap_channel_for_test_.written_payloads().back();

  // The last one should contain the remaining 1
  // byte.
  ASSERT_FALSE(written_payload.empty());
  EXPECT_EQ(written_payload.size(),
            1 + static_cast<size_t>(
                    emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));

  // Verify the header.
  // Address field: channel_number=2, D=1 (initiated by initiator), C/R=1 (from
  // initiator), EA=1
  const uint8_t expected_address =
      (kChannelNumber << 3) | (1 << 2) | (1 << 1) | 1;
  EXPECT_EQ(written_payload[0], expected_address);

  // Control field: UIH.
  EXPECT_EQ(
      written_payload[1],
      static_cast<uint8_t>(
          emboss::RfcommFrameType::UNNUMBERED_INFORMATION_WITH_HEADER_CHECK));

  // Length field: 1 byte of info.
  const uint8_t expected_length = static_cast<uint8_t>((1 << 1) | 1);
  EXPECT_EQ(written_payload[2], expected_length);

  // Verify the payload.
  const span<const uint8_t> written_data = written_payload.subspan(
      static_cast<size_t>(emboss::RfcommHeaderLength::WITH_LENGTH),
      payload.size() - kDefaultTxConfig.max_frame_size);
  EXPECT_TRUE(std::equal(written_data.begin(),
                         written_data.end(),
                         payload.begin() + kDefaultTxConfig.max_frame_size,
                         payload.end()));
}

TEST_F(RfcommChannelTest, WriteLongPacket) {
  constexpr RfcommChannelConfig kTxConfig = {
      .cid = 1, .max_frame_size = 200, .initial_credits = 10};
  RfcommChannelInternal channel(
      multibuf_allocator_,
      l2cap_channel_for_test_,
      kConnectionHandle,
      kChannelNumber,
      RfcommDirection::kInitiator,
      true,
      kDefaultRxConfig,
      kTxConfig,
      kRfcommCrc,
      [this](multibuf::MultiBuf&& pdu) {
        last_received_pdu_.resize(pdu.size());
        std::ignore = pdu.CopyTo(as_writable_bytes(span(last_received_pdu_)));
      },
      [this](RfcommEvent event) { last_event_ = event; });

  pw::Vector<uint8_t,
             static_cast<size_t>(emboss::RfcommMaxInfoSize::ONE_BYTE_LENGTH) +
                 1>
      payload;
  payload.resize(payload.capacity());
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel.Write(std::move(mbuf)).status, OkStatus());
  ASSERT_FALSE(l2cap_channel_for_test_.written_payloads().empty());
  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 1u);
  const span<const uint8_t> written_payload =
      l2cap_channel_for_test_.written_payloads().front();
  EXPECT_EQ(
      written_payload.size(),
      payload.size() + static_cast<size_t>(
                           emboss::RfcommDataFrameOverhead::WITH_LONG_HEADER));

  // Verify the header.
  // Address field: channel_number=2, D=1 (initiated by initiator), C/R=1 (from
  // initiator), EA=1
  const uint8_t expected_address =
      (kChannelNumber << 3) | (1 << 2) | (1 << 1) | 1;
  EXPECT_EQ(written_payload[0], expected_address);

  // Control field: UIH.
  EXPECT_EQ(
      written_payload[1],
      static_cast<uint8_t>(
          emboss::RfcommFrameType::UNNUMBERED_INFORMATION_WITH_HEADER_CHECK));

  // Length field (2 bytes).
  const size_t info_length = payload.size();
  const uint8_t expected_length_byte1 =
      static_cast<uint8_t>((info_length & 0x7F) << 1);
  const uint8_t expected_length_byte2 = static_cast<uint8_t>(info_length >> 7);
  EXPECT_EQ(written_payload[2], expected_length_byte1);
  EXPECT_EQ(written_payload[3], expected_length_byte2);

  // Verify the payload.
  const span<const uint8_t> written_data = written_payload.subspan(
      static_cast<size_t>(emboss::RfcommHeaderLength::WITH_EXTENDED_LENGTH),
      payload.size());
  EXPECT_TRUE(std::equal(written_data.begin(),
                         written_data.end(),
                         payload.begin(),
                         payload.end()));
}

TEST_F(RfcommChannelTest, WriteNoCredits) {
  // Exhaust all credits.
  for (int i = 0;
       i < kDefaultTxConfig.initial_credits + 10 /* kDefaultTxQueueSize */;
       ++i) {
    constexpr uint8_t kPayloadData[] = {0x01};
    const ConstByteSpan payload = as_bytes(span(kPayloadData));
    auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf& mbuf = mbuf_result.value();
    ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
    EXPECT_EQ(channel_.Write(std::move(mbuf)).status, OkStatus());
  }

  // Ensure that we can't send any more packets.
  constexpr uint8_t kPayloadData[] = {0x01};
  const ConstByteSpan payload = as_bytes(span(kPayloadData));
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf)).status, Status::Unavailable());
}

TEST_F(RfcommChannelTest, HandlePduWithCreditsAndVerify) {
  // Exhaust all credits.
  for (int i = 0;
       i < kDefaultTxConfig.initial_credits + 10 /* kDefaultTxQueueSize */;
       ++i) {
    constexpr uint8_t kPayloadData[] = {0x01};
    const ConstByteSpan payload = as_bytes(span(kPayloadData));
    auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf& mbuf = mbuf_result.value();
    ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
    EXPECT_EQ(channel_.Write(std::move(mbuf)).status, OkStatus());
  }

  // Ensure that we can't send any more packets.
  constexpr uint8_t kPayloadData[] = {0x01};
  const ConstByteSpan payload = as_bytes(span(kPayloadData));
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf)).status, Status::Unavailable());

  // Receive a PDU with credits.
  const uint8_t credits = 5;
  pw::Vector<uint8_t, 0> pdu_vec = {};
  channel_.HandlePduFromController(5, as_bytes(span(pdu_vec)));

  // Try to send a packet again.
  for (int i = 0; i < credits; ++i) {
    constexpr uint8_t kPayloadData1[] = {0x01};
    const ConstByteSpan payload1 = as_bytes(span(kPayloadData1));
    auto mbuf1_result = multibuf_allocator_.AllocateContiguous(payload1.size());
    ASSERT_TRUE(mbuf1_result.has_value());
    multibuf::MultiBuf& mbuf1 = mbuf1_result.value();
    ASSERT_EQ(mbuf1.CopyFrom(as_bytes(span(payload1))).status(),
              pw::OkStatus());
    EXPECT_EQ(channel_.Write(std::move(mbuf1)).status, OkStatus());
  }

  // Ensure that we can't send any more packets since the credits are exhausted.
  constexpr uint8_t kPayloadData1[] = {0x01};
  const ConstByteSpan payload1 = as_bytes(span(kPayloadData1));
  auto mbuf1_result = multibuf_allocator_.AllocateContiguous(payload1.size());
  ASSERT_TRUE(mbuf1_result.has_value());
  multibuf::MultiBuf& mbuf1 = mbuf1_result.value();
  ASSERT_EQ(mbuf1.CopyFrom(as_bytes(span(payload1))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf1)).status, Status::Unavailable());
}

TEST_F(RfcommChannelTest, AutoSendCredits) {
  for (uint16_t i = 0; i < kDefaultRxConfig.initial_credits / 2; ++i) {
    const pw::Vector<uint8_t, 3> pdu_vec = {0x01, 0x02, 0x03};
    channel_.HandlePduFromController(0, as_bytes(span(pdu_vec)));
  }
  // A credit packet should be sent.
  ASSERT_FALSE(l2cap_channel_for_test_.written_payloads().empty());
  // A credit packet is a UIH frame with a 1-byte payload.
  EXPECT_EQ(l2cap_channel_for_test_.written_payloads().front().size(),
            1 + static_cast<size_t>(
                    emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));

  const span<const uint8_t> written_payload =
      l2cap_channel_for_test_.written_payloads().front();

  // Address field: channel_number=2, D=1 (initiated by initiator), C/R=1 (from
  // initiator), EA=1
  const uint8_t expected_address =
      (kChannelNumber << 3) | (1 << 2) | (1 << 1) | 1;
  EXPECT_EQ(written_payload[0], expected_address);

  // Control field: UIH with P/F bit.
  EXPECT_EQ(written_payload[1],
            static_cast<uint8_t>(
                emboss::RfcommFrameType::
                    UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL));

  // Length field: 0 byte of info.
  const uint8_t expected_length = (0 << 1) | 1;
  EXPECT_EQ(written_payload[2], expected_length);

  // Info field: number of credits.
  EXPECT_EQ(written_payload[3], kDefaultRxConfig.initial_credits / 2);
}

TEST_F(RfcommChannelTest, SendAdditionalRxCredits) {
  const uint8_t kAdditionalCredits = 5;
  EXPECT_EQ(channel_.SendAdditionalRxCredits(kAdditionalCredits), OkStatus());

  ASSERT_FALSE(l2cap_channel_for_test_.written_payloads().empty());
  // A credit packet is a UIH frame with a length of 0.
  EXPECT_EQ(l2cap_channel_for_test_.written_payloads().front().size(),
            1 + static_cast<size_t>(
                    emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));

  const span<const uint8_t> written_payload =
      l2cap_channel_for_test_.written_payloads().front();

  // Address field: channel_number=2, D=1 (initiated by initiator), C/R=1 (from
  // initiator), EA=1
  const uint8_t expected_address =
      (kChannelNumber << 3) | (1 << 2) | (1 << 1) | 1;
  EXPECT_EQ(written_payload[0], expected_address);

  // Control field: UIH with P/F bit.
  EXPECT_EQ(written_payload[1],
            static_cast<uint8_t>(
                emboss::RfcommFrameType::
                    UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL));

  // Length field: 0 byte of info.
  const uint8_t expected_length = (0 << 1) | 1;
  EXPECT_EQ(written_payload[2], expected_length);

  // Info field: number of credits.
  EXPECT_EQ(written_payload[3], kAdditionalCredits);
}

TEST_F(RfcommChannelTest, Close) {
  channel_.Close(RfcommEvent::kChannelClosedByOther);
  EXPECT_EQ(last_event_, RfcommEvent::kChannelClosedByOther);
  // Write should fail on a closed channel.
  constexpr uint8_t kPayloadData[] = {0x01, 0x02, 0x03};
  const ConstByteSpan payload = as_bytes(span(kPayloadData));
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf)).status, Status::NotFound());
}

TEST_F(RfcommChannelTest, WriteSinglePacketAsNonInitiator) {
  RfcommChannelInternal channel(
      multibuf_allocator_,
      l2cap_channel_for_test_,
      kConnectionHandle,
      kChannelNumber,
      RfcommDirection::kResponder,
      false,  // mux_initiator = false
      kDefaultRxConfig,
      kDefaultTxConfig,
      kRfcommCrc,
      [this](multibuf::MultiBuf&& pdu) {
        last_received_pdu_.resize(pdu.size());
        std::ignore = pdu.CopyTo(as_writable_bytes(span(last_received_pdu_)));
      },
      [this](RfcommEvent event) { last_event_ = event; });

  const pw::Vector<uint8_t, 3> payload = {0, 0, 0};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel.Write(std::move(mbuf)).status, OkStatus());

  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 1u);
  const span<const uint8_t> written_payload =
      l2cap_channel_for_test_.written_payloads().front();

  ASSERT_FALSE(written_payload.empty());
  EXPECT_EQ(
      written_payload.size(),
      payload.size() + static_cast<size_t>(
                           emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));

  // Address field: channel_number=2, D=0 (initiated by responder), C/R=0 (from
  // responder), EA=1
  const uint8_t expected_address =
      (kChannelNumber << 3) | (0 << 2) | (0 << 1) | 1;
  EXPECT_EQ(written_payload[0], expected_address);

  // Control field: UIH.
  EXPECT_EQ(
      written_payload[1],
      static_cast<uint8_t>(
          emboss::RfcommFrameType::UNNUMBERED_INFORMATION_WITH_HEADER_CHECK));

  // Length field: 3 bytes of info.
  const uint8_t expected_length =
      static_cast<uint8_t>((payload.size() << 1) | 1);
  EXPECT_EQ(written_payload[2], expected_length);

  // Verify the payload.
  const span<const uint8_t> written_data = written_payload.subspan(
      static_cast<size_t>(emboss::RfcommHeaderLength::WITH_LENGTH),
      payload.size());
  EXPECT_TRUE(std::equal(written_data.begin(),
                         written_data.end(),
                         payload.begin(),
                         payload.end()));
}

TEST_F(RfcommChannelTest, HandlePduWithData) {
  const pw::Vector<uint8_t, 3> pdu_data = {0x01, 0x02, 0x03};
  channel_.HandlePduFromController(0, as_bytes(span(pdu_data)));

  ASSERT_EQ(last_received_pdu_.size(), pdu_data.size());
  EXPECT_TRUE(std::equal(
      last_received_pdu_.begin(), last_received_pdu_.end(), pdu_data.begin()));
}

TEST_F(RfcommChannelTest, WriteResumesAfterL2capChannelBecomesAvailable) {
  // Make the first write fail with Unavailable.
  l2cap_channel_for_test_.set_next_write_status(Status::Unavailable());

  const pw::Vector<uint8_t, 2> payload1 = {0xAA, 0xBB};
  auto mbuf1_result = multibuf_allocator_.AllocateContiguous(payload1.size());
  ASSERT_TRUE(mbuf1_result.has_value());
  multibuf::MultiBuf mbuf1 = std::move(*mbuf1_result);
  ASSERT_EQ(mbuf1.CopyFrom(as_bytes(span(payload1))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf1)).status, OkStatus());

  // The packet should not have been sent.
  EXPECT_TRUE(l2cap_channel_for_test_.written_payloads().empty());

  // Now, try writing a second packet. The L2CAP channel is now available.
  const pw::Vector<uint8_t, 2> payload2 = {0xCC, 0xDD};
  auto mbuf2_result = multibuf_allocator_.AllocateContiguous(payload2.size());
  ASSERT_TRUE(mbuf2_result.has_value());
  multibuf::MultiBuf mbuf2 = std::move(*mbuf2_result);
  ASSERT_EQ(mbuf2.CopyFrom(as_bytes(span(payload2))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf2)).status, OkStatus());

  // Both packets should have been sent now.
  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 2u);

  // Verify the first packet.
  const span<const uint8_t> written_payload1 =
      l2cap_channel_for_test_.written_payloads()[0];
  const span<const uint8_t> written_data1 = written_payload1.subspan(
      static_cast<size_t>(emboss::RfcommHeaderLength::WITH_LENGTH),
      payload1.size());
  EXPECT_TRUE(std::equal(written_data1.begin(),
                         written_data1.end(),
                         payload1.begin(),
                         payload1.end()));

  // Verify the second packet.
  const span<const uint8_t> written_payload2 =
      l2cap_channel_for_test_.written_payloads()[1];
  const span<const uint8_t> written_data2 = written_payload2.subspan(
      static_cast<size_t>(emboss::RfcommHeaderLength::WITH_LENGTH),
      payload2.size());
  EXPECT_TRUE(std::equal(written_data2.begin(),
                         written_data2.end(),
                         payload2.begin(),
                         payload2.end()));
}

TEST_F(RfcommChannelTest, CreditFrameIsPrioritized) {
  // 1. Exhaust all credits.
  for (int i = 0; i < kDefaultTxConfig.initial_credits; ++i) {
    const pw::Vector<uint8_t, 1> kTestData = {static_cast<uint8_t>(i)};
    auto mbuf_result = multibuf_allocator_.AllocateContiguous(kTestData.size());
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf new_buffer = std::move(mbuf_result.value());
    ASSERT_EQ(new_buffer.CopyFrom(as_bytes(span(kTestData))).status(),
              pw::OkStatus());
    EXPECT_EQ(channel_.Write(std::move(new_buffer)).status, OkStatus());
  }
  EXPECT_EQ(l2cap_channel_for_test_.written_payloads().size(),
            static_cast<size_t>(kDefaultTxConfig.initial_credits));

  // 2. Queue a data packet. It won't be sent due to lack of credits.
  const pw::Vector<uint8_t, 1> kPendingData = {0x42};
  auto mbuf_result =
      multibuf_allocator_.AllocateContiguous(kPendingData.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf new_buffer1 = std::move(mbuf_result.value());
  ASSERT_EQ(new_buffer1.CopyFrom(as_bytes(span(kPendingData))).status(),
            pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(new_buffer1)).status, OkStatus());
  // No new packet sent.
  EXPECT_EQ(l2cap_channel_for_test_.written_payloads().size(),
            static_cast<size_t>(kDefaultTxConfig.initial_credits));

  // 3. Trigger queuing of a credit frame by consuming receive credits.
  // The credit frame should be sent immediately, as it doesn't consume a
  // transmit credit.
  const int credits_to_consume = kDefaultRxConfig.initial_credits / 2 + 1;
  for (int i = 0; i < credits_to_consume; ++i) {
    // A data-less PDU also consumes rx credits.
    const pw::Vector<uint8_t, 1> pdu_vec = {0x01};
    channel_.HandlePduFromController(0, as_bytes(span(pdu_vec)));
  }
  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(),
            static_cast<size_t>(kDefaultTxConfig.initial_credits + 1));

  // 4. Provide one transmit credit. This should trigger sending the pending
  // data frame.
  const pw::Vector<uint8_t, 0> pdu_vec = {};
  channel_.HandlePduFromController(1, as_bytes(span(pdu_vec)));

  // 5. Verify that two new frames were sent and the credit frame was first.
  EXPECT_EQ(l2cap_channel_for_test_.written_payloads().size(),
            static_cast<size_t>(kDefaultTxConfig.initial_credits + 2));

  const auto& written_payloads = l2cap_channel_for_test_.written_payloads();
  const auto& credit_frame = written_payloads[kDefaultTxConfig.initial_credits];
  const auto& data_frame =
      written_payloads[kDefaultTxConfig.initial_credits + 1];

  // The control field is the second byte in the frame.
  // Credit frames have P/F bit set.
  auto credit_frame_view =
      emboss::MakeRfcommFrameView(credit_frame.data(), credit_frame.size());
  EXPECT_TRUE(credit_frame_view.Ok());
  EXPECT_EQ(credit_frame_view.control().Read(),
            emboss::RfcommFrameType::
                UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL);

  auto data_frame_view =
      emboss::MakeRfcommFrameView(data_frame.data(), data_frame.size());
  EXPECT_TRUE(data_frame_view.Ok());
  EXPECT_EQ(data_frame_view.control().Read(),
            emboss::RfcommFrameType::UNNUMBERED_INFORMATION_WITH_HEADER_CHECK);
}

TEST_F(RfcommChannelTest, ReceivePacketWithNoCreditsDoesNotUnderflow) {
  constexpr RfcommChannelConfig kRxConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 0};
  RfcommChannelInternal channel(
      multibuf_allocator_,
      l2cap_channel_for_test_,
      kConnectionHandle,
      kChannelNumber,
      RfcommDirection::kInitiator,
      true,
      kRxConfig,
      kDefaultTxConfig,
      kRfcommCrc,
      [this](multibuf::MultiBuf&& pdu) {
        last_received_pdu_.resize(pdu.size());
        std::ignore = pdu.CopyTo(as_writable_bytes(span(last_received_pdu_)));
      },
      [this](RfcommEvent event) { last_event_ = event; });

  // Receive one packet to exhaust the credits.
  const pw::Vector<uint8_t, 3> pdu_data = {0x01, 0x02, 0x03};
  channel.HandlePduFromController(0, as_bytes(span(pdu_data)));

  ASSERT_EQ(last_received_pdu_.size(), pdu_data.size());
  last_received_pdu_.clear();

  const pw::Vector<uint8_t, 3> pdu_data2 = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  // Receive another packet. This should be accepted but log a warning.
  channel.HandlePduFromController(0, as_bytes(span(pdu_data)));
  EXPECT_EQ(last_received_pdu_.size(), pdu_data.size());

  // The channel should not be closed.
  EXPECT_FALSE(last_event_.has_value());
}

TEST_F(RfcommChannelTest, TxCreditsOverflow) {
  const pw::Vector<uint8_t, 3> payload = {0, 0, 0};
  // Make that detects the tx credits overflow.
  channel_.HandlePduFromController(std::numeric_limits<uint8_t>::max(), {});
  // Write more packets (255[uint8_t max] + 10 [tx queue size]) to trigger
  // the Unavailable status.
  for (int i = 0; i < 255 + 10; ++i) {
    auto mbuf = multibuf_allocator_.AllocateContiguous(payload.size());
    ASSERT_TRUE(mbuf.has_value());
    EXPECT_EQ(channel_.Write(std::move(*mbuf)).status, OkStatus());
  }
  // The channel should return status unavailable since the queue is full and no
  // TX credits are available.
  auto mbuf = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf.has_value());
  EXPECT_EQ(channel_.Write(std::move(*mbuf)).status, Status::Unavailable());
}

TEST_F(RfcommChannelTest, SendCreditsFailsIfAlreadyPending) {
  // Make the write fail and return the buffer, so it stays pending.
  l2cap_channel_for_test_.set_next_write_status(Status::Unavailable());

  // This will queue the credit packet and try to send it, which fails.
  EXPECT_EQ(channel_.SendAdditionalRxCredits(5), OkStatus());

  // A subsequent attempt to send credits should fail because one is already
  // pending.
  EXPECT_EQ(channel_.SendAdditionalRxCredits(5), Status::FailedPrecondition());
}

TEST_F(RfcommChannelTest, PendingCreditsRestoredOnWriteFailure) {
  // Make the write fail but return the buffer.
  l2cap_channel_for_test_.set_next_write_status(Status::Unavailable());

  // Queue the credit packet.
  EXPECT_EQ(channel_.SendAdditionalRxCredits(5), OkStatus());

  // Verify nothing was actually written yet.
  EXPECT_TRUE(l2cap_channel_for_test_.written_payloads().empty());

  // Trigger a successful write of a data packet.
  // This should trigger TryToSendPacket, which should first send the pending
  // credit packet, and then the data packet.
  const pw::Vector<uint8_t, 3> payload = {1, 2, 3};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(payload.size());
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf& mbuf = mbuf_result.value();
  ASSERT_EQ(mbuf.CopyFrom(as_bytes(span(payload))).status(), pw::OkStatus());
  EXPECT_EQ(channel_.Write(std::move(mbuf)).status, OkStatus());

  // Verify both packets were written.
  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 2u);

  // First packet should be the credit packet (UIH with P/F bit).
  const auto& credit_payload = l2cap_channel_for_test_.written_payloads()[0];
  auto credit_frame_view =
      emboss::MakeRfcommFrameView(credit_payload.data(), credit_payload.size());
  ASSERT_TRUE(credit_frame_view.Ok());
  EXPECT_EQ(credit_frame_view.control().Read(),
            emboss::RfcommFrameType::
                UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL);
  EXPECT_EQ(credit_frame_view.credits().Read(), 5);

  // Second packet should be the data packet.
  const auto& data_payload = l2cap_channel_for_test_.written_payloads()[1];
  auto data_frame_view =
      emboss::MakeRfcommFrameView(data_payload.data(), data_payload.size());
  ASSERT_TRUE(data_frame_view.Ok());
  EXPECT_EQ(data_frame_view.control().Read(),
            emboss::RfcommFrameType::UNNUMBERED_INFORMATION_WITH_HEADER_CHECK);
}

TEST_F(RfcommChannelTest, PendingCreditsLostIfBufferNotReturnedOnWriteFailure) {
  // Make the write fail and NOT return the buffer.
  l2cap_channel_for_test_.set_next_write_status(Status::Unavailable(),
                                                /*return_buffer=*/false);

  // Queue the credit packet. It will fail to write and the buffer will be lost.
  EXPECT_EQ(channel_.SendAdditionalRxCredits(5), OkStatus());

  // Verify nothing was written.
  EXPECT_TRUE(l2cap_channel_for_test_.written_payloads().empty());

  // Since the buffer was lost, pending_credit_tx_ should be reset.
  // A subsequent call should succeed to queue (and not return
  // FailedPrecondition). We let this one succeed.
  EXPECT_EQ(channel_.SendAdditionalRxCredits(5), OkStatus());

  // Verify it was written now.
  ASSERT_EQ(l2cap_channel_for_test_.written_payloads().size(), 1u);
  const auto& credit_payload = l2cap_channel_for_test_.written_payloads()[0];
  auto credit_frame_view =
      emboss::MakeRfcommFrameView(credit_payload.data(), credit_payload.size());
  ASSERT_TRUE(credit_frame_view.Ok());
  EXPECT_EQ(credit_frame_view.credits().Read(), 5);
}

}  // namespace pw::bluetooth::proxy::rfcomm::internal
