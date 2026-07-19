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

#include "pw_bluetooth_sapphire/internal/host/l2cap/signaling_channel.h"

#include <pw_assert/check.h>
#include <pw_async/dispatcher.h>

#include <chrono>

#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::l2cap::internal {
namespace {

using Status = SignalingChannelInterface::Status;

constexpr CommandCode kUnknownCommandCode = 0x00;
constexpr CommandCode kCommandCode = 0xFF;
constexpr hci_spec::ConnectionHandle kTestHandle = 0x0001;
constexpr uint16_t kTestMTU = 100;
constexpr CommandId kMaxCommandId = std::numeric_limits<CommandId>::max();

const auto kTestResponseHandler = [](Status, const ByteBuffer&) {
  return SignalingChannel::ResponseHandlerAction::kCompleteOutboundTransaction;
};

class TestSignalingChannel : public SignalingChannel {
 public:
  explicit TestSignalingChannel(
      Channel::WeakPtr chan,
      pw::async::Dispatcher& dispatcher,
      pw::bluetooth_sapphire::LeaseProvider& lease_provider)
      : SignalingChannel(std::move(chan),
                         pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                         dispatcher,
                         lease_provider) {
    set_mtu(kTestMTU);
  }
  ~TestSignalingChannel() override = default;

  using PacketCallback = fit::function<void(const SignalingPacket& packet)>;
  void set_packet_callback(PacketCallback cb) { packet_cb_ = std::move(cb); }

  // Expose GetNextCommandId() as public so it can be called by tests below.
  using SignalingChannel::GetNextCommandId;

  // Expose ResponderImpl as public so it can be directly tested (rather than
  // passed to RequestDelegate).
  using SignalingChannel::ResponderImpl;

 private:
  // SignalingChannel overrides
  void DecodeRxUnit(ByteBufferPtr sdu,
                    const SignalingPacketHandler& cb) override {
    PW_CHECK(sdu);
    if (sdu->size()) {
      cb(SignalingPacket(sdu.get(), sdu->size() - sizeof(CommandHeader)));
    } else {
      // Silently drop the packet. See documentation in signaling_channel.h.
    }
  }

  bool IsSupportedResponse(CommandCode code) const override {
    switch (code) {
      case kCommandRejectCode:
      case kEchoResponse:
      case kLEFlowControlCredit:
        return true;
    }

    return false;
  }

  bool HandlePacket(const SignalingPacket& packet) override {
    if (packet_cb_)
      packet_cb_(packet);

    return SignalingChannel::HandlePacket(packet);
  }

  PacketCallback packet_cb_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(TestSignalingChannel);
};

class SignalingChannelTest : public testing::FakeChannelTest {
 public:
  SignalingChannelTest() = default;
  ~SignalingChannelTest() override = default;

 protected:
  void SetUp() override {
    ChannelOptions options(kLESignalingChannelId);
    options.conn_handle = kTestHandle;

    fake_channel_inst_ = CreateFakeChannel(options);
    sig_ = std::make_unique<TestSignalingChannel>(
        fake_channel_inst_->GetWeakPtr(), dispatcher(), lease_provider_);
  }

  void TearDown() override {
    // Unless a test called DestroySig(), the signaling channel will outlive the
    // underlying channel.
    fake_channel_inst_ = nullptr;
    DestroySig();
  }

  TestSignalingChannel* sig() const { return sig_.get(); }

  void DestroySig() { sig_ = nullptr; }

  pw::bluetooth_sapphire::testing::FakeLeaseProvider& lease_provider() {
    return lease_provider_;
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<TestSignalingChannel> sig_;

  // Own the fake channel so that its lifetime can span beyond that of |sig_|.
  std::unique_ptr<Channel> fake_channel_inst_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SignalingChannelTest);
};

TEST_F(SignalingChannelTest, IgnoreEmptyFrame) {
  bool send_cb_called = false;
  auto send_cb = [&send_cb_called](auto) { send_cb_called = true; };

  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());
  fake_chan()->Receive(BufferView());

  RunUntilIdle();
  EXPECT_FALSE(send_cb_called);
}

TEST_F(SignalingChannelTest, Reject) {
  constexpr uint8_t kTestId = 14;

  // Command Reject packet.
  StaticByteBuffer expected(
      // Command header
      0x01,
      kTestId,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  // A command that TestSignalingChannel does not support.
  StaticByteBuffer cmd(
      // header
      kUnknownCommandCode,
      kTestId,
      0x04,
      0x00,

      // data
      'L',
      'O',
      'L',
      'Z');

  EXPECT_TRUE(ReceiveAndExpect(cmd, expected));
}

TEST_F(SignalingChannelTest, RejectCommandCodeZero) {
  constexpr uint8_t kTestId = 14;

  // Command Reject packet.
  StaticByteBuffer expected(
      // Command header
      0x01,
      kTestId,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  // A command that TestSignalingChannel does not support.
  StaticByteBuffer cmd(
      // header
      0x00,
      kTestId,
      0x04,
      0x00,

      // data
      'L',
      'O',
      'L',
      'Z');

  EXPECT_TRUE(ReceiveAndExpect(cmd, expected));
}

TEST_F(SignalingChannelTest, RejectNotUnderstoodWithResponder) {
  constexpr uint8_t kTestId = 14;

  StaticByteBuffer expected(
      // Command header (Command Reject, ID, length)
      0x01,
      kTestId,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  bool cb_called = false;
  auto send_cb = [&expected, &cb_called](auto packet) {
    cb_called = true;
    EXPECT_TRUE(ContainersEqual(expected, *packet));
  };
  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());

  TestSignalingChannel::ResponderImpl responder(sig(), kCommandCode, kTestId);
  responder.RejectNotUnderstood();

  RunUntilIdle();
  EXPECT_TRUE(cb_called);
}

TEST_F(SignalingChannelTest, RejectInvalidCIdWithResponder) {
  constexpr uint8_t kTestId = 14;
  constexpr uint16_t kLocalCId = 0xf00d;
  constexpr uint16_t kRemoteCId = 0xcafe;

  StaticByteBuffer expected(
      // Command header (Command Reject, ID, length)
      0x01,
      kTestId,
      0x06,
      0x00,

      // Reason (Invalid channel ID)
      0x02,
      0x00,

      // Data (Channel IDs),
      LowerBits(kLocalCId),
      UpperBits(kLocalCId),
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId));

  bool cb_called = false;
  auto send_cb = [&expected, &cb_called](auto packet) {
    cb_called = true;
    EXPECT_TRUE(ContainersEqual(expected, *packet));
  };
  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());

  TestSignalingChannel::ResponderImpl responder(sig(), kCommandCode, kTestId);
  responder.RejectInvalidChannelId(kLocalCId, kRemoteCId);

  RunUntilIdle();
  EXPECT_TRUE(cb_called);
}

TEST_F(SignalingChannelTest, InvalidMTU) {
  constexpr uint8_t kTestId = 14;
  constexpr uint16_t kTooSmallMTU = 7;

  // Command Reject packet.
  StaticByteBuffer expected(
      // Command header
      0x01,
      kTestId,
      0x04,
      0x00,

      // Reason (Signaling MTU exceeded)
      0x01,
      0x00,

      // The supported MTU
      static_cast<uint8_t>(kTooSmallMTU),
      0x00);

  // A command that is one octet larger than the MTU.
  StaticByteBuffer cmd(
      // header
      kCommandCode,
      kTestId,
      0x04,
      0x00,

      // data
      'L',
      'O',
      'L',
      'Z');

  sig()->set_mtu(kTooSmallMTU);
  EXPECT_TRUE(ReceiveAndExpect(cmd, expected));
}

TEST_F(SignalingChannelTest, HandlePacket) {
  constexpr uint8_t kTestId = 14;

  // A command that TestSignalingChannel supports.
  StaticByteBuffer cmd(
      // header
      kCommandCode,
      kTestId,
      0x04,
      0x00,

      // data
      'L',
      'O',
      'L',
      'Z');

  bool called = false;
  sig()->set_packet_callback([&cmd, &called](auto packet) {
    EXPECT_TRUE(ContainersEqual(cmd, packet.data()));
    called = true;
  });

  fake_chan()->Receive(cmd);

  RunUntilIdle();
  EXPECT_TRUE(called);
}

TEST_F(SignalingChannelTest, UseChannelAfterSignalFree) {
  // Destroy the underlying channel's user (SignalingChannel).
  DestroySig();

  // Ensure that the underlying channel is still alive.
  ASSERT_TRUE(fake_chan().is_alive());

  // SignalingChannel is expected to deactivate the channel if it doesn't own
  // it. Either way, the channel isn't in a state that can receive test data.
  EXPECT_FALSE(fake_chan()->activated());

  // Ensure that closing the channel (possibly firing callback) is OK.
  fake_chan()->Close();

  RunUntilIdle();
}

TEST_F(SignalingChannelTest, ValidRequestCommandIds) {
  EXPECT_EQ(0x01, sig()->GetNextCommandId());
  for (int i = 0; i < kMaxCommandId + 1; i++) {
    EXPECT_NE(0x00, sig()->GetNextCommandId());
  }
}

TEST_F(SignalingChannelTest, DoNotRejectUnsolicitedResponse) {
  constexpr CommandId kTestCmdId = 97;
  StaticByteBuffer cmd(
      // Command header (Echo Response, length 1)
      0x09,
      kTestCmdId,
      0x01,
      0x00,

      // Payload
      0x23);

  size_t send_count = 0;
  auto send_cb = [&](auto) { send_count++; };
  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());

  fake_chan()->Receive(cmd);
  RunUntilIdle();
  EXPECT_EQ(0u, send_count);
}

TEST_F(SignalingChannelTest, RejectRemoteResponseWithWrongType) {
  constexpr CommandId kReqId = 1;

  // Remote's response with the correct ID but wrong type of response.
  const StaticByteBuffer rsp_invalid_id(
      // Disconnection Response with plausible 4-byte payload.
      0x07,
      kReqId,
      0x04,
      0x00,

      // Payload
      0x0A,
      0x00,
      0x08,
      0x00);
  const StaticByteBuffer req_data('P', 'W', 'N');

  bool tx_success = false;
  fake_chan()->SetSendCallback([&tx_success](auto) { tx_success = true; },
                               dispatcher());

  bool echo_cb_called = false;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest, req_data, [&echo_cb_called](auto, auto&) {
        echo_cb_called = true;
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  RunUntilIdle();
  EXPECT_TRUE(tx_success);

  const StaticByteBuffer reject_rsp(
      // Command header (Command Rejected)
      0x01,
      kReqId,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);
  bool reject_sent = false;
  fake_chan()->SetSendCallback(
      [&reject_rsp, &reject_sent](auto cb_packet) {
        reject_sent = ContainersEqual(reject_rsp, *cb_packet);
      },
      dispatcher());

  fake_chan()->Receive(rsp_invalid_id);

  RunUntilIdle();
  EXPECT_FALSE(echo_cb_called);
  EXPECT_TRUE(reject_sent);
}

// Ensure that the signaling channel can reuse outgoing command IDs. In the case
// that it's expecting a response on every single valid command ID, requests
// should fail.
TEST_F(SignalingChannelTest, ReuseCommandIdsUntilExhausted) {
  int req_count = 0;
  constexpr CommandId kRspId = 0x0c;

  auto check_header_id = [&req_count, kRspId](auto cb_packet) {
    req_count++;
    SignalingPacket sent_sig_pkt(cb_packet.get());
    if (req_count == kMaxCommandId + 1) {
      EXPECT_EQ(kRspId, sent_sig_pkt.header().id);
    } else {
      EXPECT_EQ(req_count, sent_sig_pkt.header().id);
    }
  };
  fake_chan()->SetSendCallback(std::move(check_header_id), dispatcher());

  const StaticByteBuffer req_data('y', 'o', 'o', 'o', 'o', '\0');
  const StaticByteBuffer empty_credit(0, 0, 0, 0);

  for (int i = 0; i < kMaxCommandId; i++) {
    EXPECT_TRUE(
        sig()->SendRequest(kEchoRequest, req_data, kTestResponseHandler));
  }

  // All command IDs should be exhausted at this point, so no commands of any
  // type should be allowed to be sent.
  EXPECT_FALSE(
      sig()->SendRequest(kEchoRequest, req_data, kTestResponseHandler));
  EXPECT_FALSE(
      sig()->SendCommandWithoutResponse(kLEFlowControlCredit, empty_credit));

  RunUntilIdle();
  EXPECT_EQ(kMaxCommandId, req_count);

  // Remote finally responds to a request, but not in order requests were sent.
  // This will free a command ID.
  const StaticByteBuffer echo_rsp(
      // Echo response with no payload.
      0x09,
      kRspId,
      0x00,
      0x00);
  fake_chan()->Receive(echo_rsp);

  RunUntilIdle();

  // Request should use freed command ID.
  EXPECT_TRUE(sig()->SendRequest(kEchoRequest, req_data, kTestResponseHandler));

  RunUntilIdle();
  EXPECT_EQ(kMaxCommandId + 1, req_count);
}

// Ensure that a response handler may destroy the signaling channel.
TEST_F(SignalingChannelTest, ResponseHandlerThatDestroysSigDoesNotCrash) {
  fake_chan()->SetSendCallback([](auto) {}, dispatcher());

  const StaticByteBuffer req_data('h', 'e', 'l', 'l', 'o');
  bool rx_success = false;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest, req_data, [this, &rx_success](Status, const ByteBuffer&) {
        rx_success = true;
        DestroySig();
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  constexpr CommandId kReqId = 1;
  const StaticByteBuffer echo_rsp(
      // Command header (Echo Response, length 1)
      kEchoResponse,
      kReqId,
      0x01,
      0x00,

      // Payload
      0x23);
  fake_chan()->Receive(echo_rsp);

  RunUntilIdle();
  EXPECT_FALSE(sig());
  EXPECT_TRUE(rx_success);
}

// Ensure that the signaling channel plumbs a rejection command from remote to
// the appropriate response handler.
TEST_F(SignalingChannelTest, RemoteRejectionPassedToHandler) {
  const StaticByteBuffer reject_rsp(
      // Command header (Command Rejected)
      0x01,
      0x01,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  bool tx_success = false;
  fake_chan()->SetSendCallback([&tx_success](auto) { tx_success = true; },
                               dispatcher());

  const StaticByteBuffer req_data('h', 'e', 'l', 'l', 'o');
  bool rx_success = false;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest,
      req_data,
      [&rx_success, &reject_rsp](Status status, const ByteBuffer& rsp_payload) {
        rx_success = true;
        EXPECT_EQ(Status::kReject, status);
        EXPECT_TRUE(ContainersEqual(reject_rsp.view(sizeof(CommandHeader)),
                                    rsp_payload));
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  RunUntilIdle();
  EXPECT_TRUE(tx_success);

  // Remote sends back a rejection.
  fake_chan()->Receive(reject_rsp);

  RunUntilIdle();
  EXPECT_TRUE(rx_success);
}

TEST_F(SignalingChannelTest,
       HandlerCompletedByResponseNotCalledAgainAfterRtxTimeout) {
  bool tx_success = false;
  fake_chan()->SetSendCallback([&tx_success](auto) { tx_success = true; },
                               dispatcher());

  const StaticByteBuffer req_data('h', 'e', 'l', 'l', 'o');
  int rx_cb_count = 0;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest, req_data, [&rx_cb_count](Status status, const ByteBuffer&) {
        rx_cb_count++;
        EXPECT_EQ(Status::kSuccess, status);
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  const StaticByteBuffer echo_rsp(
      // Echo response with no payload.
      0x09,
      0x01,
      0x00,
      0x00);
  fake_chan()->Receive(echo_rsp);

  RunUntilIdle();
  EXPECT_TRUE(tx_success);
  EXPECT_EQ(1, rx_cb_count);

  RunFor(kSignalingChannelResponseTimeout);
  EXPECT_EQ(1, rx_cb_count);
}

// Ensure that the signaling channel calls ResponseHandler with Status::kTimeOut
// after a request times out waiting for a peer response.
TEST_F(SignalingChannelTest,
       CallHandlerCalledAfterMaxNumberOfRtxTimeoutRetransmissions) {
  size_t send_cb_count = 0;
  auto send_cb = [&](auto cb_packet) {
    SignalingPacket pkt(cb_packet.get());
    EXPECT_EQ(pkt.header().id, 1u);
    send_cb_count++;
  };
  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());

  const StaticByteBuffer req_data('h', 'e', 'l', 'l', 'o');
  bool rx_cb_called = false;
  EXPECT_TRUE(
      sig()->SendRequest(kEchoRequest,
                         req_data,
                         [&rx_cb_called](Status status, const ByteBuffer&) {
                           rx_cb_called = true;
                           EXPECT_EQ(Status::kTimeOut, status);
                           return SignalingChannel::ResponseHandlerAction::
                               kCompleteOutboundTransaction;
                         }));

  RunUntilIdle();
  EXPECT_EQ(send_cb_count, 1u);
  EXPECT_FALSE(rx_cb_called);

  auto timeout = kSignalingChannelResponseTimeout;
  for (size_t i = 1; i < kMaxSignalingChannelTransmissions; i++) {
    // Ensure retransmission doesn't happen before timeout.
    RunFor(timeout - std::chrono::milliseconds(100));
    EXPECT_EQ(send_cb_count, i);

    RunFor(std::chrono::milliseconds(100));
    EXPECT_EQ(send_cb_count, 1 + i);
    EXPECT_FALSE(rx_cb_called);

    timeout *= 2;
  }

  send_cb_count = 0;
  RunFor(timeout);
  EXPECT_EQ(send_cb_count, 0u);
  EXPECT_TRUE(rx_cb_called);
}

TEST_F(SignalingChannelTest, TwoResponsesToARetransmittedOutboundRequest) {
  size_t send_cb_count = 0;
  auto send_cb = [&](auto cb_packet) {
    SignalingPacket pkt(cb_packet.get());
    EXPECT_EQ(pkt.header().id, 1u);
    send_cb_count++;
  };
  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());

  const StaticByteBuffer req_data('h', 'e', 'l', 'l', 'o');
  size_t rx_cb_count = 0;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest, req_data, [&rx_cb_count](Status status, const ByteBuffer&) {
        rx_cb_count++;
        EXPECT_EQ(Status::kSuccess, status);
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  RunUntilIdle();
  EXPECT_EQ(1u, send_cb_count);
  EXPECT_EQ(0u, rx_cb_count);

  RunFor(kSignalingChannelResponseTimeout);
  EXPECT_EQ(2u, send_cb_count);
  EXPECT_EQ(0u, rx_cb_count);

  const StaticByteBuffer echo_rsp(kEchoResponse, 0x01, 0x00, 0x00);
  fake_chan()->Receive(echo_rsp);
  EXPECT_EQ(2u, send_cb_count);
  EXPECT_EQ(1u, rx_cb_count);

  // Second response should be ignored as it is unexpected.
  fake_chan()->Receive(echo_rsp);
  EXPECT_EQ(2u, send_cb_count);
  EXPECT_EQ(1u, rx_cb_count);
}

// When the response handler expects more responses, use the longer ERTX timeout
// for the following response.
TEST_F(SignalingChannelTest,
       ExpectAdditionalResponseExtendsRtxTimeoutToErtxTimeout) {
  EXPECT_EQ(lease_provider().lease_count(), 0u);
  bool tx_success = false;
  fake_chan()->SetSendCallback([&tx_success](auto) { tx_success = true; },
                               dispatcher());

  const StaticByteBuffer req_data{'h', 'e', 'l', 'l', 'o'};
  int rx_cb_calls = 0;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest,
      req_data,
      [this, &rx_cb_calls](Status status, const ByteBuffer&) {
        rx_cb_calls++;
        EXPECT_GT(lease_provider().lease_count(), 0u);
        if (rx_cb_calls <= 2) {
          EXPECT_EQ(Status::kSuccess, status);
        } else {
          EXPECT_EQ(Status::kTimeOut, status);
        }
        return SignalingChannel::ResponseHandlerAction::
            kExpectAdditionalResponse;
      }));

  EXPECT_GT(lease_provider().lease_count(), 0u);
  RunUntilIdle();
  EXPECT_TRUE(tx_success);
  EXPECT_EQ(0, rx_cb_calls);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  const StaticByteBuffer echo_rsp(
      // Echo response with no payload.
      0x09,
      0x01,
      0x00,
      0x00);
  fake_chan()->Receive(echo_rsp);
  EXPECT_EQ(1, rx_cb_calls);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  // The handler expects more responses so the RTX timer shouldn't have expired.
  RunFor(kSignalingChannelResponseTimeout);

  fake_chan()->Receive(echo_rsp);
  EXPECT_EQ(2, rx_cb_calls);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  // The second response should have reset the ERTX timer, so it shouldn't fire
  // yet.
  RunFor(kSignalingChannelExtendedResponseTimeout -
         std::chrono::milliseconds(100));

  // If the renewed ERTX timer expires without a third response, receive a
  // kTimeOut "response."
  RunFor(std::chrono::seconds(1));
  EXPECT_EQ(3, rx_cb_calls);
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(SignalingChannelTest, RegisterRequestResponder) {
  const StaticByteBuffer remote_req(
      // Disconnection Request.
      0x06,
      0x01,
      0x04,
      0x00,

      // Payload
      0x0A,
      0x00,
      0x08,
      0x00);
  const BufferView& expected_payload = remote_req.view(sizeof(CommandHeader));

  auto expected_rej = StaticByteBuffer(
      // Command header (Command rejected, length 2)
      0x01,
      0x01,
      0x02,
      0x00,

      // Reason (Command not understood)
      0x00,
      0x00);

  // Receive remote's request before a handler is assigned, expecting an
  // outbound rejection.
  ReceiveAndExpect(remote_req, expected_rej);

  // Register the handler.
  bool cb_called = false;
  sig()->ServeRequest(
      kDisconnectionRequest,
      [&cb_called, &expected_payload](const ByteBuffer& req_payload,
                                      SignalingChannel::Responder* responder) {
        cb_called = true;
        EXPECT_TRUE(ContainersEqual(expected_payload, req_payload));
        responder->Send(req_payload);
      });

  const ByteBuffer& local_rsp = StaticByteBuffer(
      // Disconnection Response.
      0x07,
      0x01,
      0x04,
      0x00,

      // Payload
      0x0A,
      0x00,
      0x08,
      0x00);

  // Receive the same command again.
  ReceiveAndExpect(remote_req, local_rsp);
  EXPECT_TRUE(cb_called);
}

TEST_F(SignalingChannelTest, DoNotRejectRemoteResponseInvalidId) {
  // Request will use ID = 1.
  constexpr CommandId kIncorrectId = 2;
  // Remote's echo response that has a different ID to what will be in the
  // request header.
  const StaticByteBuffer rsp_invalid_id(
      // Echo response with 4-byte payload.
      0x09,
      kIncorrectId,
      0x04,
      0x00,

      // Payload
      'L',
      '3',
      '3',
      'T');
  const BufferView req_data = rsp_invalid_id.view(sizeof(CommandHeader));

  bool tx_success = false;
  fake_chan()->SetSendCallback([&tx_success](auto) { tx_success = true; },
                               dispatcher());

  bool echo_cb_called = false;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest, req_data, [&echo_cb_called](auto, auto&) {
        echo_cb_called = true;
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  RunUntilIdle();
  EXPECT_TRUE(tx_success);

  bool reject_sent = false;
  fake_chan()->SetSendCallback([&reject_sent](auto) { reject_sent = true; },
                               dispatcher());

  fake_chan()->Receive(rsp_invalid_id);

  RunUntilIdle();
  EXPECT_FALSE(echo_cb_called);
  EXPECT_FALSE(reject_sent);
}

TEST_F(SignalingChannelTest, SendWithoutResponse) {
  StaticByteBuffer expected(
      // Command header (Command code, ID, length)
      kLEFlowControlCredit,
      1,
      0x04,
      0x00,

      // Channel ID
      0x12,
      0x34,

      // Credits
      0x01,
      0x42);

  StaticByteBuffer payload(
      // Channel ID
      0x12,
      0x34,

      // Credits
      0x01,
      0x42);

  bool cb_called = false;
  auto send_cb = [&expected, &cb_called](auto packet) {
    cb_called = true;
    EXPECT_TRUE(ContainersEqual(expected, *packet));
  };
  fake_chan()->SetSendCallback(std::move(send_cb), dispatcher());

  sig()->SendCommandWithoutResponse(kLEFlowControlCredit, payload);
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  RunUntilIdle();
  EXPECT_TRUE(cb_called);
}

TEST_F(SignalingChannelTest, SendMultipleCommandsSimultaneously) {
  EXPECT_EQ(lease_provider().lease_count(), 0u);
  fake_chan()->SetSendCallback([](auto) {}, dispatcher());

  const StaticByteBuffer req_data{'h', 'e', 'l', 'l', 'o'};
  int rx_cb_calls_0 = 0;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest,
      req_data,
      [this, &rx_cb_calls_0](Status status, const ByteBuffer&) {
        rx_cb_calls_0++;
        EXPECT_EQ(Status::kSuccess, status);
        EXPECT_GT(lease_provider().lease_count(), 0u);
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  EXPECT_GT(lease_provider().lease_count(), 0u);
  RunUntilIdle();
  EXPECT_EQ(0, rx_cb_calls_0);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  int rx_cb_calls_1 = 0;
  EXPECT_TRUE(sig()->SendRequest(
      kEchoRequest,
      req_data,
      [this, &rx_cb_calls_1](Status status, const ByteBuffer&) {
        rx_cb_calls_1++;
        EXPECT_EQ(Status::kSuccess, status);
        EXPECT_GT(lease_provider().lease_count(), 0u);
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  EXPECT_GT(lease_provider().lease_count(), 0u);
  RunUntilIdle();
  EXPECT_EQ(0, rx_cb_calls_1);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  const StaticByteBuffer echo_rsp_0(
      // Echo response with no payload.
      0x09,
      0x01,  // ID (1)
      0x00,
      0x00);
  fake_chan()->Receive(echo_rsp_0);
  EXPECT_EQ(1, rx_cb_calls_0);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  const StaticByteBuffer echo_rsp_1(
      // Echo response with no payload.
      0x09,
      0x02,  // ID (2)
      0x00,
      0x00);
  fake_chan()->Receive(echo_rsp_1);
  EXPECT_EQ(1, rx_cb_calls_1);
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

// Tests that repeated Extended Response Timeout (ERTX) resets are capped so
// that the total transaction timeout does not exceed 300 seconds and the wake
// lease is released.
TEST_F(SignalingChannelTest,
       ErtxResetsAreCappedTo300SecondsAndReleaseWakeLease) {
  ASSERT_EQ(lease_provider().lease_count(), 0u);

  fake_chan()->SetSendCallback([](auto) {}, dispatcher());

  int success_responses = 0;
  int timeout_responses = 0;
  ASSERT_TRUE(sig()->SendRequest(
      kEchoRequest, BufferView(), [&](Status status, const ByteBuffer&) {
        if (status == Status::kSuccess) {
          success_responses++;
          return SignalingChannel::ResponseHandlerAction::
              kExpectAdditionalResponse;
        }
        timeout_responses++;
        return SignalingChannel::ResponseHandlerAction::
            kCompleteOutboundTransaction;
      }));

  RunUntilIdle();
  // wake_lease acquired in EnqueueResponse() and held by the PendingCommand.
  ASSERT_GT(lease_provider().lease_count(), 0u);

  // Peer-crafted L2CAP_ECHO_RSP.
  // CommandId = 0x01 (first id allocated by GetNextCommandId).
  const StaticByteBuffer kEchoResponsePacket(
      kEchoResponse,  // code = 0x09
      0x01,           // id   = 0x01 (read by attacker off the wire)
      0x00,
      0x00);  // length = 0

  // Attacker sends one response per <60 s window. Do this for
  // 50 iterations * 59 s ≈ 49 minutes of simulated time — far past the
  // 300 s spec cap.
  constexpr int kIterations = 50;
  for (int i = 0; i < kIterations; i++) {
    fake_chan()->Receive(kEchoResponsePacket);
    RunFor(std::chrono::seconds(59));
  }

  // We expect the transaction to time out before all iterations complete right
  // at the 300 s spec cap.
  EXPECT_LT(success_responses, kIterations);
  EXPECT_EQ(timeout_responses, 1)
      << "Regression: ERTX did not time out right at the 300 s spec cap.";
  EXPECT_EQ(lease_provider().lease_count(), 0u)
      << "Regression: wake_lease still held after ERTX 300 s cap expired.";
}

}  // namespace
}  // namespace bt::l2cap::internal
