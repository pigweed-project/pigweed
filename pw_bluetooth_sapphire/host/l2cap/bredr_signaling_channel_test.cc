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

#include "pw_bluetooth_sapphire/internal/host/l2cap/bredr_signaling_channel.h"

#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/mock_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::l2cap::internal {
namespace {

constexpr hci_spec::ConnectionHandle kTestHandle = 0x0001;
constexpr uint8_t kTestCmdId = 97;
constexpr pw::bluetooth::emboss::ConnectionRole kDeviceRole =
    pw::bluetooth::emboss::ConnectionRole::CENTRAL;

class BrEdrSignalingChannelTest : public testing::MockChannelTest {
 public:
  BrEdrSignalingChannelTest() = default;
  ~BrEdrSignalingChannelTest() override = default;

  BrEdrSignalingChannelTest(const BrEdrSignalingChannelTest&) = delete;
  BrEdrSignalingChannelTest& operator=(const BrEdrSignalingChannelTest&) =
      delete;

 protected:
  void SetUp() override {
    ChannelOptions options(kSignalingChannelId);
    options.conn_handle = kTestHandle;

    fake_chan_ = CreateFakeChannel(options);
    sig_ = std::make_unique<BrEdrSignalingChannel>(
        fake_chan_->GetWeakPtr(), kDeviceRole, dispatcher(), lease_provider_);
  }

  void TearDown() override {
    RunUntilIdle();
    sig_ = nullptr;
  }

  BrEdrSignalingChannel* sig() const { return sig_.get(); }

  void DestroySig() { sig_ = nullptr; }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  testing::FakeChannel::WeakPtr fake_chan_;
  std::unique_ptr<BrEdrSignalingChannel> sig_;
};

TEST_F(BrEdrSignalingChannelTest, RespondsToEchoRequest) {
  const StaticByteBuffer cmd(
      // Command header (Echo Request, length 1)
      0x08,
      kTestCmdId,
      0x01,
      0x00,

      // Payload
      0x23);

  const StaticByteBuffer response(
      // Command header (Echo Response, length 1)
      0x09,
      kTestCmdId,
      0x01,
      0x00,
      // Payload
      0x23);

  EXPECT_PACKET_OUT(response);
  fake_chan()->Receive(cmd);
}

TEST_F(BrEdrSignalingChannelTest, IgnoreEmptyFrame) {
  fake_chan()->Receive(BufferView());
}

TEST_F(BrEdrSignalingChannelTest, RejectMalformedAdditionalCommand) {
  constexpr uint8_t kTestId0 = 14;
  constexpr uint8_t kTestId1 = 15;

  // Echo Request (see other test for command support), followed by an
  // incomplete command packet
  StaticByteBuffer cmd(
      // Command header (length 3)
      0x08,
      kTestId0,
      0x03,
      0x00,

      // Payload data
      'L',
      'O',
      'L',

      // Second command header
      0x08,
      kTestId1,
      0x01,
      0x00);

  // Echo Response packet
  StaticByteBuffer rsp0(
      // Command header (length 3)
      0x09,
      kTestId0,
      0x03,
      0x00,

      // Payload data
      'L',
      'O',
      'L');

  // Command Reject packet
  StaticByteBuffer rsp1(
      // Command header
      0x01,
      kTestId1,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  EXPECT_PACKET_OUT(rsp0);
  EXPECT_PACKET_OUT(rsp1);
  fake_chan()->Receive(cmd);
}

TEST_F(BrEdrSignalingChannelTest, HandleMultipleCommands) {
  constexpr uint8_t kTestId0 = 14;
  constexpr uint8_t kTestId1 = 15;
  constexpr uint8_t kTestId2 = 16;

  StaticByteBuffer cmd(
      // Command header (Echo Request)
      0x08,
      kTestId0,
      0x04,
      0x00,

      // Payload data
      'L',
      'O',
      'L',
      'Z',

      // Header with command to be rejected
      0xFF,
      kTestId1,
      0x03,
      0x00,

      // Payload data
      'L',
      'O',
      'L',

      // Command header (Echo Request, no payload data)
      0x08,
      kTestId2,
      0x00,
      0x00,

      // Additional command fragment to be dropped
      0xFF,
      0x00);

  StaticByteBuffer echo_rsp0(
      // Command header (Echo Response)
      0x09,
      kTestId0,
      0x04,
      0x00,

      // Payload data
      'L',
      'O',
      'L',
      'Z');

  StaticByteBuffer reject_rsp1(
      // Command header (Command Rejected)
      0x01,
      kTestId1,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  StaticByteBuffer echo_rsp2(
      // Command header (Echo Response)
      0x09,
      kTestId2,
      0x00,
      0x00);

  EXPECT_PACKET_OUT(echo_rsp0);
  EXPECT_PACKET_OUT(reject_rsp1);
  EXPECT_PACKET_OUT(echo_rsp2);
  fake_chan()->Receive(cmd);
}

TEST_F(BrEdrSignalingChannelTest, SendAndReceiveEcho) {
  const StaticByteBuffer expected_req(
      // Echo request with 3-byte payload.
      0x08,
      0x01,
      0x03,
      0x00,

      // Payload
      'P',
      'W',
      'N');
  const BufferView req_data = expected_req.view(4, 3);

  const StaticByteBuffer expected_rsp(
      // Echo response with 4-byte payload.
      0x09,
      0x01,
      0x04,
      0x00,

      // Payload
      'L',
      '3',
      '3',
      'T');
  const BufferView rsp_data = expected_rsp.view(4, 4);

  EXPECT_PACKET_OUT(expected_req);
  bool rx_success = false;
  EXPECT_TRUE(
      sig()->TestLink(req_data, [&rx_success, &rsp_data](const auto& data) {
        rx_success = ContainersEqual(rsp_data, data);
      }));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Remote sends back an echo response with a different payload than in local
  // request (this is allowed).
  fake_chan()->Receive(expected_rsp);
  EXPECT_TRUE(rx_success);
}

TEST_F(BrEdrSignalingChannelTest, MultipleCommandsFirstDestroysChannel) {
  constexpr uint8_t kTestId0 = 14;
  constexpr uint8_t kTestId1 = 15;

  StaticByteBuffer cmd(
      // Command header (Echo Request)
      0x08,
      kTestId0,
      0x04,
      0x00,

      // Payload data
      'L',
      'O',
      'L',
      'Z',

      // Command header (Echo Request)
      0x08,
      kTestId1,
      0x00,
      0x00);

  sig()->ServeRequest(kEchoRequest,
                      [this](const ByteBuffer&, SignalingChannel::Responder*) {
                        // Synchronously destroy signaling channel
                        DestroySig();
                      });

  // We shouldn't crash when we receive the packet.
  fake_chan()->Receive(cmd);
  RunUntilIdle();
  EXPECT_EQ(sig(), nullptr);
}

}  // namespace
}  // namespace bt::l2cap::internal
