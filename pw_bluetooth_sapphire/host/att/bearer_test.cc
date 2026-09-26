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

// inclusive-language: disable

#include "pw_bluetooth_sapphire/internal/host/att/bearer.h"

#include "pw_bluetooth_sapphire/internal/host/l2cap/mock_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::att {
namespace {

constexpr OpCode kTestRequest = kFindInformationRequest;
constexpr OpCode kTestResponse = kFindInformationResponse;
constexpr OpCode kTestRequest2 = kExchangeMTURequest;
constexpr OpCode kTestResponse2 = kExchangeMTUResponse;
constexpr OpCode kTestRequest3 = kFindByTypeValueRequest;
constexpr OpCode kTestResponse3 = kFindByTypeValueResponse;

constexpr OpCode kTestCommand = kWriteCommand;

void NopHandler(Bearer::TransactionId /*unused*/,
                const PacketReader& /*unused*/) {}

class BearerTest : public l2cap::testing::MockChannelTest {
 public:
  BearerTest() = default;
  ~BearerTest() override = default;

 protected:
  void SetUp() override {
    ChannelOptions options(l2cap::kATTChannelId);
    bearer_ =
        Bearer::Create(CreateFakeChannel(options)->GetWeakPtr(), dispatcher());
  }

  void TearDown() override { bearer_ = nullptr; }

  Bearer* bearer() const { return bearer_.get(); }
  l2cap::testing::FakeChannel::WeakPtr fake_att_chan() const {
    return fake_chan();
  }

  void DeleteBearer() { bearer_ = nullptr; }

 private:
  std::unique_ptr<Bearer> bearer_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BearerTest);
};

TEST_F(BearerTest, CreateFailsToActivate) {
  ChannelOptions options(l2cap::kATTChannelId);
  auto fake_chan = CreateFakeChannel(options);
  fake_chan->set_activate_fails(true);

  EXPECT_FALSE(Bearer::Create(fake_chan->GetWeakPtr(), dispatcher()));
}

TEST_F(BearerTest, CreateUsesLEMaxMTUAsPreferredMTU) {
  EXPECT_EQ(kLEMaxMTU, bearer()->preferred_mtu());
}

TEST_F(BearerTest, ShutDown) {
  ASSERT_TRUE(bearer()->is_open());
  ASSERT_FALSE(fake_att_chan()->link_error());

  // Verify that shutting down an open bearer notifies the closed callback.
  bool called = false;
  auto cb = [&called] { called = true; };

  bearer()->set_closed_callback(cb);
  bearer()->ShutDown();
  EXPECT_TRUE(called);
  EXPECT_FALSE(bearer()->is_open());

  // Bearer should also signal a link error over the channel.
  EXPECT_TRUE(fake_att_chan()->link_error());

  // ShutDown() on a closed bearer does nothing.
  bearer()->ShutDown();
  EXPECT_FALSE(bearer()->is_open());
}

TEST_F(BearerTest, StartTransactionErrorClosed) {
  bearer()->ShutDown();
  ASSERT_FALSE(bearer()->is_open());

  bool received_error = false;
  Bearer::TransactionCallback cb =
      [&received_error](Bearer::TransactionResult result) {
        if (result.is_error()) {
          received_error = true;
        }
      };
  bearer()->StartTransaction(NewBuffer(kTestRequest), std::move(cb));
  EXPECT_TRUE(received_error);
}

TEST_F(BearerTest, StartTransactionInvalidPacket) {
  auto cb = [](Bearer::TransactionResult) { FAIL(); };

  // Empty
  EXPECT_DEATH_IF_SUPPORTED(
      bearer()->StartTransaction(std::make_unique<BufferView>(), cb),
      "bad length");

  // Exceeds MTU.
  bearer()->set_mtu(1);
  EXPECT_DEATH_IF_SUPPORTED(
      bearer()->StartTransaction(NewBuffer(kTestRequest, 2), cb), "bad length");
}

TEST_F(BearerTest, StartTransactionWrongMethodType) {
  auto cb = [](Bearer::TransactionResult) { FAIL(); };

  // Command
  EXPECT_DEATH_IF_SUPPORTED(
      bearer()->StartTransaction(NewBuffer(kWriteCommand), cb),
      "callback was provided");

  // Notification
  EXPECT_DEATH_IF_SUPPORTED(
      bearer()->StartTransaction(NewBuffer(kNotification), cb),
      "callback was provided");
}

TEST_F(BearerTest, RequestTimeout) {
  // We expect the channel to be closed and the pending transaction to end in an
  // error.
  bool closed = false;
  bool err_cb_called = false;
  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kTimedOut), error);
    EXPECT_EQ(0, handle);

    err_cb_called = true;
  };

  ASSERT_FALSE(fake_att_chan()->link_error());
  MutableByteBufferPtr request = NewBuffer(kTestRequest);
  EXPECT_PACKET_OUT(*request);
  bearer()->StartTransaction(std::move(request), cb);

  RunFor(kTransactionTimeout);

  EXPECT_TRUE(closed);
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(fake_att_chan()->link_error());
}

// Queue many requests but make sure that FakeChannel only receives one.
TEST_F(BearerTest, RequestTimeoutMany) {
  constexpr unsigned int kTransactionCount = 2;
  unsigned int chan_count = 0;
  auto chan_cb = [&chan_count](auto cb_packet) {
    chan_count++;
    // This should only be called once and for the first request.
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool closed = false;
  unsigned int err_cb_count = 0u;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_count](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kTimedOut), error);
    EXPECT_EQ(0, handle);

    err_cb_count++;
  };

  bearer()->StartTransaction(NewBuffer(kTestRequest, 'T', 'e', 's', 't'), cb);
  bearer()->StartTransaction(NewBuffer(kTestRequest2, 'T', 'e', 's', 't'), cb);

  RunUntilIdle();

  // The first indication should have been sent and the second one queued.
  EXPECT_EQ(1u, chan_count);

  EXPECT_FALSE(closed);
  EXPECT_EQ(0u, err_cb_count);

  // Make the request timeout.
  RunFor(kTransactionTimeout);

  EXPECT_TRUE(closed);
  EXPECT_EQ(kTransactionCount, err_cb_count);
}

TEST_F(BearerTest, IndicationTimeout) {
  // We expect the channel to be closed and the pending transaction to end in an
  // error.
  bool closed = false;
  bool err_cb_called = false;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kTimedOut), error);
    EXPECT_EQ(0, handle);

    err_cb_called = true;
  };
  MutableByteBufferPtr request = NewBuffer(kIndication, 'T', 'e', 's', 't');
  EXPECT_PACKET_OUT(*request);
  bearer()->StartTransaction(std::move(request), cb);

  RunFor(kTransactionTimeout);

  EXPECT_TRUE(closed);
  EXPECT_TRUE(err_cb_called);
}

// Queue many indications but make sure that FakeChannel only receives one.
TEST_F(BearerTest, IndicationTimeoutMany) {
  constexpr unsigned int kTransactionCount = 2;
  constexpr uint8_t kIndValue1 = 1;
  constexpr uint8_t kIndValue2 = 2;

  unsigned int chan_count = 0;
  auto chan_cb = [kIndValue1, &chan_count](auto cb_packet) {
    chan_count++;
    // This should only be called once and for the first request.
    EXPECT_EQ(kIndValue1, (*cb_packet)[1]);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool closed = false;
  unsigned int err_cb_count = 0u;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_count](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kTimedOut), error);
    EXPECT_EQ(0, handle);

    err_cb_count++;
  };

  bearer()->StartTransaction(NewBuffer(kIndication, kIndValue1), cb);
  bearer()->StartTransaction(NewBuffer(kIndication, kIndValue2), cb);

  RunUntilIdle();

  // The first indication should have been sent and the second one queued.
  EXPECT_EQ(1u, chan_count);

  EXPECT_FALSE(closed);
  EXPECT_EQ(0u, err_cb_count);

  // Make the request timeout.
  RunFor(kTransactionTimeout);

  EXPECT_TRUE(closed);
  EXPECT_EQ(kTransactionCount, err_cb_count);
}

TEST_F(BearerTest, ReceiveEmptyPacket) {
  bool closed = false;
  bearer()->set_closed_callback([&closed] { closed = true; });

  fake_chan()->Receive(BufferView());

  RunUntilIdle();
  EXPECT_TRUE(closed);
}

TEST_F(BearerTest, ReceiveResponseWithoutRequest) {
  bool closed = false;
  bearer()->set_closed_callback([&closed] { closed = true; });

  fake_chan()->Receive(StaticByteBuffer(kTestResponse));

  RunUntilIdle();
  EXPECT_TRUE(closed);
}

TEST_F(BearerTest, ReceiveConfirmationWithoutIndication) {
  bool closed = false;
  bearer()->set_closed_callback([&closed] { closed = true; });

  fake_chan()->Receive(StaticByteBuffer(kConfirmation));

  RunUntilIdle();
  EXPECT_TRUE(closed);
}

TEST_F(BearerTest, SendRequestWrongResponse) {
  unsigned int count = 0;
  auto chan_cb = [this, &count](auto cb_packet) {
    count++;
    // This should only be called once and for the first request.
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    // Send back the wrong response.
    fake_chan()->Receive(StaticByteBuffer(kTestResponse2));
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool err_cb_called = false;
  bool closed = false;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kFailed), error);
    EXPECT_EQ(0, handle);

    err_cb_called = true;
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_TRUE(err_cb_called);
  EXPECT_EQ(1u, count);
}

TEST_F(BearerTest, SendRequestErrorResponseTooShort) {
  StaticByteBuffer malformed_error_rsp(
      // Opcode: error response
      kErrorResponse,

      // Parameters are too short (by 1 byte). Contents are unimportant, as the
      // PDU should be rejected.
      1,
      2,
      3);

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &malformed_error_rsp](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(malformed_error_rsp);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool err_cb_called = false;
  bool closed = false;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kFailed), error);
    EXPECT_EQ(0, handle);

    err_cb_called = true;
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(chan_cb_called);
}

TEST_F(BearerTest, SendRequestErrorResponseTooLong) {
  StaticByteBuffer malformed_error_rsp(
      // Opcode: error response
      kErrorResponse,

      // Parameters are too long (by 1 byte). Contents are unimportant, as the
      // PDU should be rejected.
      1,
      2,
      3,
      4,
      5);

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &malformed_error_rsp](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(malformed_error_rsp);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool err_cb_called = false;
  bool closed = false;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kFailed), error);
    EXPECT_EQ(0, handle);

    err_cb_called = true;
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(chan_cb_called);
}

TEST_F(BearerTest, SendRequestErrorResponseWrongOpCode) {
  StaticByteBuffer error_rsp(
      // Opcode: error response
      kErrorResponse,

      // request opcode: non-matching opcode in error response
      kTestRequest2,

      // handle, should be ignored
      0x00,
      0x00,

      // error code:
      ErrorCode::kRequestNotSupported);

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &error_rsp](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(error_rsp);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool err_cb_called = false;
  bool closed = false;

  bearer()->set_closed_callback([&closed] { closed = true; });

  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(HostError::kFailed), error);
    EXPECT_EQ(0, handle);

    err_cb_called = true;
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(chan_cb_called);
}

TEST_F(BearerTest, SendRequestErrorResponse) {
  StaticByteBuffer error_rsp(
      // Opcode: error response
      kErrorResponse,

      // request opcode
      kTestRequest,

      // handle (0x0001)
      0x01,
      0x00,

      // error code:
      ErrorCode::kRequestNotSupported);

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &error_rsp](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(error_rsp);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool err_cb_called = false;
  auto cb = [&err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(ErrorCode::kRequestNotSupported), error);
    EXPECT_EQ(0x0001, handle);

    err_cb_called = true;
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(chan_cb_called);

  // The channel should remain open
  EXPECT_TRUE(bearer()->is_open());
}

TEST_F(BearerTest, SendRequestSuccess) {
  StaticByteBuffer response(kTestResponse, 'T', 'e', 's', 't');

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &response](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(response);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool cb_called = false;
  auto cb = [&cb_called, &response](Bearer::TransactionResult result) {
    ASSERT_FALSE(cb_called);
    if (result.is_error()) {
      return;
    }

    cb_called = true;
    EXPECT_TRUE(ContainersEqual(response, result.value().data()));
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(chan_cb_called);
  EXPECT_TRUE(cb_called);

  // The channel should remain open
  EXPECT_TRUE(bearer()->is_open());
}

// Closing the L2CAP channel while ATT requests have been queued will cause the
// error callbacks to be called. The code should fail gracefully if one of these
// callbacks deletes the attribute bearer.
TEST_F(BearerTest, CloseChannelAndDeleteBearerWhileRequestsArePending) {
  // We expect the callback to be called 3 times since we queue 3 transactions
  // below.
  constexpr size_t kExpectedCount = 3;

  size_t cb_error_count = 0;
  auto cb = [this, &cb_error_count](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    cb_error_count++;

    // Delete the bearer on the first callback. The remaining callbacks should
    // still run gracefully.
    DeleteBearer();
  };

  MutableByteBufferPtr kRequest0 = NewBuffer(kTestRequest);
  EXPECT_PACKET_OUT(*kRequest0);
  bearer()->StartTransaction(std::move(kRequest0), cb);

  MutableByteBufferPtr kRequest1 = NewBuffer(kTestRequest);
  EXPECT_PACKET_OUT(*kRequest1);
  bearer()->StartTransaction(std::move(kRequest1), cb);

  MutableByteBufferPtr kRequest2 = NewBuffer(kTestRequest);
  EXPECT_PACKET_OUT(*kRequest2);
  bearer()->StartTransaction(std::move(kRequest2), cb);

  fake_chan()->Close();
  EXPECT_EQ(kExpectedCount, cb_error_count);
}

TEST_F(BearerTest, SendManyRequests) {
  const StaticByteBuffer response1(kTestResponse, 'f', 'o', 'o');
  const StaticByteBuffer response2(kErrorResponse,

                                   // request opcode
                                   kTestRequest2,

                                   // handle (0x0001)
                                   0x01,
                                   0x00,

                                   // error code:
                                   ErrorCode::kRequestNotSupported);
  const StaticByteBuffer response3(kTestResponse3, 'b', 'a', 'r');

  auto chan_cb = [&, this](auto cb_packet) {
    OpCode opcode = (*cb_packet)[0];

    if (opcode == kTestRequest) {
      fake_chan()->Receive(response1);
    } else if (opcode == kTestRequest2) {
      fake_chan()->Receive(response2);
    } else if (opcode == kTestRequest3) {
      fake_chan()->Receive(response3);
    }
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool called_1 = false, called_2 = false, called_3 = false;

  // We expect each callback to be called in the order that we send the
  // corresponding request.
  auto callback1 = [&called_1, &called_2, &called_3, &response1](
                       Bearer::TransactionResult result) {
    EXPECT_FALSE(called_2);
    EXPECT_FALSE(called_3);
    called_1 = true;

    // First request should've succeeded
    ASSERT_EQ(fit::ok(), result);
    EXPECT_TRUE(ContainersEqual(response1, result.value().data()));
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), callback1);

  auto callback2 =
      [&called_1, &called_2, &called_3](Bearer::TransactionResult result) {
        EXPECT_TRUE(called_1);
        EXPECT_FALSE(called_3);
        called_2 = true;

        // Second request should've failed
        ASSERT_EQ(fit::failed(), result);
        const auto& [error, handle] = result.error_value();
        EXPECT_EQ(Error(ErrorCode::kRequestNotSupported), error);
        EXPECT_EQ(0x0001, handle);
      };
  bearer()->StartTransaction(NewBuffer(kTestRequest2), callback2);

  auto callback3 = [&called_1, &called_2, &called_3, &response3](
                       Bearer::TransactionResult result) {
    EXPECT_TRUE(called_1);
    EXPECT_TRUE(called_2);
    called_3 = true;

    // Third request should've succeeded
    ASSERT_EQ(fit::ok(), result);
    EXPECT_TRUE(ContainersEqual(response3, result.value().data()));
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest3), callback3);

  RunUntilIdle();

  EXPECT_TRUE(bearer()->is_open());
}

// An indication transaction can only fail in a circumstance that would shut
// down the bearer (e.g. a transaction timeout or an empty PDU). Otherwise,
// Bearer will only complete an indication transaction when it receives a
// confirmation PDU.
//
// NOTE: Bearer only looks at the opcode of a PDU and ignores the payload, so a
// malformed confirmation payload is not considered an error at this layer.
TEST_F(BearerTest, SendIndicationSuccess) {
  // Even though this is a malformed confirmation PDU it will not be rejected by
  // Bearer.
  StaticByteBuffer conf(kConfirmation, 'T', 'e', 's', 't');

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &conf](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kIndication, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(conf);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool cb_called = false;
  auto cb = [&cb_called, &conf](Bearer::TransactionResult result) {
    ASSERT_FALSE(cb_called);

    cb_called = true;
    EXPECT_TRUE(ContainersEqual(conf, result.value().data()));
  };
  bearer()->StartTransaction(NewBuffer(kIndication), cb);

  RunUntilIdle();
  EXPECT_TRUE(chan_cb_called);
  EXPECT_TRUE(cb_called);

  // The channel should remain open
  EXPECT_TRUE(bearer()->is_open());
}

TEST_F(BearerTest, SendWithoutResponseErrorClosed) {
  bearer()->ShutDown();
  ASSERT_FALSE(bearer()->is_open());

  EXPECT_FALSE(bearer()->SendWithoutResponse(NewBuffer(kTestCommand)));
}

TEST_F(BearerTest, SendWithoutResponseInvalidPacket) {
  // Empty
  EXPECT_DEATH_IF_SUPPORTED(
      [[maybe_unused]] auto _ =
          bearer()->SendWithoutResponse(std::make_unique<BufferView>()),
      "bad length");

  // Exceeds MTU
  bearer()->set_mtu(1);
  EXPECT_DEATH_IF_SUPPORTED(
      [[maybe_unused]] auto _ =
          bearer()->SendWithoutResponse(NewBuffer(kTestCommand, 2)),
      "bad length");
}

TEST_F(BearerTest, SendWithoutResponseWrongMethodType) {
  EXPECT_DEATH_IF_SUPPORTED(
      [[maybe_unused]] auto _ =
          bearer()->SendWithoutResponse(NewBuffer(kTestRequest)),
      "requires callback");
  EXPECT_DEATH_IF_SUPPORTED(
      [[maybe_unused]] auto _ =
          bearer()->SendWithoutResponse(NewBuffer(kTestResponse)),
      "unsupported opcode");
  EXPECT_DEATH_IF_SUPPORTED(
      [[maybe_unused]] auto _ =
          bearer()->SendWithoutResponse(NewBuffer(kIndication)),
      "requires callback");
}

TEST_F(BearerTest, SendWithoutResponseCorrectMethodType) {
  MutableByteBufferPtr request_0 = NewBuffer(kNotification);
  EXPECT_PACKET_OUT(*request_0);
  EXPECT_TRUE(bearer()->SendWithoutResponse(std::move(request_0)));
  MutableByteBufferPtr request_1 = NewBuffer(kTestCommand);
  EXPECT_PACKET_OUT(*request_1);
  EXPECT_TRUE(bearer()->SendWithoutResponse(std::move(request_1)));
  MutableByteBufferPtr request_2 = NewBuffer(kTestRequest | kCommandFlag);
  EXPECT_PACKET_OUT(*request_2);
  EXPECT_TRUE(bearer()->SendWithoutResponse(std::move(request_2)));

  // Any opcode is accepted as long as it has the command flag set.
  auto request_3 = NewBuffer(kInvalidOpCode | kCommandFlag);
  EXPECT_PACKET_OUT(*request_3);
  EXPECT_TRUE(bearer()->SendWithoutResponse(std::move(request_3)));
}

TEST_F(BearerTest, SendWithoutResponseMany) {
  // Everything should go through without any flow control.
  constexpr unsigned int kExpectedCount = 10;
  unsigned int chan_cb_count = 0u;

  auto chan_cb = [&chan_cb_count](auto cb_packet) {
    OpCode opcode = (*cb_packet)[0];
    EXPECT_TRUE(kCommandFlag & opcode || opcode == kIndication);

    chan_cb_count++;
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  for (OpCode opcode = 0; opcode < kExpectedCount; opcode++) {
    // Everything
    EXPECT_TRUE(
        bearer()->SendWithoutResponse(NewBuffer(opcode | kCommandFlag)));
  }

  RunUntilIdle();
  EXPECT_EQ(kExpectedCount, chan_cb_count);
}

TEST_F(BearerTest, UpgradeSecuritySynchronousFailureIsHandledGracefully) {
  // Set up the security callback to fail synchronously.
  // This emulates a situation where our device itself can't handle the security
  // request (e.g. a device with no input or output capabilities)
  bool security_cb_called = false;
  fake_att_chan()->SetSecurityCallback(
      [&security_cb_called](hci_spec::ConnectionHandle /*handle*/,
                            sm::SecurityLevel /*level*/,
                            sm::ResultFunction<> callback) {
        security_cb_called = true;
        // Fail synchronously with a non-fatal error
        callback(fit::error(HostError::kFailed));
      },
      dispatcher());
  fake_att_chan()->set_run_upgrade_security_cb_synchronously(true);

  bool err_cb_called = false;

  StaticByteBuffer kErrorResponseInsuffAuth(
      kErrorResponse,
      kTestRequest,  // request opcode
      0x01,
      0x00,  // handle
      ErrorCode::kInsufficientAuthentication);

  auto cb = [this, &err_cb_called](Bearer::TransactionResult result) {
    err_cb_called = true;
    EXPECT_TRUE(result.is_error());
    // Delete the bearer in the callback akin to what LEConnectionManager might
    // do if security upgrade failed.
    DeleteBearer();
  };

  StaticByteBuffer kExpectedReq(kTestRequest);
  EXPECT_PACKET_OUT(kExpectedReq);
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  // Simulate receiving the error response.
  fake_att_chan()->Receive(kErrorResponseInsuffAuth);
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(security_cb_called);
}

TEST_F(BearerTest, RegisterHandlerErrorClosed) {
  bearer()->ShutDown();
  EXPECT_FALSE(bearer()->is_open());
  EXPECT_EQ(Bearer::kInvalidHandlerId,
            bearer()->RegisterHandler(kWriteRequest, NopHandler));
  EXPECT_EQ(Bearer::kInvalidHandlerId,
            bearer()->RegisterHandler(kIndication, NopHandler));
}

TEST_F(BearerTest, RegisterHandlerErrorAlreadyRegistered) {
  EXPECT_NE(Bearer::kInvalidHandlerId,
            bearer()->RegisterHandler(kIndication, NopHandler));
  EXPECT_EQ(Bearer::kInvalidHandlerId,
            bearer()->RegisterHandler(kIndication, NopHandler));
}

TEST_F(BearerTest, UnregisterHandler) {
  auto id0 = bearer()->RegisterHandler(kNotification, NopHandler);
  EXPECT_NE(Bearer::kInvalidHandlerId, id0);

  bearer()->UnregisterHandler(id0);

  // It should be possible to register new handlers for the same opcodes.
  id0 = bearer()->RegisterHandler(kNotification, NopHandler);
  EXPECT_NE(Bearer::kInvalidHandlerId, id0);
}

TEST_F(BearerTest, RemoteTransactionNoHandler) {
  StaticByteBuffer error_rsp(
      // opcode
      kErrorResponse,

      // request opcode
      kTestRequest,

      // handle
      0x00,
      0x00,

      // error code
      ErrorCode::kRequestNotSupported);

  bool received_error_rsp = false;
  auto chan_cb = [&received_error_rsp, &error_rsp](auto packet) {
    received_error_rsp = true;
    EXPECT_TRUE(ContainersEqual(error_rsp, *packet));
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());
  fake_chan()->Receive(StaticByteBuffer(kTestRequest));

  RunUntilIdle();
  EXPECT_TRUE(received_error_rsp);
}

TEST_F(BearerTest, RemoteTransactionSeqProtocolError) {
  int request_count = 0;
  auto handler = [&request_count](auto, const PacketReader& packet) {
    EXPECT_EQ(kTestRequest, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    request_count++;
  };

  bearer()->RegisterHandler(kTestRequest, handler);
  fake_chan()->Receive(StaticByteBuffer(kTestRequest));

  RunUntilIdle();
  ASSERT_EQ(1, request_count);

  // Receiving a second request before sending a response should close the
  // bearer.
  bool closed = false;
  bearer()->set_closed_callback([&closed] { closed = true; });

  fake_chan()->Receive(StaticByteBuffer(kTestRequest));

  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_EQ(1, request_count);
  EXPECT_FALSE(bearer()->is_open());
}

TEST_F(BearerTest, RemoteIndicationSeqProtocolError) {
  int ind_count = 0;
  auto handler = [&ind_count](auto, const PacketReader& packet) {
    EXPECT_EQ(kIndication, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    ind_count++;
  };

  bearer()->RegisterHandler(kIndication, handler);
  fake_chan()->Receive(StaticByteBuffer(kIndication));

  RunUntilIdle();
  ASSERT_EQ(1, ind_count);

  // Receiving a second indication before sending a confirmation should close
  // the bearer.
  bool closed = false;
  bearer()->set_closed_callback([&closed] { closed = true; });

  fake_chan()->Receive(StaticByteBuffer(kIndication));

  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_EQ(1, ind_count);
  EXPECT_FALSE(bearer()->is_open());
}

TEST_F(BearerTest, ReplyInvalidPacket) {
  // Empty
  EXPECT_FALSE(bearer()->Reply(0, std::make_unique<BufferView>()));

  // Exceeds MTU.
  bearer()->set_mtu(1);
  EXPECT_FALSE(bearer()->Reply(0, NewBuffer(kTestRequest, 2)));
}

TEST_F(BearerTest, ReplyInvalidId) {
  EXPECT_FALSE(
      bearer()->Reply(Bearer::kInvalidTransactionId, NewBuffer(kTestResponse)));

  // The ID is valid but doesn't correspond to an active transaction.
  EXPECT_FALSE(bearer()->Reply(1u, NewBuffer(kTestResponse)));
}

TEST_F(BearerTest, ReplyWrongOpCode) {
  Bearer::TransactionId id;
  bool handler_called = false;
  auto handler = [&id, &handler_called](auto cb_id,
                                        const PacketReader& packet) {
    EXPECT_EQ(kTestRequest, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    handler_called = true;
    id = cb_id;
  };

  bearer()->RegisterHandler(kTestRequest, handler);
  fake_chan()->Receive(StaticByteBuffer(kTestRequest));

  RunUntilIdle();
  ASSERT_TRUE(handler_called);

  EXPECT_FALSE(bearer()->Reply(id, NewBuffer(kTestResponse2)));
}

TEST_F(BearerTest, ReplyToIndicationWrongOpCode) {
  Bearer::TransactionId id;
  bool handler_called = false;
  auto handler = [&id, &handler_called](auto cb_id,
                                        const PacketReader& packet) {
    EXPECT_EQ(kIndication, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    handler_called = true;
    id = cb_id;
  };

  bearer()->RegisterHandler(kIndication, handler);
  fake_chan()->Receive(StaticByteBuffer(kIndication));

  RunUntilIdle();
  ASSERT_TRUE(handler_called);

  EXPECT_FALSE(bearer()->Reply(id, NewBuffer(kTestResponse)));
}

TEST_F(BearerTest, ReplyWithResponse) {
  bool response_sent = false;
  auto chan_cb = [&response_sent](auto packet) {
    response_sent = true;

    EXPECT_EQ(kTestResponse, (*packet)[0]);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  Bearer::TransactionId id;
  bool handler_called = false;
  auto handler = [&id, &handler_called](auto cb_id,
                                        const PacketReader& packet) {
    EXPECT_EQ(kTestRequest, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    handler_called = true;
    id = cb_id;
  };

  bearer()->RegisterHandler(kTestRequest, handler);
  fake_chan()->Receive(StaticByteBuffer(kTestRequest));

  RunUntilIdle();
  ASSERT_TRUE(handler_called);

  EXPECT_TRUE(bearer()->Reply(id, NewBuffer(kTestResponse)));

  // The transaction is marked as complete.
  EXPECT_FALSE(bearer()->Reply(id, NewBuffer(kTestResponse)));
  EXPECT_FALSE(bearer()->ReplyWithError(id, 0, ErrorCode::kUnlikelyError));

  RunUntilIdle();
  EXPECT_TRUE(response_sent);
}

TEST_F(BearerTest, IndicationConfirmation) {
  bool conf_sent = false;
  auto chan_cb = [&conf_sent](auto packet) {
    conf_sent = true;
    EXPECT_EQ(kConfirmation, (*packet)[0]);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  Bearer::TransactionId id;
  bool handler_called = false;
  auto handler = [&id, &handler_called](auto cb_id,
                                        const PacketReader& packet) {
    EXPECT_EQ(kIndication, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    handler_called = true;
    id = cb_id;
  };

  bearer()->RegisterHandler(kIndication, handler);
  fake_chan()->Receive(StaticByteBuffer(kIndication));

  RunUntilIdle();
  ASSERT_TRUE(handler_called);

  EXPECT_TRUE(bearer()->Reply(id, NewBuffer(kConfirmation)));

  // The transaction is marked as complete.
  EXPECT_FALSE(bearer()->Reply(id, NewBuffer(kConfirmation)));

  RunUntilIdle();
  EXPECT_TRUE(conf_sent);
}

TEST_F(BearerTest, ReplyWithErrorInvalidId) {
  EXPECT_FALSE(bearer()->ReplyWithError(0, 0, ErrorCode::kUnlikelyError));
}

TEST_F(BearerTest, IndicationReplyWithError) {
  Bearer::TransactionId id;
  bool handler_called = false;
  auto handler = [&id, &handler_called](auto cb_id,
                                        const PacketReader& packet) {
    EXPECT_EQ(kIndication, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    handler_called = true;
    id = cb_id;
  };

  bearer()->RegisterHandler(kIndication, handler);
  fake_chan()->Receive(StaticByteBuffer(kIndication));

  RunUntilIdle();
  ASSERT_TRUE(handler_called);

  // Cannot reply to an indication with error.
  EXPECT_FALSE(bearer()->ReplyWithError(id, 0, ErrorCode::kUnlikelyError));
}

TEST_F(BearerTest, ReplyWithError) {
  bool response_sent = false;
  auto chan_cb = [&response_sent](auto packet) {
    response_sent = true;

    // The error response that we send below
    StaticByteBuffer expected(
        kErrorResponse, kTestRequest, 0x00, 0x00, ErrorCode::kUnlikelyError);
    EXPECT_TRUE(ContainersEqual(expected, *packet));
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  Bearer::TransactionId id;
  bool handler_called = false;
  auto handler = [&id, &handler_called](auto cb_id,
                                        const PacketReader& packet) {
    EXPECT_EQ(kTestRequest, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    handler_called = true;
    id = cb_id;
  };

  bearer()->RegisterHandler(kTestRequest, handler);
  fake_chan()->Receive(StaticByteBuffer(kTestRequest));

  RunUntilIdle();
  ASSERT_TRUE(handler_called);

  EXPECT_TRUE(bearer()->ReplyWithError(id, 0, ErrorCode::kUnlikelyError));

  // The transaction is marked as complete.
  EXPECT_FALSE(bearer()->Reply(id, NewBuffer(kTestResponse)));
  EXPECT_FALSE(bearer()->ReplyWithError(id, 0, ErrorCode::kUnlikelyError));

  RunUntilIdle();
  EXPECT_TRUE(response_sent);
}

// Requests and indications have independent flow control
TEST_F(BearerTest, RequestAndIndication) {
  Bearer::TransactionId req_id, ind_id;

  int req_count = 0;
  int ind_count = 0;
  auto req_handler = [&req_id, &req_count](auto id, const auto& packet) {
    EXPECT_EQ(kTestRequest, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    req_count++;
    req_id = id;
  };
  auto ind_handler = [&ind_id, &ind_count](auto id, const auto& packet) {
    EXPECT_EQ(kIndication, packet.opcode());
    EXPECT_EQ(0u, packet.payload_size());

    ind_count++;
    ind_id = id;
  };

  bearer()->RegisterHandler(kTestRequest, req_handler);
  bearer()->RegisterHandler(kIndication, ind_handler);

  fake_chan()->Receive(StaticByteBuffer(kTestRequest));
  fake_chan()->Receive(StaticByteBuffer(kIndication));

  RunUntilIdle();
  EXPECT_EQ(1, req_count);
  ASSERT_EQ(1, ind_count);

  // Opcodes for the wrong transaction should be rejected.
  EXPECT_FALSE(bearer()->Reply(ind_id, NewBuffer(kTestResponse)));
  EXPECT_FALSE(bearer()->Reply(req_id, NewBuffer(kConfirmation)));

  // It should be possible to end two distinct transactions.
  MutableByteBufferPtr reply_0 = NewBuffer(kTestResponse);
  EXPECT_PACKET_OUT(*reply_0);
  EXPECT_TRUE(bearer()->Reply(req_id, std::move(reply_0)));
  MutableByteBufferPtr reply_1 = NewBuffer(kConfirmation);
  EXPECT_PACKET_OUT(*reply_1);
  EXPECT_TRUE(bearer()->Reply(ind_id, std::move(reply_1)));
}

// Test receipt of non-transactional PDUs.
TEST_F(BearerTest, RemotePDUWithoutResponse) {
  int cmd_count = 0;
  auto cmd_handler = [&cmd_count](auto tid, const auto& packet) {
    EXPECT_EQ(Bearer::kInvalidTransactionId, tid);
    EXPECT_EQ(kWriteCommand, packet.opcode());
    cmd_count++;
  };
  bearer()->RegisterHandler(kWriteCommand, cmd_handler);

  int not_count = 0;
  auto not_handler = [&not_count](auto tid, const auto& packet) {
    EXPECT_EQ(Bearer::kInvalidTransactionId, tid);
    EXPECT_EQ(kNotification, packet.opcode());
    not_count++;
  };
  bearer()->RegisterHandler(kNotification, not_handler);

  fake_chan()->Receive(StaticByteBuffer(kTestCommand));
  fake_chan()->Receive(StaticByteBuffer(kNotification));

  RunUntilIdle();
  EXPECT_EQ(1, cmd_count);
  EXPECT_EQ(1, not_count);
}

// Verify proper operation if a transaction callback frees the bearer
TEST_F(BearerTest, ResponseShutsDownBearer) {
  StaticByteBuffer response(kTestResponse, 'T', 'e', 's', 't');

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &response](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(response);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool cb_called = false;
  auto cb = [this, &cb_called, &response](Bearer::TransactionResult result) {
    ASSERT_FALSE(cb_called);
    if (result.is_error()) {
      return;
    }

    cb_called = true;
    EXPECT_TRUE(ContainersEqual(response, result.value().data()));
    DeleteBearer();
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(chan_cb_called);
  EXPECT_TRUE(cb_called);
}

// Verify proper operation if a transaction error callback frees the bearer
TEST_F(BearerTest, ErrorResponseShutsDownBearer) {
  StaticByteBuffer error_rsp(
      // Opcode: error response
      kErrorResponse,

      // request opcode
      kTestRequest,

      // handle (0x0001)
      0x01,
      0x00,

      // error code:
      ErrorCode::kRequestNotSupported);

  bool chan_cb_called = false;
  auto chan_cb = [this, &chan_cb_called, &error_rsp](auto cb_packet) {
    ASSERT_FALSE(chan_cb_called);
    EXPECT_EQ(kTestRequest, (*cb_packet)[0]);

    chan_cb_called = true;
    fake_chan()->Receive(error_rsp);
  };
  fake_chan()->SetSendCallback(chan_cb, dispatcher());

  bool err_cb_called = false;
  auto cb = [this, &err_cb_called](Bearer::TransactionResult result) {
    if (result.is_ok()) {
      return;
    }
    const auto& [error, handle] = result.error_value();
    EXPECT_EQ(Error(ErrorCode::kRequestNotSupported), error);
    EXPECT_EQ(0x0001, handle);

    err_cb_called = true;
    DeleteBearer();
  };
  bearer()->StartTransaction(NewBuffer(kTestRequest), cb);

  RunUntilIdle();
  EXPECT_TRUE(err_cb_called);
  EXPECT_TRUE(chan_cb_called);
}

class BearerTestSecurity : public BearerTest {
 protected:
  void SetUp() override {
    BearerTest::SetUp();

    fake_chan()->SetSecurityCallback(
        [this](hci_spec::ConnectionHandle,
               sm::SecurityLevel level,
               sm::ResultFunction<> callback) {
          security_request_count_++;
          requested_security_level_ = level;

          ASSERT_FALSE(security_responder_)
              << "Security request received while one was pending";
          security_responder_ = std::move(callback);
        },
        dispatcher());
  }

  // Sets up the fake channel to send an error response to all packets it
  // receives.
  void SetUpErrorResponder(ErrorCode ecode, Handle handle = 1) {
    fake_chan()->SetSendCallback(
        [this, ecode, handle](auto) {
          att_request_count_++;
          fake_chan()->Receive(StaticByteBuffer(
              kErrorResponse,  // opcode (Error Response)
              kTestRequest,    // request opcode
              LowerBits(handle),
              UpperBits(handle),  // handle
              ecode               // error code
              ));
        },
        dispatcher());
  }

  // Sets up the fake channel to respond with the given |response| opcode to all
  // requests that it receives. The PDU contains no additional payload as it is
  // not needed for this test fixture.
  void SetUpResponder() {
    fake_chan()->SetSendCallback(
        [this](auto) {
          att_request_count_++;
          fake_chan()->Receive(StaticByteBuffer(kTestResponse));
        },
        dispatcher());
  }

  // Resolves the currently pending security request.
  void ResolvePendingSecurityRequest(sm::Result<> status) {
    ASSERT_TRUE(security_responder_);

    if (status.is_ok()) {
      fake_chan()->set_security(sm::SecurityProperties(
          requested_security_level_, 16, /*secure_connections=*/false));
    }

    // Clear the responder before invoking it.
    auto f = std::move(security_responder_);
    f(status);
  }

  void SendRequest() {
    bearer()->StartTransaction(NewBuffer(kTestRequest), [this](auto result) {
      if (result.is_ok()) {
        request_success_count_++;
        last_request_status_ = fit::ok();
      } else {
        request_error_count_++;
        last_request_status_ = fit::error(result.error_value().first);
      }
    });
  }

  const Result<>& last_request_status() const { return last_request_status_; }
  size_t request_success_count() const { return request_success_count_; }
  size_t request_error_count() const { return request_error_count_; }
  size_t att_request_count() const { return att_request_count_; }

  size_t security_request_count() const { return security_request_count_; }
  sm::SecurityLevel requested_security_level() const {
    return requested_security_level_;
  }

 private:
  Result<> last_request_status_ = fit::ok();
  size_t request_success_count_ = 0u;
  size_t request_error_count_ = 0u;

  size_t att_request_count_ = 0u;
  sm::SecurityLevel requested_security_level_ = sm::SecurityLevel::kNoSecurity;
  size_t security_request_count_ = 0u;

  sm::ResultFunction<> security_responder_;
};

TEST_F(BearerTestSecurity, SecurityUpgradeAfterInsufficientAuthentication) {
  // Configure the endpoint to respond with an authentication error.
  SetUpErrorResponder(ErrorCode::kInsufficientAuthentication);
  SendRequest();
  RunUntilIdle();

  // At this stage the remote device should have received the request and
  // responded with "Insufficient Authentication". Since the link was not
  // encrypted, the Bearer should have requested a security upgrade without
  // requiring MITM protection.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(sm::SecurityLevel::kEncrypted, requested_security_level());

  // The request should be still unresolved.
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(0u, request_error_count());

  // Configure the endpoint to respond with success and notify the Bearer of the
  // security upgrade. The Bearer should re-send the request.
  SetUpResponder();
  ResolvePendingSecurityRequest(fit::ok());
  RunUntilIdle();

  // We should have received the same request again.
  EXPECT_EQ(2u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(1u, request_success_count());
  EXPECT_EQ(0u, request_error_count());
}

TEST_F(BearerTestSecurity,
       SecurityUpgradeWithMitmAfterInsufficientAuthentication) {
  // Configure the channel to be already encrypted.
  fake_chan()->set_security(sm::SecurityProperties(
      sm::SecurityLevel::kEncrypted, 16, /*secure_connections=*/false));

  // Configure the endpoint to respond with an authentication error.
  SetUpErrorResponder(ErrorCode::kInsufficientAuthentication);
  SendRequest();
  RunUntilIdle();

  // At this stage the remote device should have received the request and
  // responded with "Insufficient Authentication". Since the link was already
  // encrypted, the Bearer should have requested a security upgrade with MITM
  // protection.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(sm::SecurityLevel::kAuthenticated, requested_security_level());

  // The request should be still unresolved.
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(0u, request_error_count());

  // Configure the endpoint to respond with success and notify the Bearer of the
  // security upgrade. The Bearer should re-send the request.
  SetUpResponder();
  ResolvePendingSecurityRequest(fit::ok());
  RunUntilIdle();

  // We should have received the same request again.
  EXPECT_EQ(2u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(1u, request_success_count());
  EXPECT_EQ(0u, request_error_count());
  EXPECT_EQ(fit::ok(), last_request_status());
}

TEST_F(BearerTestSecurity, SecurityUpgradeFailsAfterAuthError) {
  // Configure the endpoint to respond with an authentication error.
  SetUpErrorResponder(ErrorCode::kInsufficientAuthentication);
  SendRequest();
  RunUntilIdle();

  // At this stage the remote device should have received the request and
  // responded with "Insufficient Authentication". Since the link was not
  // encrypted, the Bearer should have requested a security upgrade without
  // requiring MITM protection.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(sm::SecurityLevel::kEncrypted, requested_security_level());

  // The request should be still unresolved.
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(0u, request_error_count());

  // Configure the endpoint to respond with success and notify the Bearer of the
  // security upgrade. The Bearer should re-send the request.
  SetUpResponder();
  ResolvePendingSecurityRequest(ToResult(HostError::kFailed));
  RunUntilIdle();

  // The request should not have been retried and failed with the original
  // error.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(1u, request_error_count());
  EXPECT_EQ(ToResult(ErrorCode::kInsufficientAuthentication),
            last_request_status());
}

TEST_F(BearerTestSecurity, NoSecurityUpgradeIfAlreadyRetried) {
  // Configure the endpoint to respond with an authentication error.
  SetUpErrorResponder(ErrorCode::kInsufficientAuthentication);
  SendRequest();
  RunUntilIdle();

  // At this stage the remote device should have received the request and
  // responded with "Insufficient Authentication". Since the link was not
  // encrypted, the Bearer should have requested a security upgrade without
  // requiring MITM protection.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(1u, security_request_count());
  EXPECT_EQ(sm::SecurityLevel::kEncrypted, requested_security_level());

  // The request should be still unresolved.
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(0u, request_error_count());

  // Resolve the pending security request with success while the channel is
  // configured to respond with "Insufficient Authentication". The Bearer should
  // re-send the request.
  ResolvePendingSecurityRequest(fit::ok());
  RunUntilIdle();

  // The request should have been retried once. The "Insufficient
  // Authentication" error received while the link is encrypted should result in
  // a second security request at the next security level.
  EXPECT_EQ(2u, att_request_count());
  EXPECT_EQ(2u, security_request_count());
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(0u, request_error_count());

  // Resolve the pending security request with success while the channel is
  // configured to respond with "Insufficient Authentication. The Bearer should
  // retry the request one last time.
  ResolvePendingSecurityRequest(fit::ok());
  RunUntilIdle();

  // The request should have failed without retrying the request a third time as
  // the highest security level has been reached.
  EXPECT_EQ(3u, att_request_count());
  EXPECT_EQ(2u, security_request_count());
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(1u, request_error_count());
  EXPECT_EQ(ToResult(ErrorCode::kInsufficientAuthentication),
            last_request_status());
}

TEST_F(BearerTestSecurity, NoSecurityUpgradeIfChannelAlreadyEncrypted) {
  // Configure the channel to be already encrypted.
  fake_chan()->set_security(sm::SecurityProperties(
      sm::SecurityLevel::kEncrypted, 16, /*secure_connections=*/false));

  // Configure the endpoint to respond with an encryption error.
  SetUpErrorResponder(ErrorCode::kInsufficientEncryption);
  SendRequest();
  RunUntilIdle();

  // No security upgrade should have been requested since the channel was
  // sufficiently encrypted.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(0u, security_request_count());
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(1u, request_error_count());
  EXPECT_EQ(ToResult(ErrorCode::kInsufficientEncryption),
            last_request_status());
}

TEST_F(BearerTestSecurity, NoSecurityUpgradeIfChannelAlreadyEncryptedWithMitm) {
  // Configure the channel to be already encrypted with MITM protection
  fake_chan()->set_security(sm::SecurityProperties(
      sm::SecurityLevel::kAuthenticated, 16, /*secure_connections=*/false));

  // Configure the endpoint to respond with an authentication error.
  SetUpErrorResponder(ErrorCode::kInsufficientAuthentication);
  SendRequest();
  RunUntilIdle();

  // No security upgrade should have been requested since the channel was
  // sufficiently encrypted.
  EXPECT_EQ(1u, att_request_count());
  EXPECT_EQ(0u, security_request_count());
  EXPECT_EQ(0u, request_success_count());
  EXPECT_EQ(1u, request_error_count());
  EXPECT_EQ(ToResult(ErrorCode::kInsufficientAuthentication),
            last_request_status());
}

}  // namespace
}  // namespace bt::att
