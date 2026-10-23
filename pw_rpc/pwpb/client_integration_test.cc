// Copyright 2022 The Pigweed Authors
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

#include "pw_assert/check.h"
#include "pw_rpc/benchmark.rpc.pwpb.h"
#include "pw_rpc/integration_testing.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_unit_test/framework.h"

namespace pwpb_rpc_test {
namespace {

using namespace std::chrono_literals;
using pw::ByteSpan;
using pw::ConstByteSpan;
using pw::Function;
using pw::OkStatus;
using pw::Status;

using pw::rpc::pw_rpc::pwpb::Benchmark;

constexpr int kIterations = 10;

class PayloadReceiver {
 public:
  const char* Wait() {
    constexpr auto kWaitTimeout = 1500ms;
    PW_CHECK(sem_.try_acquire_for(kWaitTimeout));
    return reinterpret_cast<const char*>(payload_.payload.data());
  }

  Function<void(const pw::rpc::pwpb::Payload::Message&, Status)>
  UnaryOnCompleted() {
    return [this](const pw::rpc::pwpb::Payload::Message& data, Status) {
      CopyPayload(data);
    };
  }

  Function<void(const pw::rpc::pwpb::Payload::Message&)> OnNext() {
    return [this](const pw::rpc::pwpb::Payload::Message& data) {
      CopyPayload(data);
    };
  }

 private:
  void CopyPayload(const pw::rpc::pwpb::Payload::Message& data) {
    payload_ = data;
    sem_.release();
  }

  pw::sync::BinarySemaphore sem_;
  pw::rpc::pwpb::Payload::Message payload_ = {};
};

template <size_t kSize>
pw::rpc::pwpb::Payload::Message Payload(const char (&string)[kSize]) {
  static_assert(kSize <= sizeof(pw::rpc::pwpb::Payload::Message::payload));
  pw::rpc::pwpb::Payload::Message payload{};
  payload.payload.resize(kSize);
  std::memcpy(payload.payload.data(), string, kSize);
  return payload;
}

const Benchmark::Client kClient(pw::rpc::integration_test::client(),
                                pw::rpc::integration_test::kChannelId);

TEST(PwpbRpcIntegrationTest, Unary) {
  char value[] = {"hello, world!"};

  for (int i = 0; i < kIterations; ++i) {
    PayloadReceiver receiver;

    value[0] = static_cast<char>(i);
    pw::rpc::PwpbUnaryReceiver call =
        kClient.UnaryEcho(Payload(value), receiver.UnaryOnCompleted());
    ASSERT_STREQ(receiver.Wait(), value);
  }
}

TEST(PwpbRpcIntegrationTest, Unary_ReuseCall) {
  pw::rpc::PwpbUnaryReceiver<pw::rpc::pwpb::Payload::Message> call;
  char value[] = {"O_o "};

  for (int i = 0; i < kIterations; ++i) {
    PayloadReceiver receiver;

    value[sizeof(value) - 2] = static_cast<char>(i);
    call = kClient.UnaryEcho(Payload(value), receiver.UnaryOnCompleted());
    ASSERT_STREQ(receiver.Wait(), value);
  }
}

TEST(PwpbRpcIntegrationTest, Unary_DiscardCalls) {
  constexpr int iterations = PW_RPC_USE_GLOBAL_MUTEX ? 10000 : 1;
  for (int i = 0; i < iterations; ++i) {
    kClient.UnaryEcho(Payload("O_o"));
  }
}

TEST(PwpbRpcIntegrationTest, BidirectionalStreaming_MoveCalls) {
  for (int i = 0; i < kIterations; ++i) {
    PayloadReceiver receiver;
    pw::rpc::PwpbClientReaderWriter call =
        kClient.BidirectionalEcho(receiver.OnNext());

    ASSERT_EQ(OkStatus(), call.Write(Payload("Yello")));
    ASSERT_STREQ(receiver.Wait(), "Yello");

    pw::rpc::PwpbClientReaderWriter<pw::rpc::pwpb::Payload::Message,
                                    pw::rpc::pwpb::Payload::Message>
        new_call = std::move(call);

    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_EQ(Status::FailedPrecondition(), call.Write(Payload("Dello")));

    ASSERT_EQ(OkStatus(), new_call.Write(Payload("Dello")));
    ASSERT_STREQ(receiver.Wait(), "Dello");

    call = std::move(new_call);

    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_EQ(Status::FailedPrecondition(), new_call.Write(Payload("Dello")));

    ASSERT_EQ(OkStatus(), call.Write(Payload("???")));
    ASSERT_STREQ(receiver.Wait(), "???");

    EXPECT_EQ(OkStatus(), call.Cancel());
    EXPECT_EQ(Status::FailedPrecondition(), new_call.Cancel());
  }
}

TEST(PwpbRpcIntegrationTest, BidirectionalStreaming_ReuseCall) {
  pw::rpc::PwpbClientReaderWriter<pw::rpc::pwpb::Payload::Message,
                                  pw::rpc::pwpb::Payload::Message>
      call;

  for (int i = 0; i < kIterations; ++i) {
    PayloadReceiver receiver;
    call = kClient.BidirectionalEcho(receiver.OnNext());

    ASSERT_EQ(OkStatus(), call.Write(Payload("Yello")));
    ASSERT_STREQ(receiver.Wait(), "Yello");

    ASSERT_EQ(OkStatus(), call.Write(Payload("Dello")));
    ASSERT_STREQ(receiver.Wait(), "Dello");

    ASSERT_EQ(OkStatus(), call.Write(Payload("???")));
    ASSERT_STREQ(receiver.Wait(), "???");
  }
}

}  // namespace
}  // namespace pwpb_rpc_test
