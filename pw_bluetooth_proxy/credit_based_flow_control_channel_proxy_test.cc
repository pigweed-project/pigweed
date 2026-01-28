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

#include "pw_allocator/testing.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy_private/test_utils.h"

namespace pw::bluetooth::proxy {
namespace {

constexpr uint16_t kMaxAclPacketLength = 27;
constexpr uint16_t kConnectionHandle = 0x123;
constexpr uint16_t kRemoteChannelId = 0x1234;
constexpr uint16_t kLocalChannelId = 0x4321;
constexpr uint8_t kNumEventsSentToHost = 2u;

constexpr std::array<uint8_t, 3> kExpectedPayload = {0x01, 0x02, 0x03};
constexpr std::array<std::byte, 3> kExpectedPayloadBytes = {
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

constexpr std::array<uint8_t, 14> kTxH4Packet = {0x02,  // H4 type (ACL)
                                                        // ACL header
                                                 0x23,
                                                 0x1,  // connection handle
                                                 0x09,
                                                 0x00,  // ACL length
                                                        // l2cap header
                                                 0x5,
                                                 0x00,  // payload length
                                                 0x34,
                                                 0x12,  // remote channel id
                                                 0x03,
                                                 0x00,  // SDU length
                                                 // payload
                                                 0x1,
                                                 0x2,
                                                 0x3};

constexpr std::array<uint8_t, 13> kRxAclPacket = {
    // ACL header
    0x23,
    0x01,  // connection handle
    0x09,
    0x00,  // ACL length
           // l2cap header
    0x05,
    0x00,  // payload length
    0x21,
    0x43,  // local channel id
    0x03,
    0x00,  // SDU length
           // payload
    0x01,
    0x02,
    0x03};

class CreditBasedFlowControlChannelProxyTest : public ProxyHostTest {
 public:
  void SetUp() override {
    Function<void(H4PacketWithHci&&)>&& send_to_host_fn(
        [this](H4PacketWithHci&&) { ++sent_to_host_count_; });
    Function<void(H4PacketWithH4&&)>&& send_to_controller_fn(
        [this](H4PacketWithH4&& packet) {
          ++sent_to_controller_count_;
          sent_to_controller_packets_.emplace_back(std::move(packet));
        });

    proxy_host_.emplace(std::move(send_to_host_fn),
                        std::move(send_to_controller_fn),
                        /*le_acl_credits_to_reserve=*/10,
                        /*br_edr_acl_credits_to_reserve=*/0,
                        &allocator_);
  }

  void TearDown() override {
    sent_to_controller_packets_.clear();
    proxy_host_.reset();
  }

  void SendEvents(bool receive_read_buffer_response = true) {
    StartDispatcherOnCurrentThread(proxy_host_.value());
    if (receive_read_buffer_response) {
      PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
          proxy_host_.value(),
          /*num_credits_to_reserve=*/10,
          /*le_acl_data_packet_length=*/kMaxAclPacketLength));
    }
    PW_TEST_EXPECT_OK(SendLeConnectionCompleteEvent(
        proxy_host_.value(), kConnectionHandle, emboss::StatusCode::SUCCESS));
  }

  Result<UniquePtr<ChannelProxy>> CreateChannel() {
    MultiBufReceiveFunction from_controller_fn =
        [this](FlatConstMultiBuf&& payload) {
          ++payloads_from_controller_count_;
          EXPECT_TRUE(std::equal(payload.begin(),
                                 payload.end(),
                                 kExpectedPayloadBytes.begin(),
                                 kExpectedPayloadBytes.end()));
        };

    ConnectionOrientedChannelConfig rx_config{
        .cid = kLocalChannelId, .mtu = 100, .mps = 100, .credits = 50};
    ConnectionOrientedChannelConfig tx_config{
        .cid = kRemoteChannelId, .mtu = 100, .mps = 100, .credits = 50};
    return proxy_host_->InterceptCreditBasedFlowControlChannel(
        ConnectionHandle{kConnectionHandle},
        rx_config,
        tx_config,
        std::move(from_controller_fn),
        [this](L2capChannelEvent event) { events_.push_back(event); });
  }

  void ExhaustAllocator() { allocator_.Exhaust(); }

  int sent_to_controller_count() { return sent_to_controller_count_; }

  int sent_to_host_count() { return sent_to_host_count_; }

  const Vector<H4PacketWithH4>& sent_to_controller_packets() const {
    return sent_to_controller_packets_;
  }

  Vector<L2capChannelEvent>& events() { return events_; }

  ProxyHost& proxy() { return proxy_host_.value(); }

  int payloads_from_controller_count() const {
    return payloads_from_controller_count_;
  }

 private:
  std::optional<ProxyHost> proxy_host_;
  allocator::test::AllocatorForTest<4000> allocator_;
  int sent_to_controller_count_ = 0;
  Vector<H4PacketWithH4, 5> sent_to_controller_packets_;
  int sent_to_host_count_ = 0;
  int payloads_from_controller_count_ = 0;
  Vector<L2capChannelEvent, 5> events_;
};

TEST_F(CreditBasedFlowControlChannelProxyTest, SendAdditionalRxCredits) {
  SendEvents();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  EXPECT_EQ(sent_to_controller_count(), 0);
  Status credit_status = channel->SendAdditionalRxCredits(3);
  PW_TEST_ASSERT_OK(credit_status);
  RunDispatcher();
  ASSERT_EQ(sent_to_controller_count(), 1);

  const std::array<uint8_t, 17> kExpectedCreditsPacket = {
      0x02,  // H4 indicator (ACL)
             // ACL header
      0x23,
      0x01,  // connection handle
      0x0c,
      0x00,  // ACL length
             // L2CAP header
      0x08,
      0x00,  // PDU length
      0x05,
      0x00,  // channel ID (LE signaling channel)
      // Signaling packet header
      0x16,  // code (L2CAP_FLOW_CONTROL_CREDIT_IND)
      0x01,  // signaling channel message identifier
      0x04,
      0x00,  // data length
      0x21,
      0x43,  // channel ID
      0x03,
      0x00,  // credits
  };
  span<const uint8_t> sent_packet = sent_to_controller_packets()[0].GetH4Span();
  EXPECT_TRUE(std::equal(sent_packet.begin(),
                         sent_packet.end(),
                         kExpectedCreditsPacket.begin(),
                         kExpectedCreditsPacket.end()));
}

TEST_F(CreditBasedFlowControlChannelProxyTest, WriteSuccess) {
  SendEvents();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 3> payload = kExpectedPayload;
  FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(span(payload));
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(channel->IsWriteAvailable());
  PW_TEST_EXPECT_OK(channel->Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(sent_to_controller_count(), 1);
  span<const uint8_t> sent_packet = sent_to_controller_packets()[0].GetH4Span();
  EXPECT_TRUE(std::equal(sent_packet.begin(),
                         sent_packet.end(),
                         kTxH4Packet.begin(),
                         kTxH4Packet.end()));
}

TEST_F(CreditBasedFlowControlChannelProxyTest,
       WriteUntilQueueFullThenGetWriteAvailableEvent) {
  SendEvents();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  Status write_status = OkStatus();
  while (write_status.ok()) {
    std::array<uint8_t, 3> payload = kExpectedPayload;
    FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(span(payload));
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    write_status = channel->Write(std::move(mbuf)).status;
    RunDispatcher();
  }
  EXPECT_EQ(channel->IsWriteAvailable(), Status::Unavailable());

  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy(), {{kConnectionHandle, 1}}));
  ASSERT_EQ(events().size(), 1u);
  EXPECT_EQ(events().back(), L2capChannelEvent::kWriteAvailable);
  PW_TEST_EXPECT_OK(channel->IsWriteAvailable());
}

TEST_F(CreditBasedFlowControlChannelProxyTest, ReceiveFromController) {
  SendEvents();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 13> rx_acl = kRxAclPacket;
  H4PacketWithHci h4_packet(emboss::H4PacketType::ACL_DATA, rx_acl);
  proxy().HandleH4HciFromController(std::move(h4_packet));
  EXPECT_EQ(payloads_from_controller_count(), 1);
  EXPECT_EQ(sent_to_host_count(), kNumEventsSentToHost);
}

TEST_F(CreditBasedFlowControlChannelProxyTest, ChannelAllocationFails) {
  SendEvents();
  ExhaustAllocator();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  EXPECT_EQ(channel_result.status(), Status::ResourceExhausted());
}

TEST_F(CreditBasedFlowControlChannelProxyTest, InvalidConnectionHandle) {
  SendEvents();
  ConnectionOrientedChannelConfig rx_config{
      .cid = kLocalChannelId, .mtu = 100, .mps = 100, .credits = 50};
  ConnectionOrientedChannelConfig tx_config{
      .cid = kRemoteChannelId, .mtu = 100, .mps = 100, .credits = 50};
  Result<UniquePtr<ChannelProxy>> channel_result =
      proxy().InterceptCreditBasedFlowControlChannel(
          ConnectionHandle{1337},
          rx_config,
          tx_config,
          [](FlatConstMultiBuf&&) {},
          [](L2capChannelEvent) {});
  EXPECT_EQ(channel_result.status(), Status::InvalidArgument());
}

TEST_F(CreditBasedFlowControlChannelProxyTest, ChannelClosedByOther) {
  SendEvents();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  PW_TEST_ASSERT_OK(channel_result);
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy(),
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kConnectionHandle,
                                           kLocalChannelId,
                                           kRemoteChannelId));
  ASSERT_EQ(events().size(), 1u);
  EXPECT_EQ(events().back(), L2capChannelEvent::kChannelClosedByOther);
}

TEST_F(CreditBasedFlowControlChannelProxyTest,
       CheckWriteParameterFailsPayloadTooLarge) {
  SendEvents();
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, kMaxAclPacketLength> payload = {};
  payload.fill(0xFF);
  FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(span(payload));
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(channel->IsWriteAvailable());
  EXPECT_EQ(channel->Write(std::move(mbuf)).status, Status::InvalidArgument());
  RunDispatcher();
  EXPECT_EQ(sent_to_controller_count(), 0);
}

TEST_F(CreditBasedFlowControlChannelProxyTest,
       CheckWriteParameterFailsBufferSizeUnknown) {
  SendEvents(/*receive_read_buffer_response=*/false);
  Result<UniquePtr<ChannelProxy>> channel_result = CreateChannel();
  EXPECT_EQ(channel_result.status(), Status::FailedPrecondition());
}

}  // namespace
}  // namespace pw::bluetooth::proxy
