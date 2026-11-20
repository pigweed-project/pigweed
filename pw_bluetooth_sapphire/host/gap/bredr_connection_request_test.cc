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

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_request.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pw_async/fake_dispatcher_fixture.h>
#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_chrono/system_clock.h>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;

const DeviceAddress kTestAddr(DeviceAddress::Type::kBREDR, {1});
const PeerId kPeerId;
constexpr hci::Error RetryableError =
    ToResult(pw::bluetooth::emboss::StatusCode::PAGE_TIMEOUT).error_value();
const StaticByteBuffer kCreateConnectionRsp(
    hci_spec::kCommandStatusEventCode,  // event code
    0x04,
    pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
    0xF0,                                        // num_hci_command_packets
    LowerBits(hci_spec::kCreateConnection),
    UpperBits(hci_spec::kCreateConnection));

using BrEdrConnectionRequestTests = pw::async::test::FakeDispatcherFixture;

TEST_F(BrEdrConnectionRequestTests, IncomingRequestStatusTracked) {
  // A freshly created request is not yet incoming
  auto req = BrEdrConnectionRequest(dispatcher(),
                                    kTestAddr,
                                    kPeerId,
                                    Peer::InitializingConnectionToken([] {}));
  EXPECT_FALSE(req.HasIncoming());

  req.BeginIncoming();
  // We should now have an incoming request, but still not an outgoing request
  EXPECT_TRUE(req.HasIncoming());
  EXPECT_FALSE(req.HasOutgoing());

  // A completed request is no longer incoming
  req.CompleteIncoming();
  EXPECT_FALSE(req.HasIncoming());
}

TEST_F(BrEdrConnectionRequestTests, CallbacksExecuted) {
  bool callback_called = false;
  bool token_destroyed = false;
  auto req = BrEdrConnectionRequest(
      dispatcher(),
      kTestAddr,
      kPeerId,
      Peer::InitializingConnectionToken(
          [&token_destroyed] { token_destroyed = true; }),
      [&callback_called](auto, auto) { callback_called = true; });

  // A freshly created request with a callback has an outgoing request
  EXPECT_TRUE(req.HasOutgoing());
  // Notifying callbacks triggers the callback
  req.NotifyCallbacks(fit::ok(), [&]() {
    EXPECT_TRUE(token_destroyed);
    return nullptr;
  });
  EXPECT_TRUE(token_destroyed);
  EXPECT_TRUE(callback_called);
}

#ifndef NINSPECT
TEST_F(BrEdrConnectionRequestTests, Inspect) {
  // inspector must outlive request
  inspect::Inspector inspector;
  BrEdrConnectionRequest req(dispatcher(),
                             kTestAddr,
                             kPeerId,
                             Peer::InitializingConnectionToken([] {}),
                             [](auto, auto) {});
  req.BeginIncoming();
  req.AttachInspect(inspector.GetRoot(), "request_name");

  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  EXPECT_THAT(
      hierarchy.value(),
      ChildrenMatch(ElementsAre(NodeMatches(AllOf(
          NameMatches("request_name"),
          PropertyList(UnorderedElementsAre(
              StringIs("peer_id", kPeerId.ToString()),
              UintIs("callbacks", 1u),
              BoolIs("has_incoming", true),
              IntIs("first_create_connection_request_timestamp", -1))))))));
}
#endif  // NINSPECT

using TestingBase =
    testing::FakeDispatcherControllerTest<testing::MockController>;
class BrEdrConnectionRequestLoopTest : public TestingBase {
 protected:
  BrEdrConnectionRequestLoopTest()
      : req_(dispatcher(),
             kTestAddr,
             kPeerId,
             Peer::InitializingConnectionToken([] {}),
             [this](hci::Result<> res, BrEdrConnection* conn) {
               if (handler_) {
                 handler_(res, conn);
               }
             }) {
    TestingBase::SetUp();

    // By default, an outbound ConnectionRequest with a complete handler that
    // just logs the result.
    handler_ = [](hci::Result<> res, auto /*ignore*/) {
      bt_log(INFO,
             "gap-bredr-test",
             "outbound connection request complete: %s",
             bt_str(res));
    };
  }

  BrEdrConnectionRequest& connection_req() { return req_; }

 private:
  BrEdrConnectionRequest req_;
  BrEdrConnectionRequest::OnComplete handler_;
};

TEST_F(BrEdrConnectionRequestLoopTest, CreateHciConnectionRequest) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::CreateConnectionPacket(kTestAddr),
                        &kCreateConnectionRsp);

  bool failure = false;
  auto failure_cb = [&failure](hci::Result<> result, PeerId) {
    if (result.is_error()) {
      failure = true;
    }
  };

  connection_req().CreateHciConnectionRequest(
      cmd_channel(),
      /*clock_offset=*/std::nullopt,
      /*page_scan_repetition_mode=*/std::nullopt,
      /*timeout_cb=*/[]() {},
      std::move(failure_cb),
      dispatcher());

  RunUntilIdle();

  ASSERT_FALSE(failure);
}

TEST_F(BrEdrConnectionRequestLoopTest,
       ShouldntRetryBeforeFirstCreateConnection) {
  EXPECT_FALSE(connection_req().ShouldRetry(RetryableError));
}
TEST_F(BrEdrConnectionRequestLoopTest,
       RetryableErrorCodeShouldRetryAfterFirstCreateConnection) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::CreateConnectionPacket(kTestAddr),
                        &kCreateConnectionRsp);

  bool failure = false;
  auto failure_cb = [&failure](hci::Result<> result, PeerId) {
    if (result.is_error()) {
      failure = true;
    }
  };

  connection_req().CreateHciConnectionRequest(
      cmd_channel(),
      /*clock_offset=*/std::nullopt,
      /*page_scan_repetition_mode=*/std::nullopt,
      /*timeout_cb=*/[]() {},
      std::move(failure_cb),
      dispatcher());

  RunFor(std::chrono::seconds(1));
  ASSERT_FALSE(failure);
  EXPECT_TRUE(connection_req().ShouldRetry(RetryableError));
}

TEST_F(BrEdrConnectionRequestLoopTest, ShouldntRetryWithNonRetriableErrorCode) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::CreateConnectionPacket(kTestAddr),
                        &kCreateConnectionRsp);

  bool failure = false;
  auto failure_cb = [&failure](hci::Result<> result, PeerId) {
    if (result.is_error()) {
      failure = true;
    }
  };

  connection_req().CreateHciConnectionRequest(
      cmd_channel(),
      /*clock_offset=*/std::nullopt,
      /*page_scan_repetition_mode=*/std::nullopt,
      /*timeout_cb=*/[]() {},
      std::move(failure_cb),
      dispatcher());

  RunFor(std::chrono::seconds(1));
  ASSERT_FALSE(failure);
  EXPECT_FALSE(connection_req().ShouldRetry(hci::Error(HostError::kCanceled)));
}

TEST_F(BrEdrConnectionRequestLoopTest, ShouldntRetryAfterThirtySeconds) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::CreateConnectionPacket(kTestAddr),
                        &kCreateConnectionRsp);

  bool failure = false;
  auto failure_cb = [&failure](hci::Result<> result, PeerId) {
    if (result.is_error()) {
      failure = true;
    }
  };

  connection_req().CreateHciConnectionRequest(
      cmd_channel(),
      /*clock_offset=*/std::nullopt,
      /*page_scan_repetition_mode=*/std::nullopt,
      /*timeout_cb=*/[]() {},
      std::move(failure_cb),
      dispatcher());
  RunFor(std::chrono::seconds(15));
  ASSERT_FALSE(failure);
  // Should be OK to retry after 15 seconds
  EXPECT_TRUE(connection_req().ShouldRetry(RetryableError));

  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::CreateConnectionPacket(kTestAddr),
                        &kCreateConnectionRsp);
  connection_req().CreateHciConnectionRequest(
      cmd_channel(),
      /*clock_offset=*/std::nullopt,
      /*page_scan_repetition_mode=*/std::nullopt,
      /*timeout_cb=*/[]() {},
      std::move(failure_cb),
      dispatcher());

  // Should still be OK to retry, even though we've already retried
  RunFor(std::chrono::seconds(14));
  EXPECT_TRUE(connection_req().ShouldRetry(RetryableError));

  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::CreateConnectionPacket(kTestAddr),
                        &kCreateConnectionRsp);
  connection_req().CreateHciConnectionRequest(
      cmd_channel(),
      /*clock_offset=*/std::nullopt,
      /*page_scan_repetition_mode=*/std::nullopt,
      /*timeout_cb=*/[]() {},
      std::move(failure_cb),
      dispatcher());

  RunFor(std::chrono::seconds(1));
  EXPECT_FALSE(connection_req().ShouldRetry(RetryableError));
}

}  // namespace
}  // namespace bt::gap
