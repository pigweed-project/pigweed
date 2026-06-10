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

#include "pw_rpc/synchronous_call.h"

#include <chrono>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_chrono/system_clock.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/raw/fake_channel_output.h"
#include "pw_rpc_test_protos/test.raw_rpc.pb.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"
#include "pw_work_queue/test_thread.h"
#include "pw_work_queue/work_queue.h"

namespace pw::rpc::test {
namespace {

using ::pw::rpc::test::pw_rpc::raw::TestService;
using MethodInfo = internal::MethodInfo<TestService::TestUnaryRpc>;

class RawSynchronousCallTest : public ::testing::Test {
 public:
  RawSynchronousCallTest()
      : channels_({{Channel::Create<42>(&fake_output_)}}), client_(channels_) {}

  void SetUp() override {
    work_thread_ =
        Thread(work_queue::test::WorkQueueThreadOptions(), work_queue_);
  }

  void TearDown() override {
    work_queue_.RequestStop();
#if PW_THREAD_JOINING_ENABLED
    work_thread_.join();
#else
    work_thread_.detach();
#endif  // PW_THREAD_JOINING_ENABLED
  }

 protected:
  void OnSend(span<const std::byte> buffer, Status status) {
    if (!status.ok()) {
      return;
    }
    auto result = internal::Packet::FromBuffer(buffer);
    EXPECT_TRUE(result.ok());
    request_packet_ = *result;

    EXPECT_TRUE(work_queue_.PushWork([this]() { SendResponse(); }).ok());
  }

  void SendResponse() {
    std::array<std::byte, 256> buffer;
    std::array<char, 32> payload_buffer;

    PW_CHECK_UINT_LE(response_.size(), payload_buffer.size());
    size_t size = response_.copy(payload_buffer.data(), payload_buffer.size());

    auto response =
        internal::Packet::Response(request_packet_, response_status_);
    response.set_payload(as_bytes(span(payload_buffer.data(), size)));
    EXPECT_TRUE(client_.ProcessPacket(response.Encode(buffer).value()).ok());
  }

  void set_response(std::string_view response,
                    Status response_status = OkStatus()) {
    response_ = response;
    response_status_ = response_status;
    output().set_on_send([this](span<const std::byte> buffer, Status status) {
      OnSend(buffer, status);
    });
  }

  MethodInfo::GeneratedClient generated_client() {
    return MethodInfo::GeneratedClient(client(), channel().id());
  }

  RawFakeChannelOutput<2>& output() { return fake_output_; }
  const Channel& channel() const { return channels_.front(); }
  Client& client() { return client_; }

 private:
  RawFakeChannelOutput<2> fake_output_;
  std::array<Channel, 1> channels_;
  Client client_;
  Thread work_thread_;
  work_queue::WorkQueueWithBuffer<1> work_queue_;
  std::string_view response_;
  Status response_status_ = OkStatus();
  internal::Packet request_packet_;
};

template <Status::Code kExpectedStatus = OkStatus().code()>
auto CopyReply(InlineString<32>& reply) {
  return [&reply](ConstByteSpan response, Status status) {
    EXPECT_EQ(Status(kExpectedStatus), status);
    reply.assign(reinterpret_cast<const char*>(response.data()),
                 response.size());
  };
}

void ExpectNoReply(ConstByteSpan, Status) { FAIL(); }

TEST_F(RawSynchronousCallTest, SynchronousCallSuccess) {
  set_response("jicama", OkStatus());

  InlineString<32> reply;
  ASSERT_EQ(OkStatus(),
            SynchronousCall<TestService::TestUnaryRpc>(
                client(), channel().id(), {}, CopyReply(reply)));
  EXPECT_EQ("jicama", reply);
}

TEST_F(RawSynchronousCallTest, SynchronousCallServerError) {
  set_response("raddish", Status::Internal());

  InlineString<32> reply;
  ASSERT_EQ(OkStatus(),
            SynchronousCall<TestService::TestUnaryRpc>(
                client(),
                channel().id(),
                {},
                CopyReply<Status::Internal().code()>(reply)));
  // We should still receive the response
  EXPECT_EQ("raddish", reply);
}

TEST_F(RawSynchronousCallTest, SynchronousCallRpcError) {
  // Internally, if Channel receives a non-ok status from the
  // ChannelOutput::Send, it will always return Unknown.
  output().set_send_status(Status::Unknown());

  EXPECT_EQ(Status::Unknown(),
            SynchronousCall<TestService::TestUnaryRpc>(
                client(), channel().id(), {}, ExpectNoReply));
}

TEST_F(RawSynchronousCallTest, SynchronousCallFor) {
  set_response("broccoli", Status::NotFound());

  InlineString<32> reply;
  const auto on_completed = [&reply](ConstByteSpan response, Status status) {
    EXPECT_EQ(Status::NotFound(), status);
    reply.assign(reinterpret_cast<const char*>(response.data()),
                 response.size());
  };
  ASSERT_EQ(OkStatus(),
            SynchronousCallFor<TestService::TestUnaryRpc>(
                client(),
                channel().id(),
                {},
                chrono::SystemClock::for_at_least(std::chrono::seconds(1)),
                on_completed));
  EXPECT_EQ("broccoli", reply);
}

TEST_F(RawSynchronousCallTest, SynchronousCallForTimeoutError) {
  ASSERT_EQ(Status::DeadlineExceeded(),
            SynchronousCallFor<TestService::TestUnaryRpc>(
                client(),
                channel().id(),
                {},
                chrono::SystemClock::for_at_least(std::chrono::milliseconds(1)),
                ExpectNoReply));
}

TEST_F(RawSynchronousCallTest, SynchronousCallUntilTimeoutError) {
  EXPECT_EQ(Status::DeadlineExceeded(),
            SynchronousCallUntil<TestService::TestUnaryRpc>(
                client(),
                channel().id(),
                {},
                chrono::SystemClock::now(),
                ExpectNoReply));
}

TEST_F(RawSynchronousCallTest, GeneratedClientSynchronousCallSuccess) {
  set_response("lettuce", OkStatus());

  InlineString<32> reply;
  EXPECT_EQ(OkStatus(),
            SynchronousCall<TestService::TestUnaryRpc>(
                generated_client(), {}, CopyReply(reply)));
  EXPECT_EQ("lettuce", reply);
}

TEST_F(RawSynchronousCallTest, GeneratedClientSynchronousCallServerError) {
  set_response("cabbage", Status::Internal());

  InlineString<32> reply;
  EXPECT_EQ(
      OkStatus(),
      SynchronousCall<TestService::TestUnaryRpc>(
          generated_client(), {}, CopyReply<Status::Internal().code()>(reply)));
  EXPECT_EQ("cabbage", reply);
}

TEST_F(RawSynchronousCallTest, GeneratedClientSynchronousCallRpcError) {
  output().set_send_status(Status::Unknown());

  EXPECT_EQ(Status::Unknown(),
            SynchronousCall<TestService::TestUnaryRpc>(
                generated_client(), {}, ExpectNoReply));
}

TEST_F(RawSynchronousCallTest, GeneratedClientSynchronousCallForTimeoutError) {
  EXPECT_EQ(Status::DeadlineExceeded(),
            SynchronousCallFor<TestService::TestUnaryRpc>(
                generated_client(),
                {},
                chrono::SystemClock::for_at_least(std::chrono::milliseconds(1)),
                ExpectNoReply));
}

TEST_F(RawSynchronousCallTest,
       GeneratedClientSynchronousCallUntilTimeoutError) {
  EXPECT_EQ(
      Status::DeadlineExceeded(),
      SynchronousCallUntil<TestService::TestUnaryRpc>(
          generated_client(), {}, chrono::SystemClock::now(), ExpectNoReply));
}

}  // namespace
}  // namespace pw::rpc::test
