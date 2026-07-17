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

#include "pw_bluetooth_sapphire/internal/host/l2cap/command_handler.h"

#include <pw_assert/check.h>
#include <pw_async/fake_dispatcher_fixture.h>

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::l2cap::internal {
namespace {

constexpr ChannelId kLocalCId = 0x0040;
constexpr ChannelId kRemoteCId = 0x60a3;

struct TestPayload {
  uint8_t value;
};

class TestCommandHandler final : public CommandHandler {
 public:
  // Inherit ctor
  using CommandHandler::CommandHandler;

  // A response that decoding always fails for.
  class UndecodableResponse final : public CommandHandler::Response {
   public:
    using PayloadT = TestPayload;
    static constexpr const char* kName = "Undecodable Response";

    using Response::Response;  // Inherit ctor
    bool Decode(const ByteBuffer&) { return false; }
  };

  using UndecodableResponseCallback =
      fit::function<void(const UndecodableResponse& rsp)>;

  bool SendRequestWithUndecodableResponse(CommandCode code,
                                          const ByteBuffer& payload,
                                          UndecodableResponseCallback cb) {
    auto on_rsp = BuildResponseHandler<UndecodableResponse>(std::move(cb));
    return sig()->SendRequest(code, payload, std::move(on_rsp));
  }
};

class CommandHandlerTest : public pw::async::test::FakeDispatcherFixture {
 public:
  CommandHandlerTest() = default;
  ~CommandHandlerTest() override = default;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(CommandHandlerTest);

 protected:
  // TestLoopFixture overrides
  void SetUp() override {
    signaling_channel_ =
        std::make_unique<testing::FakeSignalingChannel>(dispatcher());
    command_handler_ = std::make_unique<TestCommandHandler>(
        fake_sig(), fit::bind_member<&CommandHandlerTest::OnRequestFail>(this));
    request_fail_callback_ = nullptr;
    failed_requests_ = 0;
  }

  void TearDown() override {
    request_fail_callback_ = nullptr;
    signaling_channel_ = nullptr;
    command_handler_ = nullptr;
  }

  testing::FakeSignalingChannel* fake_sig() const {
    return signaling_channel_.get();
  }
  TestCommandHandler* cmd_handler() const { return command_handler_.get(); }
  size_t failed_requests() const { return failed_requests_; }

  void set_request_fail_callback(fit::closure request_fail_callback) {
    PW_CHECK(!request_fail_callback_);
    request_fail_callback_ = std::move(request_fail_callback);
  }

 private:
  void OnRequestFail() {
    failed_requests_++;
    if (request_fail_callback_) {
      request_fail_callback_();
    }
  }

  std::unique_ptr<testing::FakeSignalingChannel> signaling_channel_;
  std::unique_ptr<TestCommandHandler> command_handler_;
  fit::closure request_fail_callback_;
  size_t failed_requests_;
};

TEST_F(CommandHandlerTest, OutboundDisconReqRspOk) {
  // Disconnect Request payload
  StaticByteBuffer expected_discon_req(
      // Destination CID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Source CID
      LowerBits(kLocalCId),
      UpperBits(kLocalCId));

  // Disconnect Response payload
  // Channel endpoint roles (source, destination) are relative to requester so
  // the response's payload should be the same as the request's
  const ByteBuffer& ok_discon_rsp = expected_discon_req;

  EXPECT_OUTBOUND_REQ(
      *fake_sig(),
      kDisconnectionRequest,
      expected_discon_req.view(),
      {SignalingChannel::Status::kSuccess, ok_discon_rsp.view()});

  bool cb_called = false;
  CommandHandler::DisconnectionResponseCallback on_discon_rsp =
      [&cb_called](const CommandHandler::DisconnectionResponse& rsp) {
        cb_called = true;
        EXPECT_EQ(SignalingChannel::Status::kSuccess, rsp.status());
        EXPECT_EQ(kLocalCId, rsp.local_cid());
        EXPECT_EQ(kRemoteCId, rsp.remote_cid());
      };

  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kLocalCId, std::move(on_discon_rsp)));
  RunUntilIdle();
  EXPECT_TRUE(cb_called);
}

TEST_F(CommandHandlerTest, OutboundDisconReqRej) {
  // Disconnect Request payload
  StaticByteBuffer expected_discon_req(
      // Destination CID (relative to requester)
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Source CID (relative to requester)
      LowerBits(kLocalCId),
      UpperBits(kLocalCId));

  // Command Reject payload
  StaticByteBuffer rej_cid(
      // Reject Reason (invalid channel ID)
      LowerBits(static_cast<uint16_t>(RejectReason::kInvalidCID)),
      UpperBits(static_cast<uint16_t>(RejectReason::kInvalidCID)),

      // Source CID (relative to rejecter)
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Destination CID (relative to rejecter)
      LowerBits(kLocalCId),
      UpperBits(kLocalCId));

  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      expected_discon_req.view(),
                      {SignalingChannel::Status::kReject, rej_cid.view()});

  bool cb_called = false;
  CommandHandler::DisconnectionResponseCallback on_discon_rsp =
      [&cb_called](const CommandHandler::DisconnectionResponse& rsp) {
        cb_called = true;
        EXPECT_EQ(SignalingChannel::Status::kReject, rsp.status());
        EXPECT_EQ(RejectReason::kInvalidCID, rsp.reject_reason());
        EXPECT_EQ(kLocalCId, rsp.local_cid());
        EXPECT_EQ(kRemoteCId, rsp.remote_cid());
      };

  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kLocalCId, std::move(on_discon_rsp)));
  RunUntilIdle();
  EXPECT_TRUE(cb_called);
}

TEST_F(CommandHandlerTest, OutboundDisconReqRejNotEnoughBytes) {
  constexpr ChannelId kBadLocalCId = 0x0005;  // Not a dynamic channel

  // Disconnect Request payload
  auto expected_discon_req = StaticByteBuffer(
      // Destination CID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Source CID
      LowerBits(kBadLocalCId),
      UpperBits(kBadLocalCId));

  // Invalid Command Reject payload (size is too small)
  auto rej_rsp = StaticByteBuffer(0x01);

  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      expected_discon_req.view(),
                      {SignalingChannel::Status::kReject, rej_rsp.view()});

  bool cb_called = false;
  auto on_discon_rsp =
      [&cb_called](const CommandHandler::DisconnectionResponse&) {
        cb_called = true;
      };

  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kBadLocalCId, std::move(on_discon_rsp)));
  RunUntilIdle();
  EXPECT_FALSE(cb_called);
}

TEST_F(CommandHandlerTest, OutboundDisconReqRejInvalidCIDNotEnoughBytes) {
  constexpr ChannelId kBadLocalCId = 0x0005;  // Not a dynamic channel

  // Disconnect Request payload
  auto expected_discon_req = StaticByteBuffer(
      // Destination CID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Source CID
      LowerBits(kBadLocalCId),
      UpperBits(kBadLocalCId));

  // Command Reject payload (the invalid channel IDs are missing)
  auto rej_rsp = StaticByteBuffer(
      // Reject Reason (invalid channel ID)
      LowerBits(static_cast<uint16_t>(RejectReason::kInvalidCID)),
      UpperBits(static_cast<uint16_t>(RejectReason::kInvalidCID)));

  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      expected_discon_req.view(),
                      {SignalingChannel::Status::kReject, rej_rsp.view()});

  bool cb_called = false;
  auto on_discon_rsp =
      [&cb_called](const CommandHandler::DisconnectionResponse&) {
        cb_called = true;
      };

  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kBadLocalCId, std::move(on_discon_rsp)));
  RunUntilIdle();
  EXPECT_FALSE(cb_called);
}

TEST_F(CommandHandlerTest, InboundDisconReqRspOk) {
  CommandHandler::DisconnectionRequestCallback cb =
      [](ChannelId local_cid, ChannelId remote_cid, auto responder) {
        EXPECT_EQ(kLocalCId, local_cid);
        EXPECT_EQ(kRemoteCId, remote_cid);
        responder->Send();
      };
  cmd_handler()->ServeDisconnectionRequest(std::move(cb));

  // Disconnection Request payload
  auto discon_req = StaticByteBuffer(
      // Destination CID (relative to requester)
      LowerBits(kLocalCId),
      UpperBits(kLocalCId),

      // Source CID (relative to requester)
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId));

  // Disconnection Response payload is identical to request payload.
  auto expected_rsp = discon_req;

  RETURN_IF_FATAL(fake_sig()->ReceiveExpect(
      kDisconnectionRequest, discon_req, expected_rsp));
}

TEST_F(CommandHandlerTest, InboundDisconReqRej) {
  CommandHandler::DisconnectionRequestCallback cb =
      [](ChannelId local_cid, ChannelId remote_cid, auto responder) {
        EXPECT_EQ(kLocalCId, local_cid);
        EXPECT_EQ(kRemoteCId, remote_cid);
        responder->RejectInvalidChannelId();
      };
  cmd_handler()->ServeDisconnectionRequest(std::move(cb));

  // Disconnection Request payload
  auto discon_req = StaticByteBuffer(
      // Destination CID (relative to requester)
      LowerBits(kLocalCId),
      UpperBits(kLocalCId),

      // Source CID (relative to requester)
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId));

  // Disconnection Response payload
  auto expected_rsp = discon_req;

  RETURN_IF_FATAL(fake_sig()->ReceiveExpectRejectInvalidChannelId(
      kDisconnectionRequest, discon_req, kLocalCId, kRemoteCId));
}

TEST_F(CommandHandlerTest, OutboundDisconReqRspPayloadNotEnoughBytes) {
  // Disconnect Request payload
  auto expected_discon_req = StaticByteBuffer(
      // Destination CID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Source CID
      LowerBits(kLocalCId),
      UpperBits(kLocalCId));

  // Disconnect Response payload (should include Source CID)
  auto malformed_discon_rsp = StaticByteBuffer(
      // Destination CID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId));

  EXPECT_OUTBOUND_REQ(
      *fake_sig(),
      kDisconnectionRequest,
      expected_discon_req.view(),
      {SignalingChannel::Status::kSuccess, malformed_discon_rsp.view()});

  bool cb_called = false;
  auto on_discon_cb =
      [&cb_called](const CommandHandler::DisconnectionResponse&) {
        cb_called = true;
      };

  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kLocalCId, std::move(on_discon_cb)));
  RunUntilIdle();
  EXPECT_FALSE(cb_called);
}

TEST_F(CommandHandlerTest, OutboundReqRspDecodeError) {
  auto payload = StaticByteBuffer(0x00);
  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      payload.view(),
                      {SignalingChannel::Status::kSuccess, payload.view()});

  bool cb_called = false;
  auto on_rsp_cb =
      [&cb_called](const TestCommandHandler::UndecodableResponse&) {
        cb_called = true;
      };

  EXPECT_TRUE(cmd_handler()->SendRequestWithUndecodableResponse(
      kDisconnectionRequest, payload, std::move(on_rsp_cb)));
  RunUntilIdle();
  EXPECT_FALSE(cb_called);
  EXPECT_EQ(1u, failed_requests());
}

TEST_F(CommandHandlerTest, OutboundDisconReqRspTimeOut) {
  // Disconnect Request payload
  auto expected_discon_req = StaticByteBuffer(
      // Destination CID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),

      // Source CID
      LowerBits(kLocalCId),
      UpperBits(kLocalCId));

  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      expected_discon_req.view(),
                      {SignalingChannel::Status::kTimeOut, {}});
  EXPECT_OUTBOUND_REQ(
      *fake_sig(), kDisconnectionRequest, expected_discon_req.view());

  set_request_fail_callback([this]() {
    // Should still be allowed to send requests even after one failed
    auto on_discon_rsp = [](auto&) {};
    EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
        kRemoteCId, kLocalCId, std::move(on_discon_rsp)));
  });

  auto on_discon_rsp = [](auto&) { ADD_FAILURE(); };

  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kLocalCId, std::move(on_discon_rsp)));

  ASSERT_EQ(0u, failed_requests());
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_EQ(1u, failed_requests());
}

TEST_F(CommandHandlerTest, RejectInvalidChannelId) {
  CommandHandler::DisconnectionRequestCallback cb =
      [](ChannelId,
         ChannelId,
         CommandHandler::DisconnectionResponder* responder) {
        responder->RejectInvalidChannelId();
      };
  cmd_handler()->ServeDisconnectionRequest(std::move(cb));

  // Disconnection Request payload
  auto discon_req = StaticByteBuffer(
      // Destination CID (relative to requester)
      LowerBits(kLocalCId),
      UpperBits(kLocalCId),

      // Source CID (relative to requester)
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId));

  RETURN_IF_FATAL(fake_sig()->ReceiveExpectRejectInvalidChannelId(
      kDisconnectionRequest, discon_req, kLocalCId, kRemoteCId));
}

TEST_F(CommandHandlerTest, SendCredits) {
  constexpr ChannelId kExpectedChannel = 0x1234;
  constexpr uint16_t kExpectedCredits = 0x0142;
  StaticByteBuffer expected_credit_payload(
      // Channel ID
      LowerBits(kExpectedChannel),
      UpperBits(kExpectedChannel),

      // Credits
      LowerBits(kExpectedCredits),
      UpperBits(kExpectedCredits));

  EXPECT_OUTBOUND_REQ(
      *fake_sig(), kLEFlowControlCredit, expected_credit_payload.view());
  cmd_handler()->SendCredits(kExpectedChannel, kExpectedCredits);
  RunUntilIdle();
}

TEST_F(CommandHandlerTest, ReceiveCredits) {
  const uint16_t expected_credits = 5;
  int cb_count = 0;
  cmd_handler()->ServeFlowControlCreditInd(
      [&cb_count, expected_credits](ChannelId remote_cid, uint16_t credits) {
        cb_count++;
        EXPECT_EQ(remote_cid, kRemoteCId);
        EXPECT_EQ(credits, expected_credits);
      });

  StaticByteBuffer payload(
      // Channel ID
      LowerBits(kRemoteCId),
      UpperBits(kRemoteCId),
      // Credits
      LowerBits(expected_credits),
      UpperBits(expected_credits));
  fake_sig()->Receive(kLEFlowControlCredit, payload);
  RunUntilIdle();
  EXPECT_EQ(cb_count, 1);
}

// Tests that when a peer responds to a disconnection request with a
// malformed/truncated L2CAP_DISCONNECTION_RSP packet (2 bytes instead of the
// required 4 payload bytes per Bluetooth Core Specification v5.0, Vol 3, Part
// A, Section 4.7), the command handler rejects the packet and invokes the
// request's failure callback rather than leaving the transaction hanging until
// timeout.
TEST_F(CommandHandlerTest, ShortDisconnectionResponse_FailCallbackFires) {
  StaticByteBuffer req(LowerBits(kRemoteCId),
                       UpperBits(kRemoteCId),
                       LowerBits(kLocalCId),
                       UpperBits(kLocalCId));

  // ATTACKER PACKET: 2 of the required 4 payload bytes.
  StaticByteBuffer short_rsp(LowerBits(kRemoteCId), UpperBits(kRemoteCId));

  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      req.view(),
                      {SignalingChannel::Status::kSuccess, short_rsp.view()});

  bool rsp_cb_called = false;
  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kLocalCId, [&](const auto&) { rsp_cb_called = true; }));
  RunUntilIdle();

  EXPECT_FALSE(rsp_cb_called);
  EXPECT_EQ(1u, failed_requests());
}

// Tests that when a peer rejects a command with a malformed
// L2CAP_COMMAND_REJECT_RSP packet (1 byte instead of at least 2 bytes for the
// Reason field per Bluetooth Core Specification v5.0, Vol 3, Part A, Section
// 4.1), the command handler safely handles the decode failure and invokes the
// request's failure callback rather than calling the response callback or
// leaving the transaction hanging until timeout.
TEST_F(CommandHandlerTest, MalformedReject_FailCallbackFires) {
  StaticByteBuffer req(LowerBits(kRemoteCId),
                       UpperBits(kRemoteCId),
                       LowerBits(kLocalCId),
                       UpperBits(kLocalCId));

  // ATTACKER PACKET: 1-byte Reject payload.
  StaticByteBuffer short_rej(0x00);

  EXPECT_OUTBOUND_REQ(*fake_sig(),
                      kDisconnectionRequest,
                      req.view(),
                      {SignalingChannel::Status::kReject, short_rej.view()});

  bool rsp_cb_called = false;
  EXPECT_TRUE(cmd_handler()->SendDisconnectionRequest(
      kRemoteCId, kLocalCId, [&](const auto&) { rsp_cb_called = true; }));
  RunUntilIdle();

  EXPECT_FALSE(rsp_cb_called);
  EXPECT_EQ(1u, failed_requests());
}

}  // namespace
}  // namespace bt::l2cap::internal
