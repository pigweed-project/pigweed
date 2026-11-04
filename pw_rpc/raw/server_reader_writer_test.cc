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

#include "pw_rpc/raw/server_reader_writer.h"

#include <optional>

#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_rpc/dynamic_call.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/raw/fake_channel_output.h"
#include "pw_rpc/service.h"
#include "pw_rpc/writer.h"
#include "pw_rpc_test_protos/test.raw_rpc.pb.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc {

#if PW_RPC_DYNAMIC_ALLOCATION
// Instantiate DynamicCallPtr for each call type to check that it compiles.
template class DynamicCallPtr<RawServerReaderWriter>;
template class DynamicCallPtr<RawServerReader>;
template class DynamicCallPtr<RawServerWriter>;
template class DynamicCallPtr<RawUnaryResponder>;
#endif  // PW_RPC_DYNAMIC_ALLOCATION

namespace {

class TestServiceImpl final
    : public test::pw_rpc::raw::TestService::Service<TestServiceImpl> {
 public:
  static void TestUnaryRpc(ConstByteSpan, RawUnaryResponder&) {}

  void TestAnotherUnaryRpc(ConstByteSpan, RawUnaryResponder&) {}

  void TestServerStreamRpc(ConstByteSpan, RawServerWriter&) {}

  void TestClientStreamRpc(RawServerReader&) {}

  void TestBidirectionalStreamRpc(RawServerReaderWriter&) {}
};

struct ReaderWriterTestContext {
  static constexpr uint32_t kChannelId = 1;

  ReaderWriterTestContext()
      : channel(Channel::Create<kChannelId>(&output)),
        server(span(&channel, 1)) {}

  TestServiceImpl service;
  RawFakeChannelOutput<4> output;
  Channel channel;
  Server server;
};

using test::pw_rpc::raw::TestService;

TEST(RawUnaryResponder, DefaultConstructed) {
  RawUnaryResponder call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Finish({}, OkStatus()));

  call.set_on_error([](Status) {});
}

TEST(RawServerWriter, DefaultConstructed) {
  RawServerWriter call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Finish(OkStatus()));

  call.set_on_error([](Status) {});
}

TEST(RawServerReader, DefaultConstructed) {
  RawServerReader call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Finish({}, OkStatus()));

  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawServerReaderWriter, DefaultConstructed) {
  RawServerReaderWriter call;

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Finish(Status::Cancelled()));

  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawUnaryResponder, Closed) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);
  ASSERT_EQ(OkStatus(), call.Finish({}, OkStatus()));

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Finish({}, OkStatus()));

  call.set_on_error([](Status) {});
}

TEST(RawUnaryResponder, TryCloseFailed) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish({}, OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());
}

TEST(RawUnaryResponder, TryCloseSuccessful) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish({}, OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());

  // Tries to close the call again, with ChannelOutput set to return ok.
  ctx.output.set_send_status(OkStatus());
  ASSERT_EQ(OkStatus(), call.TryFinish({}, OkStatus()));
  // Call should be closed.
  ASSERT_FALSE(call.active());
}

TEST(RawUnaryResponder, FinishCallback_Successful) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);

  constexpr auto kData = bytes::Initialized<8>(0xff);
  ASSERT_EQ(OkStatus(),
            call.FinishCallback(
                [&kData](ByteSpan buffer) {
                  std::memcpy(buffer.data(), kData.data(), kData.size());
                  return StatusWithSize(kData.size());
                },
                OkStatus()));

  EXPECT_EQ(std::memcmp(
                ctx.output.payloads<TestService::TestUnaryRpc>().back().data(),
                kData.data(),
                kData.size()),
            0);
  EXPECT_FALSE(call.active());
}

TEST(RawUnaryResponder, TryFinishCallback_Successful) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);

  constexpr auto kData = bytes::Initialized<8>(0xdd);
  ASSERT_EQ(OkStatus(),
            call.TryFinishCallback(
                [&kData](ByteSpan buffer) {
                  std::memcpy(buffer.data(), kData.data(), kData.size());
                  return StatusWithSize(kData.size());
                },
                OkStatus()));

  EXPECT_EQ(std::memcmp(
                ctx.output.payloads<TestService::TestUnaryRpc>().back().data(),
                kData.data(),
                kData.size()),
            0);
  EXPECT_FALSE(call.active());
}

TEST(RawUnaryResponder, TryFinishCallback_ChannelError) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);

  ctx.output.set_send_status(Status::Unknown());

  constexpr auto kData = bytes::Initialized<8>(0xdd);
  ASSERT_EQ(Status::Unknown(),
            call.TryFinishCallback(
                [&kData](ByteSpan buffer) {
                  std::memcpy(buffer.data(), kData.data(), kData.size());
                  return StatusWithSize(kData.size());
                },
                OkStatus()));

  // Call should remain active.
  EXPECT_TRUE(call.active());
}

TEST(RawServerWriter, Closed) {
  ReaderWriterTestContext ctx;
  RawServerWriter call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  ASSERT_EQ(OkStatus(), call.Finish(OkStatus()));

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Finish(OkStatus()));

  call.set_on_error([](Status) {});
}

TEST(RawServerWriter, TryCloseFailed) {
  ReaderWriterTestContext ctx;
  RawServerWriter call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish(OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());
}

TEST(RawServerWriter, TryCloseSuccessful) {
  ReaderWriterTestContext ctx;
  RawServerWriter call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish(OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());

  // Tries to close the call again, with ChannelOutput set to return ok.
  ctx.output.set_send_status(OkStatus());
  ASSERT_EQ(OkStatus(), call.TryFinish(OkStatus()));
  // Call should be closed.
  ASSERT_FALSE(call.active());
}

TEST(RawServerReader, Closed) {
  ReaderWriterTestContext ctx;
  RawServerReader call =
      RawServerReader::Open<TestService::TestClientStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  ASSERT_EQ(OkStatus(), call.Finish({}, OkStatus()));

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Finish({}, OkStatus()));

  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawServerReader, TryCloseFailed) {
  ReaderWriterTestContext ctx;
  RawServerReader call =
      RawServerReader::Open<TestService::TestClientStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish({}, OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());
}

TEST(RawServerReader, TryCloseSuccessful) {
  ReaderWriterTestContext ctx;
  RawServerReader call =
      RawServerReader::Open<TestService::TestClientStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish({}, OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());

  // Tries to close the call again, with ChannelOutput set to return ok.
  ctx.output.set_send_status(OkStatus());
  ASSERT_EQ(OkStatus(), call.TryFinish({}, OkStatus()));
  // Call should be closed.
  ASSERT_FALSE(call.active());
}

TEST(RawServerReaderWriter, Closed) {
  ReaderWriterTestContext ctx;
  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  ASSERT_EQ(OkStatus(), call.Finish(OkStatus()));

  ASSERT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);

  EXPECT_EQ(Status::FailedPrecondition(), call.Write(ConstByteSpan()));
  EXPECT_EQ(Status::FailedPrecondition(), call.Finish(Status::Cancelled()));

  call.set_on_next([](ConstByteSpan) {});
  call.set_on_error([](Status) {});
}

TEST(RawServerReaderWriter, TryCloseFailed) {
  ReaderWriterTestContext ctx;
  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish(OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());
}

TEST(RawServerReaderWriter, TryCloseSuccessful) {
  ReaderWriterTestContext ctx;
  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);
  // Sets ChannelOutput to always return false.
  ctx.output.set_send_status(Status::Unknown());
  ASSERT_EQ(Status::Unknown(), call.TryFinish(OkStatus()));

  // Call should be still alive.
  ASSERT_TRUE(call.active());

  // Tries to close the call again, with ChannelOutput set to return ok.
  ctx.output.set_send_status(OkStatus());
  ASSERT_EQ(OkStatus(), call.TryFinish(OkStatus()));
  // Call should be closed.
  ASSERT_FALSE(call.active());
}

TEST(RawUnaryResponder, Open_ReturnsUsableResponder) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);

  EXPECT_EQ(call.channel_id(), ctx.channel.id());
  EXPECT_EQ(OkStatus(), call.Finish(as_bytes(span("hello from pw_rpc"))));

  EXPECT_STREQ(
      reinterpret_cast<const char*>(
          ctx.output.payloads<TestService::TestUnaryRpc>().back().data()),
      "hello from pw_rpc");
}

TEST(RawServerReaderWriter, Open_UnknownChannel) {
  ReaderWriterTestContext ctx;
  ASSERT_EQ(OkStatus(), ctx.server.CloseChannel(ctx.kChannelId));

  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.kChannelId, ctx.service);

  EXPECT_TRUE(call.active());
  EXPECT_EQ(call.channel_id(), ctx.kChannelId);
  EXPECT_EQ(Status::Unavailable(), call.Write(ConstByteSpan()));

  ASSERT_EQ(OkStatus(), ctx.server.OpenChannel(ctx.kChannelId, ctx.output));

  EXPECT_EQ(OkStatus(), call.Write(ConstByteSpan()));
  EXPECT_TRUE(call.active());

  EXPECT_EQ(OkStatus(), call.Finish());
  EXPECT_FALSE(call.active());
  EXPECT_EQ(call.channel_id(), Channel::kUnassignedChannelId);
}

TEST(RawUnaryResponder, Open_MultipleTimes_CancelsPrevious) {
  ReaderWriterTestContext ctx;

  RawUnaryResponder one = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);

  std::optional<Status> error;
  one.set_on_error([&error](Status status) { error = status; });

  ASSERT_TRUE(one.active());

  RawUnaryResponder two = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
      ctx.server, ctx.channel.id(), ctx.service);

  ASSERT_FALSE(one.active());
  ASSERT_TRUE(two.active());

  EXPECT_EQ(Status::Cancelled(), error);
}

TEST(RawServerWriter, Open_ReturnsUsableWriter) {
  ReaderWriterTestContext ctx;
  RawServerWriter call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  EXPECT_EQ(call.channel_id(), ctx.channel.id());
  EXPECT_EQ(OkStatus(), call.Write(as_bytes(span("321"))));

  EXPECT_STREQ(reinterpret_cast<const char*>(
                   ctx.output.payloads<TestService::TestServerStreamRpc>()
                       .back()
                       .data()),
               "321");
}

TEST(RawServerReader, Open_ReturnsUsableReader) {
  ReaderWriterTestContext ctx;
  RawServerReader call =
      RawServerReader::Open<TestService::TestClientStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  EXPECT_EQ(call.channel_id(), ctx.channel.id());
  EXPECT_EQ(OkStatus(), call.Finish(as_bytes(span("This is a message"))));

  EXPECT_STREQ(reinterpret_cast<const char*>(
                   ctx.output.payloads<TestService::TestClientStreamRpc>()
                       .back()
                       .data()),
               "This is a message");
}

TEST(RawServerReaderWriter, Open_ReturnsUsableReaderWriter) {
  ReaderWriterTestContext ctx;
  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  EXPECT_EQ(call.channel_id(), ctx.channel.id());
  EXPECT_EQ(OkStatus(), call.Write(as_bytes(span("321"))));

  EXPECT_STREQ(
      reinterpret_cast<const char*>(
          ctx.output.payloads<TestService::TestBidirectionalStreamRpc>()
              .back()
              .data()),
      "321");
}

TEST(RawUnaryResponder, Move_FinishesActiveCall) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder active_call =
      RawUnaryResponder::Open<TestService::TestUnaryRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  RawUnaryResponder inactive_call;

  active_call = std::move(inactive_call);

  const auto completions = ctx.output.completions<TestService::TestUnaryRpc>();
  ASSERT_EQ(completions.size(), 1u);
  EXPECT_EQ(completions.back(), OkStatus());
}

TEST(RawUnaryResponder, Move_DifferentActiveCalls_ClosesFirstOnly) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder active_call =
      RawUnaryResponder::Open<TestService::TestUnaryRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  RawUnaryResponder new_active_call =
      RawUnaryResponder::Open<TestService::TestAnotherUnaryRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  EXPECT_TRUE(active_call.active());
  EXPECT_TRUE(new_active_call.active());

  active_call = std::move(new_active_call);

  const auto completions = ctx.output.completions<TestService::TestUnaryRpc>();
  ASSERT_EQ(completions.size(), 1u);
  EXPECT_EQ(completions.back(), OkStatus());

  EXPECT_TRUE(
      ctx.output.completions<TestService::TestAnotherUnaryRpc>().empty());
}

TEST(RawUnaryResponder, ReplaceActiveCall_DoesNotFinishCall) {
  ReaderWriterTestContext ctx;
  RawUnaryResponder active_call =
      RawUnaryResponder::Open<TestService::TestUnaryRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  RawUnaryResponder new_active_call =
      RawUnaryResponder::Open<TestService::TestUnaryRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  active_call = std::move(new_active_call);

  ASSERT_TRUE(ctx.output.completions<TestService::TestUnaryRpc>().empty());

  constexpr const char kData[] = "Some data!";
  EXPECT_EQ(
      OkStatus(),
      active_call.Finish(as_bytes(span(kData)), Status::InvalidArgument()));

  EXPECT_STREQ(
      reinterpret_cast<const char*>(
          ctx.output.payloads<TestService::TestUnaryRpc>().back().data()),
      kData);

  const auto completions = ctx.output.completions<TestService::TestUnaryRpc>();
  ASSERT_EQ(completions.size(), 1u);
  EXPECT_EQ(completions.back(), Status::InvalidArgument());
}

TEST(RawUnaryResponder, OutOfScope_FinishesActiveCall) {
  ReaderWriterTestContext ctx;

  {
    RawUnaryResponder call = RawUnaryResponder::Open<TestService::TestUnaryRpc>(
        ctx.server, ctx.channel.id(), ctx.service);
    ASSERT_TRUE(ctx.output.completions<TestService::TestUnaryRpc>().empty());
  }

  const auto completions = ctx.output.completions<TestService::TestUnaryRpc>();
  ASSERT_EQ(completions.size(), 1u);
  EXPECT_EQ(completions.back(), OkStatus());
}

TEST(RawServerWriter, Move_InactiveToActive_FinishesActiveCall) {
  ReaderWriterTestContext ctx;
  RawServerWriter active_call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  RawServerWriter inactive_call;

  active_call = std::move(inactive_call);

  const auto completions =
      ctx.output.completions<TestService::TestServerStreamRpc>();
  ASSERT_EQ(completions.size(), 1u);
  EXPECT_EQ(completions.back(), OkStatus());
}

TEST(RawServerWriter, ReplaceActiveCall_DoesNotFinishCall) {
  ReaderWriterTestContext ctx;
  RawServerWriter active_call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  RawServerWriter new_active_call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  active_call = std::move(new_active_call);

  ASSERT_TRUE(
      ctx.output.completions<TestService::TestServerStreamRpc>().empty());

  constexpr const char kData[] = "Some data!";
  EXPECT_EQ(OkStatus(), active_call.Write(as_bytes(span(kData))));

  EXPECT_STREQ(reinterpret_cast<const char*>(
                   ctx.output.payloads<TestService::TestServerStreamRpc>()
                       .back()
                       .data()),
               kData);
}

constexpr const char kWriterData[] = "20X6";

void WriteAsWriter(Writer& writer) {
  ASSERT_TRUE(writer.active());
  ASSERT_EQ(writer.channel_id(), ReaderWriterTestContext::kChannelId);

  EXPECT_EQ(OkStatus(), writer.Write(as_bytes(span(kWriterData))));
}

TEST(RawServerWriter, UsableAsWriter) {
  ReaderWriterTestContext ctx;
  RawServerWriter call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  WriteAsWriter(call.as_writer());

  EXPECT_STREQ(reinterpret_cast<const char*>(
                   ctx.output.payloads<TestService::TestServerStreamRpc>()
                       .back()
                       .data()),
               kWriterData);
}

TEST(RawServerReaderWriter, UsableAsWriter) {
  ReaderWriterTestContext ctx;
  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  WriteAsWriter(call.as_writer());

  EXPECT_STREQ(
      reinterpret_cast<const char*>(
          ctx.output.payloads<TestService::TestBidirectionalStreamRpc>()
              .back()
              .data()),
      kWriterData);
}

TEST(RawUnaryResponder, MaxSafePayloadSize_Inactive) {
  RawUnaryResponder call;
  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

TEST(RawServerWriter, MaxSafePayloadSize_NoChannel) {
  ReaderWriterTestContext ctx;
  PW_TEST_ASSERT_OK(ctx.server.CloseChannel(ctx.kChannelId));

  RawServerWriter call =
      RawServerWriter::Open<TestService::TestServerStreamRpc>(
          ctx.server, ctx.kChannelId, ctx.service);

  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
  EXPECT_EQ(Status::Unavailable(), call.Write(ConstByteSpan()));
}

TEST(RawServerReader, MaxSafePayloadSize_UnlimitedChannel) {
  ReaderWriterTestContext ctx;
  RawServerReader call =
      RawServerReader::Open<TestService::TestClientStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  EXPECT_EQ(call.MaxWriteSizeBytes(), pw::rpc::MaxSafePayloadSize());

  PW_TEST_EXPECT_OK(call.Finish({}));
  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

TEST(RawServerReaderWriter, MaxSafePayloadSize_LimitedChannel) {
  ReaderWriterTestContext ctx;
  RawServerReaderWriter call =
      RawServerReaderWriter::Open<TestService::TestBidirectionalStreamRpc>(
          ctx.server, ctx.channel.id(), ctx.service);

  constexpr size_t kChannelMtu = 40;
  static_assert(kChannelMtu < pw::rpc::MaxSafePayloadSize());

  ctx.output.set_mtu(kChannelMtu);

  EXPECT_EQ(call.MaxWriteSizeBytes(), pw::rpc::MaxSafePayloadSize(kChannelMtu));

  PW_TEST_EXPECT_OK(call.Finish());
  EXPECT_EQ(call.MaxWriteSizeBytes(), 0u);
}

}  // namespace
}  // namespace pw::rpc
