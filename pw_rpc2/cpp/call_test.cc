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

#include "pw_rpc2/internal/call.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_rpc2/internal/client_connection_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/reader.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

class ReadTestTask : public async2::Task {
 public:
  explicit ReadTestTask(Call& call)
      : receive_fut_(call.ClaimRead()), call_(call) {}

  ~ReadTestTask() override {
    if (!released_) {
      call_.ReleaseRead();
    }
  }

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = receive_fut_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    call_.ReleaseRead();
    released_ = true;
    if (poll->has_value()) {
      result_ = std::move(**poll);
    } else if (call_.role() == EndpointRole::kServer &&
               call_.peer_ended_stream()) {
      result_ = Status::OutOfRange();
    } else {
      Status status = call_.completion_status();
      result_ = (call_.is_completed() && !status.ok()) ? status
                                                       : Status::OutOfRange();
    }
    return async2::Ready();
  }

  const std::optional<Result<ConstBuf>>& result() const { return result_; }

 private:
  async2::ReceiveFuture<ConstBuf> receive_fut_;
  Call& call_;
  bool released_ = false;
  std::optional<Result<ConstBuf>> result_;
};

ConstBuf MakePayload(Allocator& alloc, size_t size, std::byte val) {
  Buf payload = Buf::Allocate(alloc, size);
  std::memset(payload.data(), static_cast<int>(val), size);
  return ConstBuf(std::move(payload));
}

// Acquires a slot in a call's ingress queue, as the connection task
// does when it has a message to deliver.
class ReserveSlotTask : public async2::Task {
 public:
  explicit ReserveSlotTask(Call& call) : call_(call) {}

  async2::Poll<> DoPend(async2::Context& cx) override {
    reserved_ = call_.ReserveMessageSlot(cx);
    return async2::Ready();
  }

  bool reserved() const { return reserved_; }

 private:
  Call& call_;
  bool reserved_ = false;
};

TEST(CallTest, IngressMessageQueueAndBackpressure) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 100u, allocator);
  ASSERT_NE(call, nullptr);

  ReserveSlotTask reserve1(*call);
  dispatcher.Post(reserve1);
  dispatcher.RunUntilStalled();
  reserve1.Deregister();
  EXPECT_TRUE(reserve1.reserved());

  call->OnMessage(MakePayload(allocator, 5, std::byte(0xAA)));

  // The queue holds one message, so no second slot is available until the
  // reader drains it.
  ReserveSlotTask reserve2(*call);
  dispatcher.Post(reserve2);
  dispatcher.RunUntilStalled();
  reserve2.Deregister();
  EXPECT_FALSE(reserve2.reserved());

  ReadTestTask task(*call);
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value() && task.result()->ok());
  EXPECT_EQ(task.result()->value().size(), 5u);

  // Now queue has space again
  ReserveSlotTask reserve3(*call);
  dispatcher.Post(reserve3);
  dispatcher.RunUntilStalled();
  reserve3.Deregister();
  EXPECT_TRUE(reserve3.reserved());

  call->OnMessage(MakePayload(allocator, 3, std::byte(0xCC)));

  task.Deregister();
  connection_task->Deregister();
}

TEST(CallTest, IngressMessageDroppedWhenReadClosed) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 200u, allocator);
  ASSERT_NE(call, nullptr);

  call->CloseReadOnReaderDestroy();
  EXPECT_TRUE(call->is_read_closed());

  // A closed reader never blocks delivery: no slot is needed because the
  // payload is dropped.
  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();
  EXPECT_TRUE(reserve.reserved());

  // Message should be consumed (dropped) without stalling
  call->OnMessage(MakePayload(allocator, 5, std::byte(0xDD)));
  connection_task->Deregister();
}

TEST(CallTest, DeferredResourceResetPostCompletion) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 300u, allocator);
  ASSERT_NE(call, nullptr);

  // Buffer a message before completion
  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakePayload(allocator, 4, std::byte(0x11)));

  // Wire completion occurs
  call->Complete(OkStatus());
  EXPECT_TRUE(call->is_completed());

  // Buffered message can still be drained cleanly
  ReadTestTask task1(*call);
  dispatcher.Post(task1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task1.result().has_value() && task1.result()->ok());
  EXPECT_EQ(task1.result()->value().size(), 4u);
  task1.Deregister();

  // Next read returns completion status (OutOfRange)
  ReadTestTask task2(*call);
  dispatcher.Post(task2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task2.result().has_value());
  EXPECT_EQ(task2.result()->status(), Status::OutOfRange());
  task2.Deregister();

  // Write on OkStatus-completed call fails with FailedPrecondition
  call->CloseWrite();
  EXPECT_TRUE(call->is_write_closed());
  EXPECT_EQ(call->ReserveWrite(0, 8).status(), Status::FailedPrecondition());
  connection_task->Deregister();
}

TEST(CallTest, ReserveWriteReturnsCompletionErrorWhenCompletedWithError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 300u, allocator);
  ASSERT_NE(call, nullptr);

  call->CloseWrite();
  call->Complete(Status::Aborted());
  EXPECT_EQ(call->ReserveWrite(0, 8).status(), Status::Aborted());
  connection_task->Deregister();
}

TEST(CallTest, ReserveWriteReturnsFailedPreconditionWhenOnlyWriteClosed) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 301u, allocator);
  ASSERT_NE(call, nullptr);

  call->CloseWrite();
  EXPECT_TRUE(call->is_write_closed());
  EXPECT_FALSE(call->is_completed());
  EXPECT_EQ(call->ReserveWrite(0, 8).status(), Status::FailedPrecondition());
  connection_task->Deregister();
}

TEST(CallTest, ReserveWriteRejectsWritesLargerThanTransportLimit) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  const size_t max_size = conn.max_write_message_size_bytes();
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 302u, allocator);
  ASSERT_NE(call, nullptr);

  EXPECT_EQ(call->ReserveWrite(0, max_size + 1).status(),
            Status::ResourceExhausted());
  EXPECT_FALSE(call->is_write_closed());
  PW_TEST_EXPECT_OK(call->ReserveWrite(0, max_size).status());

  // Sizes whose sum would overflow are rejected rather than wrapping around.
  EXPECT_EQ(call->ReserveWrite(1, std::numeric_limits<size_t>::max()).status(),
            Status::ResourceExhausted());
  EXPECT_EQ(call->ReserveWrite(max_size + 1, 0).status(),
            Status::ResourceExhausted());
  connection_task->Deregister();
}

TEST(CallTest, CallCreatedOnClosedConnectionUsesConnectionCloseStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  connection_task->CloseConnection(Status::Aborted());
  EXPECT_TRUE(connection_task->is_closed());

  auto call = ClientCall::Create(*connection_task, 302u, allocator);
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), Status::Aborted());
  connection_task->Deregister();
}

TEST(CallTest, FinishFollowedByConnectionClosePreservesOutOfRange) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 303u, allocator);
  ASSERT_NE(call, nullptr);

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakePayload(allocator, 3, std::byte(0x42)));
  call->Complete(OkStatus());

  // Connection closes before the caller drains the final message and EOF.
  connection_task->CloseConnection(Status::Cancelled());

  ReadTestTask first_read(*call);
  dispatcher.Post(first_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(first_read.result().has_value());
  PW_TEST_ASSERT_OK(*first_read.result());
  EXPECT_EQ(first_read.result()->value().size(), 3u);
  first_read.Deregister();

  ReadTestTask eof_read(*call);
  dispatcher.Post(eof_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(eof_read.result().has_value());
  EXPECT_EQ(eof_read.result()->status(), Status::OutOfRange());
  eof_read.Deregister();

  connection_task->Deregister();
}

// A server may half-close its stream before ending the RPC. The client's
// reader stays open until the terminal packet, which carries the status.
TEST(CallTest, PeerStreamEndKeepsClientReaderOpenUntilTerminal) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 303u, allocator);
  ASSERT_NE(call, nullptr);

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakePayload(allocator, 3, std::byte(0x42)));
  call->OnPeerStreamEnd();
  EXPECT_TRUE(call->peer_ended_stream());
  EXPECT_FALSE(call->is_read_closed());
  EXPECT_FALSE(call->is_completed());

  ReadTestTask first_read(*call);
  dispatcher.Post(first_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(first_read.result().has_value());
  PW_TEST_ASSERT_OK(*first_read.result());
  first_read.Deregister();

  ReadTestTask final_read(*call);
  dispatcher.Post(final_read);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(final_read.result().has_value());

  call->Complete(Status::Aborted());
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(final_read.result().has_value());
  EXPECT_EQ(final_read.result()->status(), Status::Aborted());
  final_read.Deregister();

  connection_task->Deregister();
}

TEST(CallTest, PeerStreamEndThenConnectionCloseReportsCloseStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 303u, allocator);
  ASSERT_NE(call, nullptr);

  call->OnPeerStreamEnd();
  // The RPC never finished, so the reader reports why rather than EOF.
  connection_task->CloseConnection(Status::Cancelled());

  ReadTestTask read(*call);
  dispatcher.Post(read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read.result().has_value());
  EXPECT_EQ(read.result()->status(), Status::Cancelled());
  read.Deregister();

  connection_task->Deregister();
}

TEST(CallTest, DroppingReaderUnblocksReserveReadSlot) {
  class BlockingReserveSlotTask : public async2::Task {
   public:
    explicit BlockingReserveSlotTask(Call& call) : call_(call) {}

    async2::Poll<> DoPend(async2::Context& cx) override {
      if (!call_.ReserveMessageSlot(cx)) {
        return async2::Pending();
      }
      reserved_ = true;
      return async2::Ready();
    }

    bool reserved() const { return reserved_; }

   private:
    Call& call_;
    bool reserved_ = false;
  };

  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 304u, allocator);
  ASSERT_NE(call, nullptr);

  std::optional<RawReader> reader = CallAccess::Create<RawReader>(call);

  // Fill the single channel slot.
  ReserveSlotTask reserve1(*call);
  dispatcher.Post(reserve1);
  dispatcher.RunUntilStalled();
  reserve1.Deregister();
  ASSERT_TRUE(reserve1.reserved());
  call->OnMessage(MakePayload(allocator, 4, std::byte(0xAA)));

  // Second reservation stalls because the 1-slot channel is full.
  BlockingReserveSlotTask reserve2(*call);
  dispatcher.Post(reserve2);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(reserve2.reserved());

  // Dropping the Reader closes the read side, frees the buffered payload,
  // and wakes the stalled reservation so ConnectionTask cannot deadlock.
  reader.reset();
  EXPECT_TRUE(call->is_read_closed());

  dispatcher.RunUntilStalled();
  EXPECT_TRUE(reserve2.reserved());
  reserve2.Deregister();

  connection_task->Deregister();
}

TEST(CallTest, ClientCallAutoDetachesOnBothSidesClosed) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 305u, allocator);
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->is_attached());

  call->CloseWrite();
  EXPECT_TRUE(call->is_attached());

  // A server half-close leaves the call waiting for the terminal packet.
  call->OnPeerStreamEnd();
  EXPECT_TRUE(call->is_attached());

  call->CloseReadOnReaderDestroy();
  EXPECT_FALSE(call->is_attached());

  connection_task->Deregister();
}

}  // namespace
}  // namespace pw::rpc2::internal
