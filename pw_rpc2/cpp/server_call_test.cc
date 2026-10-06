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
#include <limits>
#include <optional>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/connection_task.h"
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

ConstBuf MakeConstBuf(Allocator& alloc, std::string_view str) {
  Buf buf = Buf::Allocate(alloc, str.size());
  std::memcpy(buf.data(), str.data(), str.size());
  return ConstBuf(std::move(buf));
}

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

  const std::optional<Result<ConstBuf>>& result() const { return result_; }

 private:
  Fut fut_;
  std::optional<Result<ConstBuf>> result_;
};

template <typename Fut>
ReadTestTask(Fut) -> ReadTestTask<Fut>;

template <typename Fut = ReserveWriteFuture>
class ReserveTestTask : public async2::Task {
 public:
  explicit ReserveTestTask(Fut fut) : fut_(std::move(fut)) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    result_ = std::move(*poll);
    return async2::Ready();
  }

  std::optional<Result<WriteReservation>>& result() { return result_; }
  const std::optional<Result<WriteReservation>>& result() const {
    return result_;
  }

 private:
  Fut fut_;
  std::optional<Result<WriteReservation>> result_;
};

template <typename Fut>
ReserveTestTask(Fut) -> ReserveTestTask<Fut>;

TEST(ServerCallTest, UnaryWriterFinishSendsResponsePacket) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 50u, allocator);
  ASSERT_NE(call, nullptr);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  EXPECT_EQ(internal::CallAccess::call_id(responder), 50u);

  const auto resp_bytes = as_bytes(span("unary resp", 10));
  WriteTestTask task(responder.FinishCopy(resp_bytes));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            (internal::PacketType::Make<flags::kServer,
                                        flags::kHasPayload,
                                        flags::kOkTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 50u);
  EXPECT_EQ(decode_res->payload().size(), 10u);
  EXPECT_EQ(std::memcmp(decode_res->payload().data(), "unary resp", 10), 0);

  task.Deregister();
  connection_task->Deregister();
}

TEST(ServerCallTest, UnaryWriterReserveThatOverflowsFails) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 55u, allocator);
  ASSERT_NE(call, nullptr);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));

  // Adding the packet header to this size would wrap around.
  ReserveTestTask task(
      responder.ReserveFinish(std::numeric_limits<size_t>::max()));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), Status::ResourceExhausted());
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  task.Deregister();
  connection_task->Deregister();
}

TEST(ServerCallTest, ServerStreamingWriterWriteAndFinish) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 60u, allocator);
  ASSERT_NE(call, nullptr);

  auto responder = internal::CallAccess::Create<RawWriter>(std::move(call));

  // Write chunk 1
  ReserveTestTask task1(responder.ReserveWrite(7));
  dispatcher.Post(task1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task1.result().has_value() && task1.result()->ok());
  std::memcpy((*task1.result())->data(), "chunk 1", 7);
  PW_TEST_EXPECT_OK((*task1.result())->Commit(7));

  auto decode1 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode1.ok());
  EXPECT_EQ(decode1->type(),
            (internal::PacketType::Make<flags::kServer, flags::kHasPayload>()));

  // Finish stream
  WriteTestTask task2(responder.Finish());
  dispatcher.Post(task2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task2.result().has_value() && task2.result()->ok());

  auto decode2 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode2.ok());
  EXPECT_EQ(decode2->type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));

  task1.Deregister();
  task2.Deregister();
  connection_task->Deregister();
}

TEST(ServerCallTest, ReaderAndWriterShareCallId) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 70u, allocator);
  ASSERT_NE(call, nullptr);
  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  EXPECT_EQ(internal::CallAccess::call_id(reader), 70u);
  EXPECT_EQ(internal::CallAccess::call_id(writer), 70u);
  connection_task->Deregister();
}

TEST(ServerCallTest, ClientStreamingReadsRequestsThenFinishes) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 110u, allocator);
  ASSERT_NE(call, nullptr);

  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto responder = internal::CallAccess::Create<RawUnaryWriter>(call);

  // 1. Client pushes chunk 1 into server receiver
  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakeConstBuf(allocator, "chunk1"));

  ReadTestTask read_task(reader.Read());
  dispatcher.Post(read_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task.result().has_value() && read_task.result()->ok());
  EXPECT_EQ(read_task.result()->value().size(), 6u);
  read_task.Deregister();

  // 2. Server finishes with UnaryWriter via ReserveFinish
  ReserveTestTask finish_task(responder.ReserveFinish(7));
  dispatcher.Post(finish_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(finish_task.result().has_value() && finish_task.result()->ok());
  std::memcpy((*finish_task.result())->data(), "summary", 7);
  PW_TEST_EXPECT_OK((*finish_task.result())->Commit(7));
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            (internal::PacketType::Make<flags::kServer,
                                        flags::kHasPayload,
                                        flags::kOkTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 110u);

  finish_task.Deregister();
  connection_task->Deregister();
}

TEST(ServerCallTest, BidiStreamingWritesMessageThenFinishes) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 120u, allocator);
  ASSERT_NE(call, nullptr);

  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // Server writes message via ReserveWrite
  ReserveTestTask write_task(writer.ReserveWrite(12));
  dispatcher.Post(write_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write_task.result().has_value() && write_task.result()->ok());
  std::memcpy((*write_task.result())->data(), "server chunk", 12);
  PW_TEST_EXPECT_OK((*write_task.result())->Commit(12));
  EXPECT_EQ(raw_conn->commit_count(), 1u);
  write_task.Deregister();

  // Server finishes
  WriteTestTask close_task(writer.Finish());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(close_task.result().has_value() && close_task.result()->ok());
  EXPECT_EQ(raw_conn->commit_count(), 2u);
  close_task.Deregister();
  connection_task->Deregister();
}

TEST(ServerCallTest, UnaryWriterCancel) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 80u, allocator);
  ASSERT_NE(call, nullptr);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  responder.Cancel();
  EXPECT_TRUE(responder.is_closed());
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(
      decode_res->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res->error(), internal::ProtocolStatus::kCancelled);

  connection_task->Deregister();
}

TEST(ServerCallTest, RAIICancellationOnDestruction) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 90u, allocator);
  ASSERT_NE(call, nullptr);

  {
    auto responder =
        internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
    EXPECT_EQ(internal::CallAccess::call_id(responder), 90u);
    // responder destroyed here without calling Finish() or FinishWithError()
  }

  // Destroying an unfinished responder queues a
  // kDroppedWithoutResponse error packet, which the connection
  // task writes out the next time it is polled.
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(
      decode_res->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 90u);
  EXPECT_EQ(decode_res->error(),
            internal::ProtocolStatus::kDroppedWithoutResponse);

  connection_task->Deregister();
}

TEST(ServerCallTest, ZeroCopyReserveFinish) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 100u, allocator);
  ASSERT_NE(call, nullptr);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  ReserveTestTask task(responder.ReserveFinish(12));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  auto res = std::move(**task.result());
  EXPECT_GE(res.size(), 12u);

  // Write payload zero-copy into reserved buffer
  const char* msg = "hello 0-copy";
  std::memcpy(res.data(), msg, 12);
  PW_TEST_EXPECT_OK(res.Commit(12));

  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            (internal::PacketType::Make<flags::kServer,
                                        flags::kHasPayload,
                                        flags::kOkTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 100u);
  EXPECT_EQ(decode_res->payload().size(), 12u);

  task.Deregister();
  connection_task->Deregister();
}

TEST(ServerCallTest, DroppedUnaryFinishFutureFallsBackToRAIICancellation) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 101u, allocator);
  ASSERT_NE(call, nullptr);

  // Obtain a terminal future, destroy the responder while the future is still
  // alive, and then drop the future without awaiting/committing it.
  {
    std::optional<ReserveWriteFuture> pending_finish;
    {
      auto responder =
          internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
      pending_finish.emplace(responder.ReserveFinish(8));
      EXPECT_TRUE(responder.is_closed());
    }
    // Responder is now destroyed; dropping `pending_finish` must trigger the
    // deferred kDroppedWithoutResponse packet.
  }

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(
      decode_res->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 101u);
  EXPECT_EQ(decode_res->error(),
            internal::ProtocolStatus::kDroppedWithoutResponse);

  connection_task->Deregister();
}

TEST(ServerCallTest, DroppedUnaryReservationReopensWriterWhileAlive) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 102u, allocator);
  ASSERT_NE(call, nullptr);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  {
    ReserveTestTask task(responder.ReserveFinish(8));
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    ASSERT_TRUE(task.result().has_value() && task.result()->ok());
    // Drop the reservation without calling Commit().
    (*task.result())->Drop();
    task.Deregister();
  }

  // Because the reservation was dropped without committing and the responder
  // is still alive, the write side is open again.
  EXPECT_FALSE(responder.is_closed());

  responder.Cancel();
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(
      decode_res->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 102u);
  EXPECT_EQ(decode_res->error(), internal::ProtocolStatus::kCancelled);

  connection_task->Deregister();
}

TEST(ServerCallTest, DroppedWriterFinishFutureFallsBackToRAIIStreamEnd) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<test::TestConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);
  auto call = internal::ClientCall::Create(*connection_task, 103u, allocator);
  ASSERT_NE(call, nullptr);

  {
    std::optional<WriteFuture<>> pending_close;
    {
      auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));
      pending_close.emplace(writer.Finish());
      EXPECT_TRUE(writer.is_closed());
    }
    // Writer is destroyed while `pending_close` is still uncommitted; dropping
    // `pending_close` must send the deferred StreamEnd packet.
  }

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 103u);

  connection_task->Deregister();
}

}  // namespace
}  // namespace pw::rpc2
