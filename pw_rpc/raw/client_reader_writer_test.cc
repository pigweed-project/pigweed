// Copyright 2021 The Pigweed Authors
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

#include "pw_rpc/raw/client_reader_writer.h"

#include <optional>

#include "pw_bytes/array.h"
#include "pw_rpc/dynamic_call.h"
#include "pw_rpc/raw/client_testing.h"
#include "pw_rpc/writer.h"
#include "pw_rpc_test_protos/test.raw_rpc.pb.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc {

#if PW_RPC_DYNAMIC_ALLOCATION
// Instantiate DynamicCallPtr for each call type to check that it compiles.
template class DynamicCallPtr<RawClientReaderWriter>;
template class DynamicCallPtr<RawClientReader>;
template class DynamicCallPtr<RawClientWriter>;
template class DynamicCallPtr<RawUnaryReceiver>;
#endif  // PW_RPC_DYNAMIC_ALLOCATION

namespace {

using test::pw_rpc::raw::TestService;

void FailIfCalled(Status) { FAIL(); }
void FailIfOnNextCalled(ConstByteSpan) { FAIL(); }
void FailIfOnCompletedCalled(ConstByteSpan, Status) { FAIL(); }

TEST(RawUnaryReceiver, DefaultConstructed) {
  RawUnaryReceiver call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());

  call.set_on_completed([](ConstByteSpan, Status) {});
  call.set_on_error([](Status) {});
}

TEST(RawClientWriter, DefaultConstructed) {
  RawClientWriter call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  call.set_on_completed([](ConstByteSpan, Status) {});
  call.set_on_error([](Status) {});
}

TEST(RawClientReader, DefaultConstructed) {
  RawClientReader call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  call.set_on_completed([](Status) {});
  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawClientReaderWriter, DefaultConstructed) {
  RawClientReaderWriter call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  call.set_on_completed([](Status) {});
  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawClientWriter, RequestCompletion) {
  RawClientTestContext ctx;
  RawClientWriter call = TestService::TestClientStreamRpc(
      ctx.client(), ctx.channel().id(), FailIfOnCompletedCalled, FailIfCalled);
  ASSERT_EQ(OkStatus(), call.RequestCompletion());

  ASSERT_TRUE(call.active());
  EXPECT_EQ(call.channel_id(), ctx.channel().id());

  EXPECT_EQ(OkStatus(), call.Write(ConstByteSpan()));
  EXPECT_EQ(OkStatus(), call.RequestCompletion());
  EXPECT_EQ(OkStatus(), call.Cancel());

  call.set_on_completed([](ConstByteSpan, Status) {});
  call.set_on_error([](Status) {});
}

TEST(RawClientReader, RequestCompletion) {
  RawClientTestContext ctx;
  RawClientReader call = TestService::TestServerStreamRpc(ctx.client(),
                                                          ctx.channel().id(),
                                                          {},
                                                          FailIfOnNextCalled,
                                                          FailIfCalled,
                                                          FailIfCalled);
  ASSERT_EQ(OkStatus(), call.RequestCompletion());

  ASSERT_TRUE(call.active());
  EXPECT_EQ(call.channel_id(), ctx.channel().id());

  EXPECT_EQ(OkStatus(), call.RequestCompletion());
  EXPECT_EQ(OkStatus(), call.Cancel());

  call.set_on_completed([](Status) {});
  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawClientReaderWriter, RequestCompletion) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);
  ASSERT_EQ(OkStatus(), call.RequestCompletion());

  ASSERT_TRUE(call.active());
  EXPECT_EQ(call.channel_id(), ctx.channel().id());

  EXPECT_EQ(OkStatus(), call.Write(ConstByteSpan()));
  EXPECT_EQ(OkStatus(), call.RequestCompletion());
  EXPECT_EQ(OkStatus(), call.Cancel());

  call.set_on_completed([](Status) {});
  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawUnaryReceiver, Cancel) {
  RawClientTestContext ctx;
  RawUnaryReceiver call = TestService::TestUnaryRpc(ctx.client(),
                                                    ctx.channel().id(),
                                                    {},
                                                    FailIfOnCompletedCalled,
                                                    FailIfCalled);
  ASSERT_EQ(OkStatus(), call.Cancel());

  // Additional calls should do nothing and return FAILED_PRECONDITION.
  ASSERT_EQ(Status::FailedPrecondition(), call.Cancel());
  ASSERT_EQ(Status::FailedPrecondition(), call.Cancel());

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());

  call.set_on_completed([](ConstByteSpan, Status) {});
  call.set_on_error([](Status) {});

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & cancellation only
}

TEST(RawClientWriter, Cancel) {
  RawClientTestContext ctx;
  RawClientWriter call = TestService::TestClientStreamRpc(
      ctx.client(), ctx.channel().id(), FailIfOnCompletedCalled, FailIfCalled);
  ASSERT_EQ(OkStatus(), call.Cancel());

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  call.set_on_completed([](ConstByteSpan, Status) {});
  call.set_on_error([](Status) {});

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & cancellation only
}

TEST(RawClientReader, Cancel) {
  RawClientTestContext ctx;
  RawClientReader call = TestService::TestServerStreamRpc(ctx.client(),
                                                          ctx.channel().id(),
                                                          {},
                                                          FailIfOnNextCalled,
                                                          FailIfCalled,
                                                          FailIfCalled);
  ASSERT_EQ(OkStatus(), call.Cancel());

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  call.set_on_completed([](Status) {});
  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & cancellation only
}

TEST(RawClientReaderWriter, Cancel) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);
  ASSERT_EQ(OkStatus(), call.Cancel());

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  call.set_on_completed([](Status) {});
  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & cancellation only
}

TEST(RawUnaryReceiver, Abandon) {
  RawClientTestContext ctx;
  RawUnaryReceiver call = TestService::TestUnaryRpc(ctx.client(),
                                                    ctx.channel().id(),
                                                    {},
                                                    FailIfOnCompletedCalled,
                                                    FailIfCalled);
  call.Abandon();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());

  EXPECT_EQ(ctx.output().total_packets(), 1u);  // request only
}

TEST(RawClientWriter, Abandon) {
  RawClientTestContext ctx;
  RawClientWriter call = TestService::TestClientStreamRpc(
      ctx.client(), ctx.channel().id(), FailIfOnCompletedCalled, FailIfCalled);
  call.Abandon();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & client stream end
}

TEST(RawClientReader, Abandon) {
  RawClientTestContext ctx;
  RawClientReader call = TestService::TestServerStreamRpc(ctx.client(),
                                                          ctx.channel().id(),
                                                          {},
                                                          FailIfOnNextCalled,
                                                          FailIfCalled,
                                                          FailIfCalled);
  call.Abandon();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  EXPECT_EQ(ctx.output().total_packets(), 1u);  // request only
}

TEST(RawClientReaderWriter, Abandon) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);
  call.Abandon();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & client stream end
}

TEST(RawUnaryReceiver, CloseAndWaitForCallbacks) {
  RawClientTestContext ctx;
  RawUnaryReceiver call = TestService::TestUnaryRpc(ctx.client(),
                                                    ctx.channel().id(),
                                                    {},
                                                    FailIfOnCompletedCalled,
                                                    FailIfCalled);
  call.CloseAndWaitForCallbacks();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());

  EXPECT_EQ(ctx.output().total_packets(), 1u);  // request only
}

TEST(RawClientWriter, CloseAndWaitForCallbacks) {
  RawClientTestContext ctx;
  RawClientWriter call = TestService::TestClientStreamRpc(
      ctx.client(), ctx.channel().id(), FailIfOnCompletedCalled, FailIfCalled);
  call.CloseAndWaitForCallbacks();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & client stream end
}

TEST(RawClientReader, CloseAndWaitForCallbacks) {
  RawClientTestContext ctx;
  RawClientReader call = TestService::TestServerStreamRpc(ctx.client(),
                                                          ctx.channel().id(),
                                                          {},
                                                          FailIfOnNextCalled,
                                                          FailIfCalled,
                                                          FailIfCalled);
  call.CloseAndWaitForCallbacks();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  EXPECT_EQ(ctx.output().total_packets(), 1u);  // request only
}

TEST(RawClientReaderWriter, CloseAndWaitForCallbacks) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);
  call.CloseAndWaitForCallbacks();

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Cancel());
  EXPECT_EQ(Status::FailedPrecondition(), call.RequestCompletion());

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & client stream end
}

TEST(RawClientReaderWriter, WriteCallback) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);

  constexpr auto kData = bytes::Initialized<16>(0x33);
  EXPECT_EQ(OkStatus(), call.Write([&kData](ByteSpan buffer) {
    std::memcpy(buffer.data(), kData.data(), kData.size());
    return StatusWithSize(kData.size());
  }));

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // request & written stream
  EXPECT_EQ(std::memcmp(ctx.output()
                            .payloads<TestService::TestBidirectionalStreamRpc>()
                            .back()
                            .data(),
                        kData.data(),
                        kData.size()),
            0);
}

TEST(RawClientReaderWriter, Move_InactiveToActive_EndsClientStream) {
  RawClientTestContext ctx;

  RawClientReaderWriter active_call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);

  ASSERT_EQ(ctx.output().total_packets(), 1u);  // Sent the request

  RawClientReaderWriter inactive_call;

  active_call = std::move(inactive_call);

  EXPECT_EQ(ctx.output().total_packets(),
            2u);  // Sent CLIENT_REQUEST_COMPLETION
  EXPECT_EQ(
      ctx.output()
          .client_stream_end_packets<TestService::TestBidirectionalStreamRpc>(),
      1u);

  EXPECT_FALSE(active_call.active());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(inactive_call.active());
}

TEST(RawUnaryReceiver, Move_InactiveToActive_SilentlyCloses) {
  RawClientTestContext ctx;

  RawUnaryReceiver active_call =
      TestService::TestUnaryRpc(ctx.client(),
                                ctx.channel().id(),
                                {},
                                FailIfOnCompletedCalled,
                                FailIfCalled);

  ASSERT_EQ(ctx.output().total_packets(), 1u);  // Sent the request

  RawUnaryReceiver inactive_call;

  active_call = std::move(inactive_call);

  EXPECT_EQ(ctx.output().total_packets(), 1u);  // No more packets

  EXPECT_FALSE(active_call.active());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(inactive_call.active());
}

TEST(RawUnaryReceiver, Move_ActiveToActive) {
  RawClientTestContext ctx;

  RawUnaryReceiver active_call_1 =
      TestService::TestUnaryRpc(ctx.client(), ctx.channel().id(), {});

  RawUnaryReceiver active_call_2 =
      TestService::TestAnotherUnaryRpc(ctx.client(), ctx.channel().id(), {});

  ASSERT_EQ(ctx.output().total_packets(), 2u);  // Sent the requests
  ASSERT_TRUE(active_call_1.active());
  ASSERT_TRUE(active_call_2.active());

  active_call_2 = std::move(active_call_1);

  EXPECT_EQ(ctx.output().total_packets(), 2u);  // No more packets

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(active_call_1.active());
  EXPECT_TRUE(active_call_2.active());
}

TEST(RawUnaryReceiver, InvalidChannelId) {
  RawClientTestContext ctx;
  std::optional<Status> error;

  RawUnaryReceiver call = TestService::TestUnaryRpc(
      ctx.client(), 1290341, {}, {}, [&error](Status status) {
        error = status;
      });
  EXPECT_FALSE(call.active());
  EXPECT_EQ(error, Status::Unavailable());
}

TEST(RawClientReader, NoClientStream_OutOfScope_SilentlyCloses) {
  RawClientTestContext ctx;

  {
    RawClientReader call = TestService::TestServerStreamRpc(ctx.client(),
                                                            ctx.channel().id(),
                                                            {},
                                                            FailIfOnNextCalled,
                                                            FailIfCalled,
                                                            FailIfCalled);
    ASSERT_EQ(ctx.output().total_packets(), 1u);  // Sent the request
  }

  EXPECT_EQ(ctx.output().total_packets(), 1u);  // No more packets
}

TEST(RawClientWriter, WithClientStream_OutOfScope_SendsClientStreamEnd) {
  RawClientTestContext ctx;

  {
    RawClientWriter call =
        TestService::TestClientStreamRpc(ctx.client(),
                                         ctx.channel().id(),
                                         FailIfOnCompletedCalled,
                                         FailIfCalled);
    ASSERT_EQ(ctx.output().total_packets(), 1u);  // Sent the request
  }

  EXPECT_EQ(ctx.output().total_packets(),
            2u);  // Sent CLIENT_REQUEST_COMPLETION
  EXPECT_EQ(ctx.output()
                .client_stream_end_packets<TestService::TestClientStreamRpc>(),
            1u);
}

constexpr const char kWriterData[] = "20X6";

void WriteAsWriter(Writer& writer) {
  ASSERT_TRUE(writer.active());
  ASSERT_EQ(writer.channel_id(), RawClientTestContext<>::kDefaultChannelId);

  EXPECT_EQ(OkStatus(), writer.Write(as_bytes(span(kWriterData))));
}

TEST(RawClientWriter, UsableAsWriter) {
  RawClientTestContext ctx;
  RawClientWriter call = TestService::TestClientStreamRpc(
      ctx.client(), ctx.channel().id(), FailIfOnCompletedCalled, FailIfCalled);

  WriteAsWriter(call.as_writer());

  EXPECT_STREQ(reinterpret_cast<const char*>(
                   ctx.output()
                       .payloads<TestService::TestClientStreamRpc>()
                       .back()
                       .data()),
               kWriterData);
}

TEST(RawClientReaderWriter, UsableAsWriter) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);

  WriteAsWriter(call.as_writer());

  EXPECT_STREQ(reinterpret_cast<const char*>(
                   ctx.output()
                       .payloads<TestService::TestBidirectionalStreamRpc>()
                       .back()
                       .data()),
               kWriterData);
}

const char* span_as_cstr(ConstByteSpan span) {
  return reinterpret_cast<const char*>(span.data());
}

TEST(RawClientReaderWriter,
     MultipleCallsToSameMethodOkAndReceiveSeparateResponses) {
  RawClientTestContext ctx;

  ConstByteSpan data_1 = as_bytes(span("data_1_unset"));
  ConstByteSpan data_2 = as_bytes(span("data_2_unset"));

  Status error;
  auto set_error = [&error](Status status) { error.Update(status); };
  RawClientReaderWriter active_call_1 = TestService::TestBidirectionalStreamRpc(
      ctx.client(),
      ctx.channel().id(),
      [&data_1](ConstByteSpan payload) { data_1 = payload; },
      FailIfCalled,
      set_error);

  EXPECT_TRUE(active_call_1.active());

  RawClientReaderWriter active_call_2 = TestService::TestBidirectionalStreamRpc(
      ctx.client(),
      ctx.channel().id(),
      [&data_2](ConstByteSpan payload) { data_2 = payload; },
      FailIfCalled,
      set_error);

  EXPECT_TRUE(active_call_1.active());
  EXPECT_TRUE(active_call_2.active());
  EXPECT_EQ(error, OkStatus());

  ConstByteSpan message_1 = as_bytes(span("hello_1"));
  ConstByteSpan message_2 = as_bytes(span("hello_2"));

  ctx.server().SendServerStream<TestService::TestBidirectionalStreamRpc>(
      message_2, active_call_2.id());
  EXPECT_STREQ(span_as_cstr(data_2), span_as_cstr(message_2));
  ctx.server().SendServerStream<TestService::TestBidirectionalStreamRpc>(
      message_1, active_call_1.id());
  EXPECT_STREQ(span_as_cstr(data_1), span_as_cstr(message_1));
}

TEST(RawUnaryReceiver, MaxSafePayloadSize_Inactive) {
  RawUnaryReceiver call;
  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

TEST(RawClientReader, MaxSafePayloadSize_NoChannel) {
  RawClientTestContext ctx;
  uint32_t channel_id = ctx.channel().id();
  PW_TEST_ASSERT_OK(ctx.client().CloseChannel(channel_id));

  // This test is similar to RawServerWriter.MaxSafePayloadSize_NoChannel, but
  // doesn't actually test having a call with an unknown channel ID. Client-side
  // Open() APIs do not yet exist, so it is not possible to have an open client
  // call with an unknown channel ID. The call below is never actually open.
  //
  // If Open() is added for client calls, this test could be updated to use it.
  RawClientReader call = TestService::TestServerStreamRpc(ctx.client(),
                                                          channel_id,
                                                          {},
                                                          FailIfOnNextCalled,
                                                          FailIfCalled,
                                                          [](Status) {});

  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

TEST(RawClientWriter, MaxSafePayloadSize_UnlimitedChannel) {
  RawClientTestContext ctx;
  RawClientWriter call = TestService::TestClientStreamRpc(
      ctx.client(), ctx.channel().id(), FailIfOnCompletedCalled, FailIfCalled);

  EXPECT_EQ(call.MaxWriteSizeBytes(), pw::rpc::MaxSafePayloadSize());

  PW_TEST_EXPECT_OK(call.Cancel());
  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

TEST(RawClientReaderWriter, MaxSafePayloadSize_LimitedChannel) {
  RawClientTestContext ctx;
  RawClientReaderWriter call =
      TestService::TestBidirectionalStreamRpc(ctx.client(),
                                              ctx.channel().id(),
                                              FailIfOnNextCalled,
                                              FailIfCalled,
                                              FailIfCalled);

  constexpr size_t kChannelMtu = 40;
  static_assert(kChannelMtu < pw::rpc::MaxSafePayloadSize());

  ctx.output().set_mtu(kChannelMtu);

  EXPECT_EQ(call.MaxWriteSizeBytes(), pw::rpc::MaxSafePayloadSize(kChannelMtu));

  PW_TEST_EXPECT_OK(call.Cancel());
  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

}  // namespace
}  // namespace pw::rpc
