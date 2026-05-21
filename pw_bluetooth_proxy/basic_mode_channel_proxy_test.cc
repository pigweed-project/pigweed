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

#include "pw_allocator/testing.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy_private/test_utils.h"

namespace pw::bluetooth::proxy {

namespace {

constexpr uint16_t kConnectionHandle = 0x123;
constexpr uint16_t kRemoteChannelId = 0x1234;
constexpr uint16_t kLocalChannelId = 0x4321;
constexpr uint8_t kNumEventsSentToHost = 2u;

using SpanReceiveFn = L2capChannelManagerInterface::SpanReceiveFunction;
using MultiBufReceiveFn =
    L2capChannelManagerInterface::OptionalBufferReceiveFunction;

constexpr std::array<uint8_t, 12> kTxH4Packet = {0x02,  // H4 type (ACL)
                                                        // ACL header
                                                 0x23,
                                                 0x1,  // connection handle
                                                 0x07,
                                                 0x00,  // ACL length
                                                        // l2cap header
                                                 0x3,
                                                 00,  // payload length
                                                 0x34,
                                                 0x12,  // remote channel id
                                                        // payload
                                                 0x1,
                                                 0x2,
                                                 0x3};

constexpr std::array<uint8_t, 12> kRxAclPacket = {
    // ACL header
    0x23,
    0x1,  // connection handle
    0x07,
    0x00,  // ACL length
           // l2cap header
    0x3,
    00,  // payload length
    0x21,
    0x43,  // local channel id
           // payload
    0x1,
    0x2,
    0x3};

constexpr uint16_t kMaxAclPacketLength = 27;

constexpr std::array<uint8_t, 3> kExpectedPayload = {0x01, 0x02, 0x03};
constexpr std::array<std::byte, 3> kExpectedPayloadBytes = {
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

class BasicModeChannelProxyTest : public ProxyHostTest {
 public:
  // ProxyHost is too large for the test fixture with the "light" test backend,
  // so it has to be constructed on the test stack.
  ProxyHost CreateProxy() {
    Function<void(H4PacketWithHci&&)>&& send_to_host_fn(
        [this](H4PacketWithHci&&) { ++sent_to_host_count_; });
    Function<void(H4PacketWithH4&&)>&& send_to_controller_fn(
        [this](H4PacketWithH4&& packet) {
          ++sent_to_controller_count_;
          sent_to_controller_packets_.emplace_back(std::move(packet));
        });

    return ProxyHost(std::move(send_to_host_fn),
                     std::move(send_to_controller_fn),
                     /*le_acl_credits_to_reserve=*/10,
                     /*br_edr_acl_credits_to_reserve=*/0,
                     &allocator_);
  }

  void SendEvents(ProxyHost& proxy_host,
                  bool receive_read_buffer_response = true) {
    StartDispatcherOnCurrentThread(proxy_host);
    if (receive_read_buffer_response) {
      PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
          proxy_host,
          /*num_credits_to_reserve=*/10,
          /*le_acl_data_packet_length=*/kMaxAclPacketLength));
    }
    PW_TEST_EXPECT_OK(SendLeConnectionCompleteEvent(
        proxy_host, kConnectionHandle, emboss::StatusCode::SUCCESS));
  }

  void TearDown() override {
    sent_to_controller_packets_.clear();
    ProxyHostTest::TearDown();
  }

  template <bool kConsumePayloads = false>
  Result<UniquePtr<ChannelProxy>> CreateChannelWithSpanCallbacks(
      ProxyHost& proxy_host) {
    SpanReceiveFn from_controller_fn = [this](
                                           ConstByteSpan payload,
                                           ConnectionHandle connection_handle,
                                           uint16_t local_channel_id,
                                           uint16_t remote_channel_id) -> bool {
      ++payloads_from_controller_count_;
      EXPECT_EQ(connection_handle, ConnectionHandle{kConnectionHandle});
      EXPECT_EQ(local_channel_id, kLocalChannelId);
      EXPECT_EQ(remote_channel_id, kRemoteChannelId);
      EXPECT_TRUE(std::equal(payload.begin(),
                             payload.end(),
                             kExpectedPayloadBytes.begin(),
                             kExpectedPayloadBytes.end()));
      return kConsumePayloads;
    };

    SpanReceiveFn from_host_fn = [this](ConstByteSpan payload,
                                        ConnectionHandle connection_handle,
                                        uint16_t local_channel_id,
                                        uint16_t remote_channel_id) -> bool {
      ++payloads_from_host_count_;
      EXPECT_EQ(connection_handle, ConnectionHandle{kConnectionHandle});
      EXPECT_EQ(local_channel_id, kLocalChannelId);
      EXPECT_EQ(remote_channel_id, kRemoteChannelId);
      EXPECT_TRUE(std::equal(payload.begin(),
                             payload.end(),
                             kExpectedPayloadBytes.begin(),
                             kExpectedPayloadBytes.end()));
      return kConsumePayloads;
    };

    return proxy_host.InterceptBasicModeChannel(
        ConnectionHandle{kConnectionHandle},
        kLocalChannelId,
        kRemoteChannelId,
        AclTransportType::kLe,
        std::move(from_controller_fn),
        std::move(from_host_fn),
        [this](L2capChannelEvent event) { events_.push_back(event); });
  }

  template <bool kConsumePayloads = false>
  Result<UniquePtr<ChannelProxy>> CreateChannelWithMultiBufCallbacks(
      ProxyHost& proxy_host) {
    MultiBufReceiveFn from_controllerr_fn =
        [this](
            multibuf::MultiBuf&& payload,
            ConnectionHandle connection_handle,
            uint16_t local_channel_id,
            uint16_t remote_channel_id) -> std::optional<multibuf::MultiBuf> {
      ++payloads_from_controller_count_;
      EXPECT_EQ(connection_handle, ConnectionHandle{kConnectionHandle});
      EXPECT_EQ(local_channel_id, kLocalChannelId);
      EXPECT_EQ(remote_channel_id, kRemoteChannelId);
      EXPECT_TRUE(std::equal(payload.begin(),
                             payload.end(),
                             kExpectedPayloadBytes.begin(),
                             kExpectedPayloadBytes.end()));
      if (kConsumePayloads) {
        return std::nullopt;
      }
      return std::move(payload);
    };

    MultiBufReceiveFn from_host_fn =
        [this](
            multibuf::MultiBuf&& payload,
            ConnectionHandle connection_handle,
            uint16_t local_channel_id,
            uint16_t remote_channel_id) -> std::optional<multibuf::MultiBuf> {
      ++payloads_from_host_count_;
      EXPECT_EQ(connection_handle, ConnectionHandle{kConnectionHandle});
      EXPECT_EQ(local_channel_id, kLocalChannelId);
      EXPECT_EQ(remote_channel_id, kRemoteChannelId);
      EXPECT_TRUE(std::equal(payload.begin(),
                             payload.end(),
                             kExpectedPayloadBytes.begin(),
                             kExpectedPayloadBytes.end()));
      if (kConsumePayloads) {
        return std::nullopt;
      }
      return std::move(payload);
    };

    return proxy_host.InterceptBasicModeChannel(
        ConnectionHandle{kConnectionHandle},
        kLocalChannelId,
        kRemoteChannelId,
        AclTransportType::kLe,
        std::move(from_controllerr_fn),
        std::move(from_host_fn),
        [this](L2capChannelEvent event) { events_.push_back(event); });
  }

  void ExhaustAllocator() { allocator_.Exhaust(); }

  int sent_to_controller_count() { return sent_to_controller_count_; }

  int sent_to_host_count() { return sent_to_host_count_; }

  const Vector<H4PacketWithH4>& sent_to_controller_packets() const {
    return sent_to_controller_packets_;
  }

  int payloads_from_controller_count() const {
    return payloads_from_controller_count_;
  }

  int payloads_from_host_count() const { return payloads_from_host_count_; }

  Vector<L2capChannelEvent>& events() { return events_; }

 private:
  allocator::test::AllocatorForTest<4000> allocator_;
  int sent_to_controller_count_ = 0;
  Vector<H4PacketWithH4, 5> sent_to_controller_packets_;
  int sent_to_host_count_ = 0;
  int payloads_from_controller_count_ = 0;
  int payloads_from_host_count_ = 0;
  Vector<L2capChannelEvent, 5> events_;
};

TEST_F(BasicModeChannelProxyTest, WriteSuccess) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);

  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 3> payload = kExpectedPayload;
  multibuf::MultiBuf mbuf = MultiBufFromSpan(span(payload));
  PW_TEST_EXPECT_OK(channel->IsWriteAvailable());
  PW_TEST_EXPECT_OK(channel->Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(sent_to_controller_count(), 1);
  span<const uint8_t> sent_packet = sent_to_controller_packets()[0].GetH4Span();
  EXPECT_TRUE(std::equal(sent_packet.begin(),
                         sent_packet.end(),
                         kTxH4Packet.begin(),
                         kTxH4Packet.end()));
  TearDown();
}

TEST_F(BasicModeChannelProxyTest,
       WriteUntilQueueFullThenGetWriteAvailableEvent) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  Status write_status = OkStatus();
  while (write_status.ok()) {
    std::array<uint8_t, 3> payload = kExpectedPayload;
    multibuf::MultiBuf mbuf = MultiBufFromSpan(span(payload));
    write_status = channel->Write(std::move(mbuf)).status;
    RunDispatcher();
  }
  EXPECT_EQ(channel->IsWriteAvailable(), Status::Unavailable());

  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, 1}}));
  ASSERT_EQ(events().size(), 1u);
  EXPECT_EQ(events().back(), L2capChannelEvent::kWriteAvailable);
  PW_TEST_EXPECT_OK(channel->IsWriteAvailable());
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveSpanFromControllerAndConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks</*kConsumePayloads=*/true>(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 12> rx_acl = kRxAclPacket;
  H4PacketWithHci h4_packet(emboss::H4PacketType::ACL_DATA, rx_acl);
  proxy.HandleH4HciFromController(std::move(h4_packet));
  EXPECT_EQ(payloads_from_controller_count(), 1);
  EXPECT_EQ(sent_to_host_count(), kNumEventsSentToHost);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveSpanFromControllerAndDoNotConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  auto acl_packet = kRxAclPacket;
  H4PacketWithHci h4_packet(emboss::H4PacketType::ACL_DATA, acl_packet);
  proxy.HandleH4HciFromController(std::move(h4_packet));
  EXPECT_EQ(payloads_from_controller_count(), 1);
  EXPECT_EQ(sent_to_host_count(), 1 + kNumEventsSentToHost);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveMultiBufFromControllerAndConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks</*kConsumePayloads=*/true>(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 12> rx_acl = kRxAclPacket;
  H4PacketWithHci h4_packet(emboss::H4PacketType::ACL_DATA, rx_acl);
  proxy.HandleH4HciFromController(std::move(h4_packet));
  EXPECT_EQ(payloads_from_controller_count(), 1);
  EXPECT_EQ(sent_to_host_count(), kNumEventsSentToHost);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest,
       ReceiveMultiBufFromControllerAndDoNotConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  auto acl_packet = kRxAclPacket;
  H4PacketWithHci h4_packet(emboss::H4PacketType::ACL_DATA, acl_packet);
  proxy.HandleH4HciFromController(std::move(h4_packet));
  EXPECT_EQ(payloads_from_controller_count(), 1);
  EXPECT_EQ(sent_to_host_count(), 1 + kNumEventsSentToHost);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveSpanFromHostAndConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks</*kConsumePayloads=*/true>(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 12> h4_data = kTxH4Packet;
  H4PacketWithH4 h4_packet(h4_data);
  proxy.HandleH4HciFromHost(std::move(h4_packet));
  EXPECT_EQ(payloads_from_host_count(), 1);
  EXPECT_EQ(sent_to_controller_count(), 0);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveSpanFromHostAndDoNotConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 12> h4_data = kTxH4Packet;
  H4PacketWithH4 h4_packet(h4_data);
  proxy.HandleH4HciFromHost(std::move(h4_packet));
  EXPECT_EQ(payloads_from_host_count(), 1);
  EXPECT_EQ(sent_to_controller_count(), 1);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveMultiBufFromHostAndConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks</*kConsumePayloads=*/true>(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 12> h4_data = kTxH4Packet;
  H4PacketWithH4 h4_packet(h4_data);
  proxy.HandleH4HciFromHost(std::move(h4_packet));
  EXPECT_EQ(payloads_from_host_count(), 1);
  EXPECT_EQ(sent_to_controller_count(), 0);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReceiveMultiBufFromHostAndDoNotConsume) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, 12> h4_data = kTxH4Packet;
  H4PacketWithH4 h4_packet(h4_data);
  proxy.HandleH4HciFromHost(std::move(h4_packet));
  EXPECT_EQ(payloads_from_host_count(), 1);
  EXPECT_EQ(sent_to_controller_count(), 1);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ChannelAllocationFails) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  ExhaustAllocator();
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks(proxy);
  EXPECT_EQ(channel_result.status(), Status::ResourceExhausted());
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, InvalidConnectionHandle) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      proxy.InterceptBasicModeChannel(
          ConnectionHandle{1337},
          kLocalChannelId,
          kRemoteChannelId,
          AclTransportType::kLe,
          [](auto, auto, auto, auto) { return false; },
          [](auto, auto, auto, auto) { return false; },
          [](L2capChannelEvent) {});
  EXPECT_EQ(channel_result.status(), Status::InvalidArgument());
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, SendRxCreditFails) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());
  Status status = channel->SendAdditionalRxCredits(1);
  EXPECT_EQ(status, Status::Unimplemented());
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ChannelClosedByOther) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithMultiBufCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kConnectionHandle,
                                           kLocalChannelId,
                                           kRemoteChannelId));
  ASSERT_EQ(events().size(), 1u);
  EXPECT_EQ(events().back(), L2capChannelEvent::kChannelClosedByOther);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, CheckWriteParameterFailsPayloadTooLarge) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel_result);
  UniquePtr<ChannelProxy> channel = std::move(channel_result.value());

  std::array<uint8_t, kMaxAclPacketLength> payload = {};
  payload.fill(0xFF);
  multibuf::MultiBuf mbuf = MultiBufFromSpan(span(payload));
  PW_TEST_EXPECT_OK(channel->IsWriteAvailable());
  EXPECT_EQ(channel->Write(std::move(mbuf)).status, Status::InvalidArgument());
  RunDispatcher();
  EXPECT_EQ(sent_to_controller_count(), 0);
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, CheckWriteParameterFailsBufferSizeUnknown) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy, /*receive_read_buffer_response=*/false);
  Result<UniquePtr<ChannelProxy>> channel_result =
      CreateChannelWithSpanCallbacks(proxy);
  EXPECT_EQ(channel_result.status(), Status::FailedPrecondition());
  TearDown();
}

TEST_F(BasicModeChannelProxyTest, RegisterExistingChannelFailsAlreadyExists) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);

  Result<UniquePtr<ChannelProxy>> channel1_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel1_result);

  // Attempting to intercept again with the same parameters while channel1 is
  // active should fail with AlreadyExists.
  Result<UniquePtr<ChannelProxy>> channel2_result =
      CreateChannelWithSpanCallbacks(proxy);
  EXPECT_EQ(channel2_result.status(), Status::AlreadyExists());

  TearDown();
}

TEST_F(BasicModeChannelProxyTest, ReplaceStaleChannelSuccess) {
  ProxyHost proxy = CreateProxy();
  SendEvents(proxy);

  // Create first channel.
  Result<UniquePtr<ChannelProxy>> channel1_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel1_result);

  // Destroy the client channel so that the underlying internal channel becomes
  // stale.
  channel1_result.value().Reset();

  // Re-intercepting with the same parameters should replace the stale channel.
  Result<UniquePtr<ChannelProxy>> channel2_result =
      CreateChannelWithSpanCallbacks(proxy);
  PW_TEST_ASSERT_OK(channel2_result);

  TearDown();
}

}  // namespace

}  // namespace pw::bluetooth::proxy
