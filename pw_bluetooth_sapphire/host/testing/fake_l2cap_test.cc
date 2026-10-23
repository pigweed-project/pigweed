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

#include "pw_bluetooth_sapphire/internal/host/testing/fake_l2cap.h"

#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::testing {

hci_spec::ConnectionHandle kConnectionHandle = 0x01;

class FakeL2capTest : public ::testing::Test {
 public:
  FakeL2capTest() = default;
  ~FakeL2capTest() override = default;

  void SetUp() override {
    auto send_cb = [this](auto handle, auto cid, auto& buffer) {
      if (this->send_frame_callback_) {
        send_frame_callback_(handle, cid, buffer);
      }
    };
    fake_l2cap_ = std::make_unique<FakeL2cap>(send_cb);
  }

 protected:
  void set_send_frame_callback(FakeL2cap::SendFrameCallback cb) {
    send_frame_callback_ = std::move(cb);
  }
  FakeL2cap& fake_l2cap() { return *fake_l2cap_; }

 private:
  FakeL2cap::SendFrameCallback send_frame_callback_;
  std::unique_ptr<FakeL2cap> fake_l2cap_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeL2capTest);
};

TEST_F(FakeL2capTest, RegisterHandler) {
  size_t n_pdus = 0;
  auto cb = [&](auto /*kConnectionHandle*/, auto& buffer) {
    ++n_pdus;
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x23), buffer));
  };

  fake_l2cap().RegisterHandler(l2cap::kSignalingChannelId, cb);
  StaticByteBuffer sample_packet(
      // L2CAP B-Frame header for signaling channel packet.
      // Length 0x0001
      0x01,
      0x00,
      // Channel Id: 0x0001
      LowerBits(l2cap::kSignalingChannelId),
      UpperBits(l2cap::kSignalingChannelId),
      // Payload: "#"
      0x23);
  fake_l2cap().HandlePdu(kConnectionHandle, sample_packet);
  EXPECT_EQ(1u, n_pdus);
}

TEST_F(FakeL2capTest, CallHandlerMultipleTimes) {
  size_t n_pdus = 0;
  auto cb = [&](auto /*kConnectionHandle*/, auto& buffer) {
    ++n_pdus;
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x23), buffer));
  };

  fake_l2cap().RegisterHandler(l2cap::kSignalingChannelId, cb);
  StaticByteBuffer sample_packet(
      // L2CAP B-Frame header for signaling channel packet.
      // Length 0x0001
      0x01,
      0x00,
      // Channel Id: 0x0001
      LowerBits(l2cap::kSignalingChannelId),
      UpperBits(l2cap::kSignalingChannelId),
      // Payload: "#"
      0x23);
  fake_l2cap().HandlePdu(kConnectionHandle, sample_packet);
  EXPECT_EQ(1u, n_pdus);
  fake_l2cap().HandlePdu(kConnectionHandle, sample_packet);
  EXPECT_EQ(2u, n_pdus);
}

TEST_F(FakeL2capTest, CustomUnexpectedPacketHandler) {
  size_t n_pdus = 0;
  auto unexpected_cb = [&](auto /*kConnectionHandle*/, auto& buffer) {
    ++n_pdus;
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x01, 0x00, 0x01, 0x00, 0x23),
                                buffer));
  };
  auto send_cb =
      [](auto /*kConnectionHandle*/, auto /*cid*/, auto& /*buffer*/) {};
  auto fake_l2cap_custom_handler =
      FakeL2cap(send_cb, unexpected_cb, l2cap::kLastACLDynamicChannelId);

  StaticByteBuffer sample_packet(
      // L2CAP B-Frame header for signaling channel packet.
      // Length 0x0001
      0x01,
      0x00,
      // Channel Id: 0x0001
      LowerBits(l2cap::kSignalingChannelId),
      UpperBits(l2cap::kSignalingChannelId),
      // Payload: "#"
      0x23);
  fake_l2cap_custom_handler.HandlePdu(kConnectionHandle, sample_packet);
  EXPECT_EQ(1u, n_pdus);
}

TEST_F(FakeL2capTest, DefaultUnexpectedPacketHandler) {
  size_t n_pdus = 0;
  auto cb = [&](auto /*kConnectionHandle*/, auto& /*buffer*/) { ++n_pdus; };
  fake_l2cap().RegisterHandler(l2cap::kConnectionlessChannelId, cb);
  StaticByteBuffer sample_packet = StaticByteBuffer(
      // L2CAP B-Frame header for signaling channel packet.
      // Length 0x0001
      0x01,
      0x00,
      // Channel Id: 0x0001
      LowerBits(l2cap::kSignalingChannelId),
      UpperBits(l2cap::kSignalingChannelId),
      // Payload: "#"
      0x23);
  fake_l2cap().HandlePdu(kConnectionHandle, sample_packet);

  // As the ChannelIds of the registered handler and the received packet are
  // different, cb should not be called and n_pdus should still be 0 as the
  // default packet handler ignores unroutable packets.
  EXPECT_EQ(0u, n_pdus);
}

TEST_F(FakeL2capTest, DefaultSendPacketOnCustomChannel) {
  std::unique_ptr<ByteBuffer> received_packet;
  auto send_cb = [&received_packet](
                     auto /*kConnectionHandle*/, auto /*cid*/, auto& buffer) {
    received_packet = std::make_unique<DynamicByteBuffer>(buffer);
  };
  set_send_frame_callback(send_cb);

  // Configure the FakeService to use FakeL2cap's SendFrameCallback.
  auto channel_cb = [](FakeDynamicChannel::WeakPtr channel) {
    auto handle_sdu = [channel](auto& request) {
      if (channel.is_alive()) {
        auto& callback = channel->send_packet_callback();
        return callback(std::move(request));
      }
    };
    channel->set_packet_handler_callback(handle_sdu);
  };
  fake_l2cap().RegisterService(l2cap::kSDP, channel_cb);
  l2cap::ChannelId src_id = l2cap::kFirstDynamicChannelId;

  // Open up the SDP channel.
  fake_l2cap().RegisterDynamicChannel(
      kConnectionHandle, l2cap::kSDP, src_id, src_id);
  auto channel =
      fake_l2cap().FindDynamicChannelByLocalId(kConnectionHandle, src_id);
  channel->set_opened();
  fake_l2cap().RegisterDynamicChannelWithPsm(kConnectionHandle, src_id);

  // Expect that the custom channel only sends back the payload
  StaticByteBuffer sample_packet(
      // L2CAP B-Frame header for signaling channel packet.
      // Length 0x0001
      0x01,
      0x00,
      // Channel Id: 0x0040
      LowerBits(src_id),
      UpperBits(src_id),
      // Payload: "#"
      0x23);
  fake_l2cap().HandlePdu(kConnectionHandle, sample_packet);
  auto response_payload = DynamicByteBuffer(StaticByteBuffer(0x23));
  EXPECT_TRUE(ContainersEqual(response_payload, *received_packet));
}

}  // namespace bt::testing
