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

#include "pw_rpc2/internal/connection_task.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <variant>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_bytes/endian.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/client_connection_task.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/server.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

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
    } else {
      Status status = call_.completion_status();
      result_ = (call_.is_completed() && !status.ok()) ? status
                                                       : Status::OutOfRange();
    }
    return async2::Ready();
  }

  const std::optional<Result<pw::ConstBuf>>& result() const { return result_; }

 private:
  async2::ReceiveFuture<pw::ConstBuf> receive_fut_;
  Call& call_;
  bool released_ = false;
  std::optional<Result<pw::ConstBuf>> result_;
};

TEST(ConnectionTaskTest, IngressRequestPacketIsDispatchedToTheServer) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->written_packet_count(), 0u);

  std::byte payload_data[4] = {
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  auto pkt_res = PacketFramer::FrameStartUnaryPacket(allocator,
                                                     /*call_id=*/42,
                                                     /*service_id=*/10,
                                                     /*method_id=*/20,
                                                     payload_data);
  ASSERT_TRUE(pkt_res.ok());
  raw_conn->SetNextRead(std::move(pkt_res.value()));

  task.Wake();
  dispatcher.RunUntilStalled();

  // No service 10 is registered, so dispatch rejects the call. The error
  // packet is the observable evidence that the request reached the server.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto response =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(response->call_id(), 42u);
  EXPECT_EQ(response->error(), ProtocolStatus::kUnknownService);

  task.Deregister();
}

TEST(ConnectionTaskTest, ProcessOutgoingErrorsFastPathSynchronous) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);

  // Fast-path: immediate synchronous transmission when transport write is
  // available
  task.QueueError(100u, ProtocolStatus::kReceivedPacketForWrongEndpoint);

  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res = InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res->call_id(), 100u);
  EXPECT_EQ(decode_res->error(),
            ProtocolStatus::kReceivedPacketForWrongEndpoint);
}

TEST(ConnectionTaskTest, ProcessOutgoingErrorsFallbackQueueAndAsyncFlush) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);

  // Simulate transport write backpressure
  raw_conn->SetBlockReserveWrite(true);

  // QueueError should fall back to internal queue
  task.QueueError(101u, ProtocolStatus::kCancelled);
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  dispatcher.Post(task);

  // Poll 1: ConnectionTask attempts to flush error but transport is still
  // blocked
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  // Unblock transport write
  raw_conn->SetBlockReserveWrite(false);
  raw_conn->UnblockReserveWrite(sizeof(ErrorWireFormat));

  // Poll 2: ConnectionTask finishes reservation and commits error frame
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res = InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res->call_id(), 101u);
  EXPECT_EQ(decode_res->error(), ProtocolStatus::kCancelled);

  task.Deregister();
}

TEST(ConnectionTaskTest, ProcessOutgoingErrorsQueuesUnboundedBacklog) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);

  // Block transport writes so every packet has to be queued.
  raw_conn->SetBlockReserveWrite(true);

  const size_t baseline_allocated = allocator.metrics().allocated_bytes.value();

  // Queue a backlog well beyond what a fixed-size queue used to hold. Every
  // one of these is terminal for its call, so none may be dropped.
  constexpr uint32_t kBacklog = 8;
  for (uint32_t i = 1; i <= kBacklog; ++i) {
    task.QueueError(i, ProtocolStatus::kCancelled);
  }
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_GT(allocator.metrics().allocated_bytes.value(), baseline_allocated);

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  // Unblocking the transport flushes the whole backlog, in order.
  raw_conn->SetBlockReserveWrite(false);
  dispatcher.RunUntilStalled();
  ASSERT_EQ(raw_conn->commit_count(), size_t{kBacklog});

  for (uint32_t i = 0; i < kBacklog; ++i) {
    auto decode_res =
        InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(i)));
    ASSERT_TRUE(decode_res.ok());
    EXPECT_EQ(decode_res->type(), PacketType::Make<flags::kErrorTerminal>());
    EXPECT_EQ(decode_res->call_id(), i + 1);
    EXPECT_EQ(decode_res->error(), ProtocolStatus::kCancelled);
  }

  // Once drained, the control packet queue frees its buffer because its
  // capacity exceeded kMaxIdleControlPacketCapacity. Clearing the mock
  // connection's recorded packet buffers returns allocation back to baseline.
  raw_conn->clear_written();
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), baseline_allocated);

  task.Deregister();
}

TEST(ConnectionTaskTest, DemuxStreamingMessagesToActiveCallWithBackpressure) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  auto task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(task, nullptr);

  auto call = ClientCall::Create(*task, 42u, allocator);
  ASSERT_NE(call, nullptr);

  // Step 1: Inject message 1
  std::byte msg1_data[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt1 = PacketFramer::FrameServerMessagePacket(allocator, 42u, msg1_data);
  ASSERT_TRUE(pkt1.ok());
  raw_conn->SetNextRead(std::move(*pkt1));
  task->Wake();
  dispatcher.RunUntilStalled();

  // Message 1 is now in the SPSC channel, which holds one message, so nothing
  // can be delivered until it is drained.
  ReserveSlotTask reserve(*call);
  dispatcher.Post(reserve);
  dispatcher.RunUntilStalled();
  reserve.Deregister();
  EXPECT_FALSE(reserve.reserved());

  // Step 2: Inject message 2
  std::byte msg2_data[2] = {std::byte{10}, std::byte{20}};
  auto pkt2 = PacketFramer::FrameServerMessagePacket(allocator, 42u, msg2_data);
  ASSERT_TRUE(pkt2.ok());
  raw_conn->SetNextRead(std::move(*pkt2));
  task->Wake();
  dispatcher.RunUntilStalled();

  // Step 3: Drain message 1 via reader task
  ReadTestTask read_task1(*call);
  dispatcher.Post(read_task1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task1.result().has_value() && read_task1.result()->ok());
  EXPECT_EQ(read_task1.result()->value().size(), 3u);
  read_task1.Deregister();

  // Step 4: Wake task and allow DispatchPendingIngressPacket to deliver message
  // 2
  task->Wake();
  dispatcher.RunUntilStalled();

  // Step 5: Drain message 2
  ReadTestTask read_task2(*call);
  dispatcher.Post(read_task2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task2.result().has_value() && read_task2.result()->ok());
  EXPECT_EQ(read_task2.result()->value().size(), 2u);
  read_task2.Deregister();

  task->Deregister();
}

TEST(ConnectionTaskTest, DuplicateStartCancelsActiveCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  // Register active call 50
  constexpr Method method(1, MethodType::kUnary, 0, nullptr);
  auto server_call = ServerCall::Allocate(task, 50u, method, allocator);
  ASSERT_TRUE(server_call.ok());
  IntrusivePtr<Call> call = (*server_call)->shared_call();

  // Send a duplicate request packet with call_id = 50
  std::byte payload[2] = {std::byte{1}, std::byte{2}};
  auto pkt = PacketFramer::FrameStartUnaryPacket(
      allocator, 50u, /*service_id=*/10, /*method_id=*/20, payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // The duplicate is not dispatched: had it reached the server, the
  // unregistered service would have produced a NOT_FOUND error packet.
  // Instead, the active call is cancelled and the client is told once.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_TRUE(reply.ok());
  EXPECT_EQ(reply->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(reply->call_id(), 50u);
  EXPECT_EQ(reply->error(), ProtocolStatus::kCancelled);
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), Status::Cancelled());

  task.Deregister();
}

TEST(ConnectionTaskTest, WrongDirectionPacketsAreRejected) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  auto call = ClientCall::Create(task, 50u, allocator);
  ASSERT_NE(call, nullptr);

  // 1. A client receiving a request packet replies with
  // kReceivedPacketForWrongEndpoint and fails any matching local call.
  std::byte payload[2] = {std::byte{1}, std::byte{2}};
  auto req_pkt = PacketFramer::FrameStartUnaryPacket(
      allocator, 50u, /*service_id=*/10, /*method_id=*/20, payload);
  ASSERT_TRUE(req_pkt.ok());
  raw_conn->PushNextRead(std::move(*req_pkt));

  // 2. Client-to-server packets (messages, stream ends, and errors)
  // arriving at a client are dropped. Each non-terminal one is answered with
  // kReceivedPacketForWrongEndpoint; the error is terminal, so it gets no
  // reply.
  auto msg_pkt =
      PacketFramer::FrameClientMessagePacket(allocator, 50u, payload);
  ASSERT_TRUE(msg_pkt.ok());
  raw_conn->PushNextRead(std::move(*msg_pkt));

  auto end_pkt = PacketFramer::FrameClientStreamEndPacket(allocator, 50u);
  ASSERT_TRUE(end_pkt.ok());
  raw_conn->PushNextRead(std::move(*end_pkt));

  auto err_pkt = PacketFramer::FrameClientErrorPacket(
      allocator, 50u, ProtocolStatus::kCancelled);
  ASSERT_TRUE(err_pkt.ok());
  raw_conn->PushNextRead(std::move(*err_pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // The request, message, and stream end were each answered with
  // kReceivedPacketForWrongEndpoint, and the matching local call was completed
  // with UNIMPLEMENTED.
  ASSERT_EQ(raw_conn->written_packet_count(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    auto reply =
        InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(i)));
    ASSERT_TRUE(reply.ok());
    EXPECT_EQ(reply->type(), PacketType::Make<flags::kErrorTerminal>());
    EXPECT_EQ(reply->call_id(), 50u);
    EXPECT_EQ(reply->error(), ProtocolStatus::kReceivedPacketForWrongEndpoint);
  }
  EXPECT_TRUE(call->is_closed());
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), Status::Unimplemented());
  EXPECT_FALSE(call->peer_ended_stream());

  task.Deregister();
}

TEST(ConnectionTaskTest, StrayPacketForUnknownCallIsCancelled) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  // A well-formed message packet for a call that does not exist.
  std::byte msg_data[2] = {std::byte{1}, std::byte{2}};
  auto pkt = PacketFramer::FrameServerMessagePacket(allocator, 9999u, msg_data);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // The packet is dropped and the server is told to stop sending for the
  // call. The connection stays up, since a stray packet is a normal race
  // against call teardown.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_TRUE(reply.ok());
  EXPECT_EQ(reply->type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(reply->call_id(), 9999u);
  EXPECT_EQ(reply->error(), ProtocolStatus::kCancelled);
  EXPECT_FALSE(raw_conn->is_closed());

  task.Deregister();
}

TEST(ConnectionTaskTest, StrayTerminalPacketGetsNoReply) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  // Terminal packets for a call that does not exist: a response and an
  // error. The peer has already forgotten the call, so neither is answered.
  std::byte msg_data[2] = {std::byte{1}, std::byte{2}};
  auto response = PacketFramer::FrameResponsePacket(allocator, 9999u, msg_data);
  ASSERT_TRUE(response.ok());
  raw_conn->PushNextRead(std::move(*response));
  auto error = PacketFramer::FrameServerErrorPacket(
      allocator, 9998u, ProtocolStatus::kCancelled);
  ASSERT_TRUE(error.ok());
  raw_conn->PushNextRead(std::move(*error));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->written_packet_count(), 0u);
  EXPECT_FALSE(raw_conn->is_closed());

  task.Deregister();
}

TEST(ConnectionTaskTest, ServerCancelsStrayClientStream) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  // A client message for a call the server has already finished.
  std::byte msg_data[2] = {std::byte{1}, std::byte{2}};
  auto pkt = PacketFramer::FrameClientMessagePacket(allocator, 77u, msg_data);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_TRUE(reply.ok());
  EXPECT_EQ(reply->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(reply->call_id(), 77u);
  EXPECT_EQ(reply->error(), ProtocolStatus::kCancelled);

  task.Deregister();
}

TEST(ConnectionTaskTest, ServerRejectsServerToClientPackets) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  // A server message arriving at a server is answered with
  // kReceivedPacketForWrongEndpoint; a server error is terminal, so it is not.
  std::byte msg_data[2] = {std::byte{1}, std::byte{2}};
  auto msg = PacketFramer::FrameServerMessagePacket(allocator, 5u, msg_data);
  ASSERT_TRUE(msg.ok());
  raw_conn->PushNextRead(std::move(*msg));
  auto error = PacketFramer::FrameServerErrorPacket(
      allocator, 6u, ProtocolStatus::kCancelled);
  ASSERT_TRUE(error.ok());
  raw_conn->PushNextRead(std::move(*error));

  task.Wake();
  dispatcher.RunUntilStalled();

  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_TRUE(reply.ok());
  EXPECT_EQ(reply->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(reply->call_id(), 5u);
  EXPECT_EQ(reply->error(), ProtocolStatus::kReceivedPacketForWrongEndpoint);

  task.Deregister();
}

TEST(ConnectionTaskTest, UnrecognizedPacketTypeClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  // A complete 5-byte header whose type byte is not a valid PacketType for the
  // negotiated protocol version.
  pw::Buf unknown_type = pw::Buf::Allocate(allocator, sizeof(PacketHeader));
  ASSERT_EQ(unknown_type.size(), sizeof(PacketHeader));
  std::memset(unknown_type.data(), 0, unknown_type.size());
  unknown_type[offsetof(PacketHeader, type)] = std::byte{0x7F};
  raw_conn->SetNextRead(std::move(unknown_type));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_TRUE(raw_conn->is_closed());

  task.Deregister();
}

TEST(ConnectionTaskTest, MalformedPacketClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  // A complete frame that is shorter than the packet header. The peer is not
  // speaking the protocol, so the connection is torn down.
  pw::Buf corrupt_buf = pw::Buf::Allocate(allocator, 3);
  corrupt_buf[0] = std::byte{0xFF};
  corrupt_buf[1] = std::byte{0xEE};
  corrupt_buf[2] = std::byte{0xDD};
  raw_conn->SetNextRead(std::move(corrupt_buf));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(raw_conn->is_closed());

  task.Deregister();
}

TEST(ConnectionTaskTest, SendControlPacketFutureEncapsulation) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  class SendControlPacketTestTask : public async2::Task {
   public:
    explicit SendControlPacketTestTask(SendControlPacketFuture fut)
        : fut_(std::move(fut)) {}
    async2::Poll<> DoPend(async2::Context& cx) override {
      auto poll = fut_.Pend(cx);
      if (poll.IsPending()) {
        return async2::Pending();
      }
      status_ = *poll;
      return async2::Ready();
    }
    std::optional<Status> status() const { return status_; }

   private:
    SendControlPacketFuture fut_;
    std::optional<Status> status_;
  };

  raw_conn->SetBlockReserveWrite(true);
  SendControlPacketFuture fut(
      conn, OutboundPacket::ClientError(88u, ProtocolStatus::kCancelled));
  EXPECT_TRUE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());

  SendControlPacketTestTask task(std::move(fut));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_FALSE(task.status().has_value());

  raw_conn->SetBlockReserveWrite(false);
  raw_conn->UnblockReserveWrite(sizeof(ErrorWireFormat));
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 1u);
  ASSERT_TRUE(task.status().has_value());
  EXPECT_EQ(*task.status(), OkStatus());

  auto decode_res = InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res->call_id(), 88u);
  EXPECT_EQ(decode_res->error(), ProtocolStatus::kCancelled);

  task.Deregister();
}

TEST(ConnectionTaskTest, ConnectionCloseNotifiesActiveCalls) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  auto task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(task, nullptr);
  auto call = ClientCall::Create(*task, 123u, allocator);
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(call->is_closed());
  task->CloseConnection(Status::Aborted());

  EXPECT_TRUE(task->is_closed());
  EXPECT_TRUE(call->is_closed());
  EXPECT_EQ(call->completion_status(), Status::Aborted());
  task->Deregister();
}

// An error code this build does not recognize, e.g. from a newer peer, maps to
// `Status::Unknown()`. It must still complete the call, and a pending read must
// report the error rather than a clean end of stream.
TEST(ConnectionTaskTest, UnrecognizedErrorCodeCompletesCallWithError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  auto call = ClientCall::Create(task, 7u, allocator);
  ASSERT_NE(call, nullptr);
  ReadTestTask read_task(*call);
  dispatcher.Post(read_task);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(read_task.result().has_value());

  auto pkt = PacketFramer::FrameServerErrorPacket(
      allocator, 7u, static_cast<ProtocolStatus>(200));
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));
  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(call->is_completed());
  EXPECT_TRUE(call->is_closed());
  EXPECT_EQ(call->completion_status(), Status::Unknown());
  ASSERT_TRUE(read_task.result().has_value());
  EXPECT_EQ(read_task.result()->status(), Status::Unknown());

  read_task.Deregister();
  task.Deregister();
}

TEST(ConnectionTaskTest, ServerRoleHandshake) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(conn, allocator, server);
  EXPECT_FALSE(task.is_handshake_complete());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.is_handshake_complete());

  // Step 1: Inject client handshake SYN packet
  Result<Buf> client_syn =
      HandshakePacket(HandshakePacket::Type::kSyn)
          .Encode(Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes));
  ASSERT_TRUE(client_syn.ok());
  raw_conn->SetNextRead(std::move(*client_syn));

  task.Wake();
  dispatcher.RunUntilStalled();

  // Server processed SYN, sent SYN-ACK, but is awaiting ACK
  EXPECT_FALSE(task.is_handshake_complete());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Step 2: Inject client handshake ACK packet
  Result<Buf> client_ack =
      HandshakePacket(HandshakePacket::Type::kAck)
          .Encode(Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes));
  ASSERT_TRUE(client_ack.ok());
  raw_conn->SetNextRead(std::move(*client_ack));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(task.is_handshake_complete());
  EXPECT_EQ(task.handshake_info().negotiated_version, 1u);

  task.Deregister();
}

TEST(ClientConnectionTaskTest, AllocatesSequentialCallIds) {
  allocator::test::AllocatorForTest<4096> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn, HandshakeInfo{}},
                            allocator);
  EXPECT_TRUE(task.is_handshake_complete());
  EXPECT_EQ(task.NewCallId(), 1u);
  EXPECT_EQ(task.NewCallId(), 2u);
  EXPECT_EQ(task.NewCallId(), 3u);
}

}  // namespace
}  // namespace pw::rpc2::internal
