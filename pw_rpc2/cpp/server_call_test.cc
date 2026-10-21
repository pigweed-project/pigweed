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
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/server_task.h"
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

  const std::optional<RpcStatus>& result() const { return result_; }

 private:
  Fut fut_;
  std::optional<RpcStatus> result_;
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

  const std::optional<RpcResult<ConstBuf>>& result() const { return result_; }

 private:
  Fut fut_;
  std::optional<RpcResult<ConstBuf>> result_;
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

  std::optional<RpcResult<WriteReservation>>& result() { return result_; }
  const std::optional<RpcResult<WriteReservation>>& result() const {
    return result_;
  }

 private:
  Fut fut_;
  std::optional<RpcResult<WriteReservation>> result_;
};

template <typename Fut>
ReserveTestTask(Fut) -> ReserveTestTask<Fut>;

// Each test starts calls on a real server connection and acts as their method.
class ServerCallTest : public ::testing::Test {
 protected:
  ServerCallTest() {
    auto [conn, raw_conn] = test::MakeMockConnection(allocator_);
    raw_conn_ = raw_conn;
    connection_task_ = dispatcher_.Post<internal::ServerConnectionTask>(
        allocator_,
        internal::EstablishedConnection{std::move(conn)},
        allocator_,
        server_);
  }

  // Unpost the connection while the server it refers to is still alive.
  ~ServerCallTest() override { connection_task_->Deregister(); }

  allocator::test::AllocatorForTest<8192> allocator_;
  async2::DispatcherForTest dispatcher_;
  internal::ServerTask server_{allocator_, dispatcher_};
  test::MockConnection* raw_conn_ = nullptr;
  SharedPtr<internal::ServerConnectionTask> connection_task_;
};

TEST_F(ServerCallTest, UnaryWriterFinishSendsResponsePacket) {
  auto call = test::StartServerCall(*connection_task_, 50u);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  EXPECT_EQ(internal::CallAccess::call_id(responder), 50u);

  const auto resp_bytes = as_bytes(span("unary resp", 10));
  WriteTestTask task(responder.FinishCopy(ConstBuf::Unowned(resp_bytes)));
  dispatcher_.Post(task);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(raw_conn_->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(),
            (internal::PacketType::Make<flags::kServer,
                                        flags::kHasPayload,
                                        flags::kOkTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 50u);
  EXPECT_EQ(decode_res.payload().size(), 10u);
  EXPECT_EQ(std::memcmp(decode_res.payload().data(), "unary resp", 10), 0);

  task.Deregister();
}

TEST_F(ServerCallTest, UnaryWriterFinishCopyReleasesOwnedResponseWhenSent) {
  allocator::test::AllocatorForTest<256> response_allocator;
  auto call = test::StartServerCall(*connection_task_, 51u);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));

  ConstBuf response = MakeConstBuf(response_allocator, "owned");
  ASSERT_GT(response_allocator.metrics().allocated_bytes.value(), 0u);

  // `task` keeps the `WriteFuture` alive after it completes.
  WriteTestTask task(responder.FinishCopy(std::move(response)));
  dispatcher_.Post(task);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(response_allocator.metrics().allocated_bytes.value(), 0u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.call_id(), 51u);
  ASSERT_EQ(decode_res.payload().size(), 5u);
  EXPECT_EQ(std::memcmp(decode_res.payload().data(), "owned", 5), 0);

  task.Deregister();
}

// Typed writers copy encoded responses from a `ConstBuf` like raw writers do.
TEST_F(ServerCallTest, TypedUnaryWriterFinishCopySendsBytes) {
  struct TypedResponse {
    int value;
  };

  allocator::test::AllocatorForTest<256> response_allocator;
  auto call = test::StartServerCall(*connection_task_, 52u);

  auto responder =
      internal::CallAccess::Create<UnaryWriter<TypedResponse>>(std::move(call));

  ConstBuf response = MakeConstBuf(response_allocator, "typed");
  ASSERT_GT(response_allocator.metrics().allocated_bytes.value(), 0u);

  // `task` keeps the `WriteFuture` alive after it completes.
  WriteTestTask task(responder.FinishCopy(std::move(response)));
  dispatcher_.Post(task);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(response_allocator.metrics().allocated_bytes.value(), 0u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.call_id(), 52u);
  ASSERT_EQ(decode_res.payload().size(), 5u);
  EXPECT_EQ(std::memcmp(decode_res.payload().data(), "typed", 5), 0);

  task.Deregister();
}

TEST_F(ServerCallTest, UnaryWriterReserveThatOverflowsFails) {
  auto call = test::StartServerCall(*connection_task_, 55u);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));

  // Adding the packet header to this size would wrap around.
  ReserveTestTask task(
      responder.ReserveFinish(std::numeric_limits<size_t>::max()));
  dispatcher_.Post(task);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), RpcStatus::MessageTooLarge());
  EXPECT_EQ(raw_conn_->commit_count(), 0u);

  task.Deregister();
}

TEST_F(ServerCallTest, ServerStreamingWriterWriteAndFinish) {
  auto call = test::StartServerCall(*connection_task_, 60u);

  auto responder = internal::CallAccess::Create<RawWriter>(std::move(call));

  // Write chunk 1
  ReserveTestTask task1(responder.ReserveWrite(7));
  dispatcher_.Post(task1);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(task1.result().has_value() && task1.result()->ok());
  std::memcpy((*task1.result())->data(), "chunk 1", 7);
  PW_TEST_EXPECT_OK((*task1.result())->Commit(7));

  auto decode1 = internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode1, nullptr);
  EXPECT_EQ(decode1.type(),
            (internal::PacketType::Make<flags::kServer, flags::kHasPayload>()));

  // Finish stream
  WriteTestTask task2(responder.Finish());
  dispatcher_.Post(task2);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(task2.result().has_value() && task2.result()->ok());

  auto decode2 = internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode2, nullptr);
  EXPECT_EQ(decode2.type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));

  task1.Deregister();
  task2.Deregister();
}

TEST_F(ServerCallTest, ReaderAndWriterShareCallId) {
  auto call = test::StartServerCall(*connection_task_, 70u);
  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  EXPECT_EQ(internal::CallAccess::call_id(reader), 70u);
  EXPECT_EQ(internal::CallAccess::call_id(writer), 70u);
}

TEST_F(ServerCallTest, ClientStreamingReadsRequestsThenFinishes) {
  auto call = test::StartServerCall(*connection_task_, 110u);

  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto responder = internal::CallAccess::Create<RawUnaryWriter>(call);

  // 1. Client pushes chunk 1 into server receiver
  ReserveSlotTask reserve(*call);
  dispatcher_.Post(reserve);
  dispatcher_.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakeConstBuf(allocator_, "chunk1"));

  ReadTestTask read_task(reader.Read());
  dispatcher_.Post(read_task);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(read_task.result().has_value() && read_task.result()->ok());
  EXPECT_EQ(read_task.result()->value().size(), 6u);
  read_task.Deregister();

  // 2. Server finishes with UnaryWriter via ReserveFinish
  ReserveTestTask finish_task(responder.ReserveFinish(7));
  dispatcher_.Post(finish_task);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(finish_task.result().has_value() && finish_task.result()->ok());
  std::memcpy((*finish_task.result())->data(), "summary", 7);
  PW_TEST_EXPECT_OK((*finish_task.result())->Commit(7));
  EXPECT_EQ(raw_conn_->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(),
            (internal::PacketType::Make<flags::kServer,
                                        flags::kHasPayload,
                                        flags::kOkTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 110u);

  finish_task.Deregister();
}

TEST_F(ServerCallTest, BidiStreamingWritesMessageThenFinishes) {
  auto call = test::StartServerCall(*connection_task_, 120u);

  auto reader = internal::CallAccess::Create<RawReader>(call);
  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // Server writes message via ReserveWrite
  ReserveTestTask write_task(writer.ReserveWrite(12));
  dispatcher_.Post(write_task);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(write_task.result().has_value() && write_task.result()->ok());
  std::memcpy((*write_task.result())->data(), "server chunk", 12);
  PW_TEST_EXPECT_OK((*write_task.result())->Commit(12));
  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  write_task.Deregister();

  // Server finishes
  WriteTestTask close_task(writer.Finish());
  dispatcher_.Post(close_task);
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(close_task.result().has_value() && close_task.result()->ok());
  EXPECT_EQ(raw_conn_->commit_count(), 2u);
  close_task.Deregister();
}

TEST_F(ServerCallTest, UnaryWriterCancel) {
  auto call = test::StartServerCall(*connection_task_, 80u);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  responder.Cancel();
  EXPECT_TRUE(responder.is_closed());
  dispatcher_.RunUntilStalled();

  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(
      decode_res.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res.error(), internal::ProtocolError::kCancelled);
}

TEST_F(ServerCallTest, RAIICancellationOnDestruction) {
  auto call = test::StartServerCall(*connection_task_, 90u);

  {
    auto responder =
        internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
    EXPECT_EQ(internal::CallAccess::call_id(responder), 90u);
    // responder destroyed here without calling Finish() or FinishWithError()
  }

  // Destroying an unfinished responder queues a
  // kDroppedWithoutResponse error packet, which the connection
  // task writes out the next time it is polled.
  dispatcher_.RunUntilStalled();

  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(
      decode_res.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 90u);
  EXPECT_EQ(decode_res.error(),
            internal::ProtocolError::kDroppedWithoutResponse);
}

TEST_F(ServerCallTest, ZeroCopyReserveFinish) {
  auto call = test::StartServerCall(*connection_task_, 100u);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  ReserveTestTask task(responder.ReserveFinish(12));
  dispatcher_.Post(task);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  auto res = std::move(**task.result());
  EXPECT_GE(res.size(), 12u);

  // Write payload zero-copy into reserved buffer
  const char* msg = "hello 0-copy";
  std::memcpy(res.data(), msg, 12);
  PW_TEST_EXPECT_OK(res.Commit(12));

  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(),
            (internal::PacketType::Make<flags::kServer,
                                        flags::kHasPayload,
                                        flags::kOkTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 100u);
  EXPECT_EQ(decode_res.payload().size(), 12u);

  task.Deregister();
}

TEST_F(ServerCallTest, DroppedUnaryFinishFutureFallsBackToRAIICancellation) {
  auto call = test::StartServerCall(*connection_task_, 101u);

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

  dispatcher_.RunUntilStalled();
  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(
      decode_res.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 101u);
  EXPECT_EQ(decode_res.error(),
            internal::ProtocolError::kDroppedWithoutResponse);
}

TEST_F(ServerCallTest, DroppedUnaryReservationReopensWriterWhileAlive) {
  auto call = test::StartServerCall(*connection_task_, 102u);

  auto responder =
      internal::CallAccess::Create<RawUnaryWriter>(std::move(call));
  {
    ReserveTestTask task(responder.ReserveFinish(8));
    dispatcher_.Post(task);
    dispatcher_.RunUntilStalled();
    ASSERT_TRUE(task.result().has_value() && task.result()->ok());
    // Drop the reservation without calling Commit().
    (*task.result())->Drop();
    task.Deregister();
  }

  // Because the reservation was dropped without committing and the responder
  // is still alive, the write side is open again.
  EXPECT_FALSE(responder.is_closed());

  responder.Cancel();
  dispatcher_.RunUntilStalled();

  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(
      decode_res.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 102u);
  EXPECT_EQ(decode_res.error(), internal::ProtocolError::kCancelled);
}

TEST_F(ServerCallTest, DroppedWriterFinishFutureFallsBackToRAIIStreamEnd) {
  auto call = test::StartServerCall(*connection_task_, 103u);

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

  dispatcher_.RunUntilStalled();
  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res =
      internal::InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 103u);
}

}  // namespace
}  // namespace pw::rpc2
