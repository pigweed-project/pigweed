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
#include "pw_async2/future_task.h"
#include "pw_rpc2/internal/client_connection_task.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/server_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/reader.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

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

  auto call = ClientCall::Create(*connection_task, 100u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

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

  async2::FutureTask read(reader.Read());
  dispatcher.Post(read);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(read.has_value());
  PW_TEST_ASSERT_OK(read.value());
  EXPECT_EQ(read.value()->size(), 5u);

  // Now queue has space again
  ReserveSlotTask reserve3(*call);
  dispatcher.Post(reserve3);
  dispatcher.RunUntilStalled();
  reserve3.Deregister();
  EXPECT_TRUE(reserve3.reserved());

  call->OnMessage(MakePayload(allocator, 3, std::byte(0xCC)));

  connection_task->Deregister();
}

TEST(CallTest, IngressMessageDroppedWhenReadClosed) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 200u);
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

  auto call = ClientCall::Create(*connection_task, 300u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

  // Buffer a message before completion
  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakePayload(allocator, 4, std::byte(0x11)));

  // Wire completion occurs
  call->Complete(RpcStatus());
  EXPECT_TRUE(call->is_completed());

  // Buffered message can still be drained cleanly
  async2::FutureTask read1(reader.Read());
  dispatcher.Post(read1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read1.has_value());
  PW_TEST_ASSERT_OK(read1.value());
  EXPECT_EQ(read1.value()->size(), 4u);

  // Next read reports the end of the stream (kEndOfStream)
  async2::FutureTask read2(reader.Read());
  dispatcher.Post(read2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read2.has_value());
  EXPECT_EQ(read2.value().status(), RpcStatus::EndOfStream());

  // Write on a call that completed with kOk fails with kWriteClosed
  call->CloseWrite();
  EXPECT_TRUE(call->is_write_closed());
  EXPECT_EQ(call->ReserveWrite(0, 8).status(), RpcStatus::WriteClosed());
  connection_task->Deregister();
}

TEST(CallTest, ReserveWriteReturnsCompletionErrorWhenCompletedWithError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 300u);
  ASSERT_NE(call, nullptr);

  call->CloseWrite();
  call->Complete(RpcStatus::ProtocolError());
  EXPECT_EQ(call->ReserveWrite(0, 8).status(), RpcStatus::ProtocolError());
  connection_task->Deregister();
}

TEST(CallTest, ReserveWriteReturnsWriteClosedWhenOnlyWriteClosed) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 301u);
  ASSERT_NE(call, nullptr);

  call->CloseWrite();
  EXPECT_TRUE(call->is_write_closed());
  EXPECT_FALSE(call->is_completed());
  EXPECT_EQ(call->ReserveWrite(0, 8).status(), RpcStatus::WriteClosed());
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

  auto call = ClientCall::Create(*connection_task, 302u);
  ASSERT_NE(call, nullptr);

  EXPECT_EQ(call->ReserveWrite(0, max_size + 1).status(),
            RpcStatus::MessageTooLarge());
  EXPECT_FALSE(call->is_write_closed());
  PW_TEST_EXPECT_OK(call->ReserveWrite(0, max_size).status());

  // Sizes whose sum would overflow are rejected rather than wrapping around.
  EXPECT_EQ(call->ReserveWrite(1, std::numeric_limits<size_t>::max()).status(),
            RpcStatus::MessageTooLarge());
  EXPECT_EQ(call->ReserveWrite(max_size + 1, 0).status(),
            RpcStatus::MessageTooLarge());
  connection_task->Deregister();
}

TEST(CallTest, CallCreatedOnClosedConnectionUsesConnectionCloseStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  connection_task->CloseConnection(RpcStatus::ProtocolError());
  EXPECT_TRUE(connection_task->is_closed());

  auto call = ClientCall::Create(*connection_task, 302u);
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());
  connection_task->Deregister();
}

TEST(CallTest, FinishFollowedByConnectionClosePreservesEndOfStream) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 303u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakePayload(allocator, 3, std::byte(0x42)));
  call->Complete(RpcStatus());

  // Connection closes before the caller drains the final message and EOF.
  connection_task->CloseConnection(RpcStatus::Closed());

  async2::FutureTask first_read(reader.Read());
  dispatcher.Post(first_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(first_read.has_value());
  PW_TEST_ASSERT_OK(first_read.value());
  EXPECT_EQ(first_read.value()->size(), 3u);

  async2::FutureTask eof_read(reader.Read());
  dispatcher.Post(eof_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(eof_read.has_value());
  EXPECT_EQ(eof_read.value().status(), RpcStatus::EndOfStream());

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

  auto call = ClientCall::Create(*connection_task, 303u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  call->OnMessage(MakePayload(allocator, 3, std::byte(0x42)));
  call->OnPeerStreamEnd();
  EXPECT_TRUE(call->peer_ended_stream());
  EXPECT_FALSE(call->is_read_closed());
  EXPECT_FALSE(call->is_completed());

  async2::FutureTask first_read(reader.Read());
  dispatcher.Post(first_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(first_read.has_value());
  PW_TEST_ASSERT_OK(first_read.value());

  async2::FutureTask final_read(reader.Read());
  dispatcher.Post(final_read);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(final_read.has_value());

  call->Complete(RpcStatus::ProtocolError());
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(final_read.has_value());
  EXPECT_EQ(final_read.value().status(), RpcStatus::ProtocolError());

  connection_task->Deregister();
}

TEST(CallTest, PeerStreamEndThenConnectionCloseReportsCloseStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 303u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

  call->OnPeerStreamEnd();
  // The RPC never finished, so the reader reports why rather than EOF.
  connection_task->CloseConnection(RpcStatus::Closed());

  async2::FutureTask read(reader.Read());
  dispatcher.Post(read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read.value().status(), RpcStatus::Closed());

  connection_task->Deregister();
}

TEST(CallTest, ServerPeerStreamEndThenConnectionClosePreservesEndOfStream) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ServerConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator, server);
  ASSERT_NE(connection_task, nullptr);

  auto call = test::StartServerCall(*connection_task, 303u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();

  // Unlike a client call, a server call's incoming stream ends cleanly once the
  // client half-closes it, even if the call later fails before the server
  // drains the stream.
  call->OnMessage(MakePayload(allocator, 3, std::byte(0x42)));
  call->OnPeerStreamEnd();
  connection_task->CloseConnection(RpcStatus::Closed());

  async2::FutureTask first_read(reader.Read());
  dispatcher.Post(first_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(first_read.has_value());
  PW_TEST_ASSERT_OK(first_read.value());
  EXPECT_EQ(first_read.value()->size(), 3u);

  async2::FutureTask eof_read(reader.Read());
  dispatcher.Post(eof_read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(eof_read.has_value());
  EXPECT_EQ(eof_read.value().status(), RpcStatus::EndOfStream());

  connection_task->Deregister();
}

TEST(CallTest, ServerConnectionCloseWithoutPeerStreamEndReportsCloseStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<ServerConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator, server);
  ASSERT_NE(connection_task, nullptr);

  auto call = test::StartServerCall(*connection_task, 303u);
  ASSERT_NE(call, nullptr);
  RawReader reader = CallAccess::Create<RawReader>(call);

  connection_task->CloseConnection(RpcStatus::Closed());

  async2::FutureTask read(reader.Read());
  dispatcher.Post(read);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read.value().status(), RpcStatus::Closed());

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

  auto call = ClientCall::Create(*connection_task, 304u);
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

  auto call = ClientCall::Create(*connection_task, 305u);
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

TEST(CallTest, CallOutlivesItsConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<ClientConnectionTask>(
      EstablishedConnection{std::move(conn)}, allocator);
  ASSERT_NE(connection_task, nullptr);

  auto call = ClientCall::Create(*connection_task, 306u);
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(call->is_attached());

  // Destroying the connection detaches the call.
  connection_task = nullptr;

  EXPECT_FALSE(call->is_attached());
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::Closed());
}

}  // namespace
}  // namespace pw::rpc2::internal
