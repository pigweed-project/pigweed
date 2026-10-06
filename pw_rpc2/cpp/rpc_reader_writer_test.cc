// Copyright 2026 The Pigweed Authors
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
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_bytes/endian.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/client_connection_task.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/writer.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2 {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

// Acquires a slot in a call's ingress queue, as the connection task
// does when it has a message to deliver.
class ReserveSlotTask : public async2::Task {
 public:
  explicit ReserveSlotTask(internal::Call& call) : call_(call) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    reserved_ = call_.ReserveMessageSlot(cx);
    return async2::Ready();
  }

  bool reserved() const { return reserved_; }

 private:
  internal::Call& call_;
  bool reserved_ = false;
};

template <typename Fut>
class WriteTestTask : public async2::Task {
 public:
  explicit WriteTestTask(Fut fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    result_ = *poll;
    return async2::Ready();
  }

  const std::optional<Status>& result() const { return result_; }

 private:
  Fut fut_;
  std::optional<Status> result_;
};

template <typename Fut>
WriteTestTask(Fut fut) -> WriteTestTask<Fut>;

template <typename Fut = RawReadFuture>
class ReadTestTask : public async2::Task {
 public:
  explicit ReadTestTask(Fut fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    result_ = std::move(*poll);
    return async2::Ready();
  }

  Fut& future() { return fut_; }
  const std::optional<Result<ConstBuf>>& result() const { return result_; }

 private:
  Fut fut_;
  std::optional<Result<ConstBuf>> result_;
};

template <typename Fut>
ReadTestTask(Fut fut) -> ReadTestTask<Fut>;

class ReserveTestTask : public async2::Task {
 public:
  explicit ReserveTestTask(ReserveWriteFuture fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    result_ = std::move(*poll);
    return async2::Ready();
  }

  Result<WriteReservation>& result() { return *result_; }

 private:
  ReserveWriteFuture fut_;
  std::optional<Result<WriteReservation>> result_;
};

TEST(RawReaderWriterTest, RequireValidCallAndExposeCallId) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 123u, allocator);
  ASSERT_NE(call, nullptr);

  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  EXPECT_EQ(internal::CallAccess::call_id(reader), 123u);
  EXPECT_EQ(internal::CallAccess::call_id(writer), 123u);
  EXPECT_FALSE(writer.is_closed());
}

TEST(RawWriterTest, WritesMessageAndFinish) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 77u, allocator);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // 1. Reserve 10 bytes, write 3 bytes, and commit 3 bytes.
  ReserveTestTask task1(writer.ReserveWrite(10));
  dispatcher.Post(task1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task1.result().ok());
  WriteReservation& res1 = *task1.result();
  EXPECT_GE(res1.size(), 10u);
  EXPECT_EQ(static_cast<size_t>(res1.end() - res1.begin()), res1.size());
  EXPECT_EQ(static_cast<size_t>(res1.cend() - res1.cbegin()), res1.size());
  res1[0] = std::byte(1);
  res1[1] = std::byte(2);
  res1[2] = std::byte(3);
  PW_TEST_EXPECT_OK(res1.Commit(3));
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode1 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode1.ok());
  EXPECT_EQ(decode1->type(), internal::PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decode1->call_id(), 77u);
  ASSERT_EQ(decode1->payload().size(), 3u);
  EXPECT_EQ(decode1->payload()[0], std::byte(1));
  EXPECT_EQ(decode1->payload()[1], std::byte(2));
  EXPECT_EQ(decode1->payload()[2], std::byte(3));

  // 2. Write second message via WriteCopy
  const std::byte msg2[2] = {std::byte(10), std::byte(20)};
  WriteTestTask task2(writer.WriteCopy(msg2));
  dispatcher.Post(task2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task2.result().has_value() && task2.result()->ok());
  EXPECT_EQ(raw_conn->commit_count(), 2u);

  auto decode2 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode2.ok());
  EXPECT_EQ(decode2->type(), internal::PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decode2->call_id(), 77u);
  EXPECT_EQ(decode2->payload().size(), 2u);
  EXPECT_EQ(decode2->payload()[0], std::byte(10));
  EXPECT_EQ(decode2->payload()[1], std::byte(20));

  // 3. Finish stream
  WriteTestTask task3(writer.Finish());
  dispatcher.Post(task3);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task3.result().has_value() && task3.result()->ok());
  EXPECT_TRUE(writer.is_closed());
  EXPECT_EQ(raw_conn->commit_count(), 3u);

  auto decode3 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode3.ok());
  EXPECT_EQ(decode3->type(), internal::PacketType::Make<flags::kStreamEnd>());

  task1.Deregister();
  task2.Deregister();
  task3.Deregister();
}

TEST(RawWriterTest, CancelledReservationReleasesWithoutCommit) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 88u, allocator);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  ReserveTestTask task(writer.ReserveWrite(10));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().ok());
  auto res = std::move(task.result().value());

  // Explicitly drop reservation without committing
  res.Drop();
  EXPECT_EQ(res.Commit(0), Status::FailedPrecondition());
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  task.Deregister();
}

TEST(RawWriterTest, OversizeReservationFailsWithoutClosingWriter) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  const size_t max_size = conn.max_write_message_size_bytes();
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 89u, allocator);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // The packet header pushes this over the transport's limit.
  ReserveTestTask oversize(writer.ReserveWrite(max_size));
  dispatcher.Post(oversize);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(oversize.result().status(), Status::ResourceExhausted());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_FALSE(writer.is_closed());

  // A size that would overflow `sizeof(PacketHeader) + max_payload_size` must
  // also fail with RESOURCE_EXHAUSTED rather than wrapping around.
  ReserveTestTask overflow(
      writer.ReserveWrite(std::numeric_limits<size_t>::max()));
  dispatcher.Post(overflow);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(overflow.result().status(), Status::ResourceExhausted());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_FALSE(writer.is_closed());

  // The writer can still send messages that fit.
  ReserveTestTask fits(writer.ReserveWrite(3));
  dispatcher.Post(fits);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(fits.result().ok());
  PW_TEST_EXPECT_OK(fits.result()->Commit(3));
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  oversize.Deregister();
  overflow.Deregister();
  fits.Deregister();
}

TEST(RawWriterTest, WriterErrorTransmitsErrorPacket) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 99u, allocator);
  ASSERT_NE(call, nullptr);
  // The server only learns of a call once its start packet is sent, which
  // this test bypasses.
  call->MarkStarted();

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  writer.Cancel();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(writer.is_closed());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            internal::PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res->call_id(), 99u);
  EXPECT_EQ(decode_res->error(), internal::ProtocolStatus::kCancelled);
}

TEST(RawWriterTest, CancelBeforeStartSendsNothing) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 99u, allocator);
  ASSERT_NE(call, nullptr);
  internal::Call& raw_call = *call;

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // The start packet was never sent, so the server does not know the call.
  writer.Cancel();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(writer.is_closed());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_TRUE(raw_call.is_completed());
  EXPECT_EQ(raw_call.completion_status(), Status::Cancelled());
}

// Destroying a `Writer` ends the outbound stream normally. This is the
// asymmetric counterpart to `UnaryWriter`, which cancels instead.
TEST(RawWriterTest, DestructorEndsStreamNormally) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 55u, allocator);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  {
    auto writer = internal::CallAccess::Create<RawWriter>(call);
    EXPECT_FALSE(writer.is_closed());
  }

  EXPECT_TRUE(call->is_write_closed());
  ASSERT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            internal::PacketType::Make<flags::kStreamEnd>());
  EXPECT_EQ(decode_res->call_id(), 55u);
}

// A unary call owes the client-facing side exactly one response, so an
// unfinished `UnaryWriter` cancels rather than ending the stream.
TEST(RawWriterTest, UnaryWriterDestructorCancels) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<test::TestConnectionTask>(
      internal::EstablishedConnection{std::move(conn)},
      allocator,
      internal::EndpointRole::kServer);
  auto call = internal::ClientCall::Create(*connection_task, 56u, allocator);
  ASSERT_NE(call, nullptr);

  {
    auto responder = internal::CallAccess::Create<RawUnaryWriter>(call);
    EXPECT_FALSE(responder.is_closed());
  }

  EXPECT_TRUE(call->is_write_closed());
  ASSERT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(
      decode_res->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 56u);
  EXPECT_EQ(decode_res->error(),
            internal::ProtocolStatus::kDroppedWithoutResponse);
}

// Once the stream has been finished explicitly, the destructor must not send a
// second stream end.
TEST(RawWriterTest, DestructorAfterFinishDoesNotResend) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 57u, allocator);
  ASSERT_NE(call, nullptr);

  {
    auto writer = internal::CallAccess::Create<RawWriter>(call);
    WriteTestTask task(writer.Finish());
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    ASSERT_TRUE(task.result().has_value() && task.result()->ok());
    EXPECT_TRUE(writer.is_closed());
    task.Deregister();
  }

  // Exactly one stream end: the explicit `Finish()`, not a second from the
  // destructor.
  EXPECT_EQ(raw_conn->commit_count(), 1u);
}

// A message reserved before the stream was finished must not be sent after the
// stream end, since the peer would see a message on a closed stream.
TEST(RawWriterTest, MessageCommittedAfterFinishIsRejected) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 58u, allocator);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  ReserveTestTask message(writer.ReserveWrite(2));
  dispatcher.Post(message);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(message.result().ok());

  WriteTestTask finish(writer.Finish());
  dispatcher.Post(finish);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(finish.result().has_value() && finish.result()->ok());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  EXPECT_EQ(message.result()->Commit(2), Status::FailedPrecondition());
  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decoded = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->type(), internal::PacketType::Make<flags::kStreamEnd>());

  message.Deregister();
  finish.Deregister();
}

// A message reserved before the call was cancelled reports the cancellation
// when committed, and is not sent.
TEST(RawWriterTest, MessageCommittedAfterCancelReportsCancellation) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 59u, allocator);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  ReserveTestTask message(writer.ReserveWrite(2));
  dispatcher.Post(message);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(message.result().ok());

  writer.Cancel();
  EXPECT_EQ(message.result()->Commit(2), Status::Cancelled());
  dispatcher.RunUntilStalled();
  // The call was never started, so nothing at all reaches the peer.
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  message.Deregister();
}

TEST(RawReaderTest, ReadsPayloadBufFromCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 99u, allocator);
  ASSERT_NE(call, nullptr);
  auto reader = internal::CallAccess::Create<RawReader>(call);

  ReadTestTask task(reader.Read());
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.result().has_value());

  Buf msg = Buf::Allocate(allocator, 4);
  std::memset(msg.data(), 0x55, 4);

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  EXPECT_TRUE(reserve.reserved());
  call->OnMessage(ConstBuf(std::move(msg)));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(task.result()->value().size(), 4u);

  task.Deregister();
}

// Reads a single message, expecting the read to resolve within this call.
template <typename Fut>
void ReadOneMessage(async2::DispatcherForTest& dispatcher,
                    Allocator& allocator,
                    internal::Call& call,
                    Fut read_fut,
                    size_t expected_size) {
  ReadTestTask task(std::move(read_fut));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ReserveSlotTask reserve(call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  Buf msg = Buf::Allocate(allocator, expected_size);
  std::memset(msg.data(), 0x11, expected_size);
  ASSERT_TRUE(reserve.reserved());
  call.OnMessage(ConstBuf(std::move(msg)));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(task.result()->value().size(), expected_size);
  task.Deregister();
}

TEST(RawReaderTest, SequentialReadsReuseTheCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 101u, allocator);
  ASSERT_NE(call, nullptr);
  auto reader = internal::CallAccess::Create<RawReader>(call);

  // Each read completes before the next one starts, so only one read is ever
  // outstanding on the call.
  ReadOneMessage(dispatcher, allocator, *call, reader.Read(), 4u);
  ReadOneMessage(dispatcher, allocator, *call, reader.Read(), 2u);

  // A completed future that is still alive must not block the next read.
  RawReadFuture read_fut = reader.Read();
  ReadTestTask task(std::move(read_fut));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  Buf msg = Buf::Allocate(allocator, 3);
  std::memset(msg.data(), 0x22, 3);
  ASSERT_TRUE(reserve.reserved());
  call->OnMessage(ConstBuf(std::move(msg)));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.result().has_value() && task.result()->ok());

  // `task` still owns the completed future here.
  RawReadFuture next_fut = reader.Read();
  EXPECT_TRUE(next_fut.is_pendable());

  task.Deregister();
}

TEST(RawResponseFutureTest, ResolvesToResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 102u, allocator);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  auto response = internal::CallAccess::Create<RawResponseFuture>(
      IntrusivePtr<internal::Call>(call));
  EXPECT_TRUE(response.is_pendable());
  EXPECT_FALSE(call->is_completed());
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  ReadOneMessage(dispatcher, allocator, *call, std::move(response), 4u);
}

TEST(RawResponseFutureTest, DroppingPendingFutureCancelsCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 104u, allocator);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  {
    ReadTestTask task(internal::CallAccess::Create<RawResponseFuture>(
        IntrusivePtr<internal::Call>(call)));
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    EXPECT_FALSE(task.result().has_value());
    task.Deregister();
  }
  // Nothing is left to observe the response, so the call is cancelled.
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), Status::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), 1u);
}

TEST(RawResponseFutureTest, CancelResolvesPendingFutureToCancelled) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 103u, allocator);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  ReadTestTask task(
      internal::CallAccess::Create<RawResponseFuture>(std::move(call)));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.result().has_value());

  task.future().Cancel();
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), Status::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Cancelling again, after the call has ended, does nothing.
  task.future().Cancel();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  task.Deregister();
}

TEST(RawResponseFutureTest, CancelWithoutCallIsNoOp) {
  RawResponseFuture response;
  EXPECT_FALSE(response.is_pendable());
  response.Cancel();
  EXPECT_FALSE(response.is_pendable());
}

}  // namespace
}  // namespace pw::rpc2
