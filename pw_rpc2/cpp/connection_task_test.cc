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
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <variant>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_bytes/endian.h"
#include "pw_enum/to_string.h"
#include "pw_enum/traits.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/client_connection_task.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/protocol_error.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/server.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

class ReadTestTask : public async2::Task {
 public:
  explicit ReadTestTask(Call& call) : call_(call) { call_.ClaimRead(); }

  ~ReadTestTask() override {
    if (!released_) {
      call_.ReleaseRead();
    }
  }

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = call_.PendRead(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    call_.ReleaseRead();
    released_ = true;
    result_ = std::move(*poll);
    return async2::Ready();
  }

  const std::optional<RpcResult<pw::ConstBuf>>& result() const {
    return result_;
  }

 private:
  Call& call_;
  bool released_ = false;
  std::optional<RpcResult<pw::ConstBuf>> result_;
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
  Buf pkt = PacketFramer::FrameStartUnaryPacket(allocator,
                                                /*call_id=*/42,
                                                /*service_id=*/10,
                                                /*method_id=*/20,
                                                payload_data);
  ASSERT_NE(pkt, nullptr);
  raw_conn->SetNextRead(std::move(pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // No service 10 is registered, so dispatch rejects the call. The error
  // packet is the observable evidence that the request reached the server.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto response =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response.type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(response.call_id(), 42u);
  EXPECT_EQ(response.error(), ProtocolError::kUnknownService);

  task.Deregister();
}

TEST(ConnectionTaskTest, ProcessOutgoingErrorsFastPathSynchronous) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);

  // Fast-path: immediate synchronous transmission when transport write is
  // available
  task.QueueError(100u, ProtocolError::kReceivedPacketForWrongEndpoint);

  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res = InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res.call_id(), 100u);
  EXPECT_EQ(decode_res.error(), ProtocolError::kReceivedPacketForWrongEndpoint);
}

TEST(ConnectionTaskTest, ProcessOutgoingErrorsFallbackQueueAndAsyncFlush) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);

  // Simulate transport write backpressure
  raw_conn->SetBlockReserveWrite(true);

  // QueueError should fall back to internal queue
  task.QueueError(101u, ProtocolError::kCancelled);
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
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res.call_id(), 101u);
  EXPECT_EQ(decode_res.error(), ProtocolError::kCancelled);

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
    task.QueueError(i, ProtocolError::kCancelled);
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
    ASSERT_NE(decode_res, nullptr);
    EXPECT_EQ(decode_res.type(), PacketType::Make<flags::kErrorTerminal>());
    EXPECT_EQ(decode_res.call_id(), i + 1);
    EXPECT_EQ(decode_res.error(), ProtocolError::kCancelled);
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

  auto call = ClientCall::Create(*task, 42u);
  ASSERT_NE(call, nullptr);

  // Step 1: Inject message 1
  std::byte msg1_data[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt1 = PacketFramer::FrameServerMessagePacket(allocator, 42u, msg1_data);
  ASSERT_NE(pkt1, nullptr);
  raw_conn->SetNextRead(std::move(pkt1));
  task->Wake();
  dispatcher.RunUntilStalled();

  // Message 1 is now in the call's buffer, which holds one message, so nothing
  // can be delivered until it is drained.
  EXPECT_TRUE(call->has_buffered_message());

  // Step 2: Inject message 2, which the connection task stashes because the
  // call's buffer is full.
  std::byte msg2_data[2] = {std::byte{10}, std::byte{20}};
  auto pkt2 = PacketFramer::FrameServerMessagePacket(allocator, 42u, msg2_data);
  ASSERT_NE(pkt2, nullptr);
  raw_conn->SetNextRead(std::move(pkt2));
  task->Wake();
  dispatcher.RunUntilStalled();

  // Step 3: Drain message 1 via reader task
  ReadTestTask read_task1(*call);
  dispatcher.Post(read_task1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task1.result().has_value() && read_task1.result()->ok());
  EXPECT_EQ(read_task1.result()->value().size(), 3u);
  read_task1.Deregister();

  // Draining the buffer wakes the connection task, which delivers the stashed
  // message 2 without any other prompting.
  EXPECT_TRUE(call->has_buffered_message());

  // Step 4: Drain message 2
  ReadTestTask read_task2(*call);
  dispatcher.Post(read_task2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task2.result().has_value() && read_task2.result()->ok());
  EXPECT_EQ(read_task2.result()->value().size(), 2u);
  read_task2.Deregister();

  task->Deregister();
}

TEST(ConnectionTaskTest, ClosingReaderUnblocksStalledIngress) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  auto task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(task, nullptr);

  auto blocked_call = ClientCall::Create(*task, 42u);
  ASSERT_NE(blocked_call, nullptr);
  auto other_call = ClientCall::Create(*task, 43u);
  ASSERT_NE(other_call, nullptr);

  // Fill the blocked call's buffer, then stash a second message for it, which
  // stalls all ingress on the connection.
  std::byte data[2] = {std::byte{1}, std::byte{2}};
  auto pkt1 = PacketFramer::FrameServerMessagePacket(allocator, 42u, data);
  ASSERT_NE(pkt1, nullptr);
  raw_conn->SetNextRead(std::move(pkt1));
  task->Wake();
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(blocked_call->has_buffered_message());

  auto pkt2 = PacketFramer::FrameServerMessagePacket(allocator, 42u, data);
  ASSERT_NE(pkt2, nullptr);
  raw_conn->SetNextRead(std::move(pkt2));
  task->Wake();
  dispatcher.RunUntilStalled();

  // A message for another call waits behind the stashed packet.
  auto pkt3 = PacketFramer::FrameServerMessagePacket(allocator, 43u, data);
  ASSERT_NE(pkt3, nullptr);
  raw_conn->SetNextRead(std::move(pkt3));
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(other_call->has_buffered_message());

  // Dropping the blocked call's reader discards its messages and wakes the
  // connection task, which drops the stashed packet and resumes reading, so
  // the other call receives its message.
  blocked_call->CloseReadOnReaderDestroy();
  EXPECT_TRUE(blocked_call->is_read_closed());
  EXPECT_FALSE(blocked_call->has_buffered_message());
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(other_call->has_buffered_message());

  task->Deregister();
}

TEST(ConnectionTaskTest, DuplicateStartEndsActiveCallWithProtocolError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  // Register active call 50
  constexpr Method method(1, MethodType::kUnary, 0, nullptr);
  ServerCall* server_call = ServerCall::Allocate(task, 50u, method);
  ASSERT_NE(server_call, nullptr);
  IntrusivePtr<Call> call = server_call->shared_call();

  // Send a duplicate request packet with call_id = 50
  std::byte payload[2] = {std::byte{1}, std::byte{2}};
  auto pkt = PacketFramer::FrameStartUnaryPacket(
      allocator, 50u, /*service_id=*/10, /*method_id=*/20, payload);
  ASSERT_NE(pkt, nullptr);
  raw_conn->SetNextRead(std::move(pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // The duplicate is not dispatched: had it reached the server, the
  // unregistered service would have produced a kUnknownService error packet.
  // The client reused an active call ID, which is a client bug, so the active
  // call ends with a protocol error and the client is told once.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ(reply.type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(reply.call_id(), 50u);
  EXPECT_EQ(reply.error(), ProtocolError::kReceivedDuplicateStart);
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());

  // Only the two calls end. The connection stays up for the client's other
  // calls.
  EXPECT_FALSE(task.is_closed());
  EXPECT_FALSE(raw_conn->is_closed());

  task.Deregister();
}

TEST(ConnectionTaskTest, WrongDirectionPacketsAreRejected) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  auto call = ClientCall::Create(task, 50u);
  ASSERT_NE(call, nullptr);

  // 1. A client receiving a request packet replies with
  // kReceivedPacketForWrongEndpoint and fails any matching local call.
  std::byte payload[2] = {std::byte{1}, std::byte{2}};
  auto req_pkt = PacketFramer::FrameStartUnaryPacket(
      allocator, 50u, /*service_id=*/10, /*method_id=*/20, payload);
  ASSERT_NE(req_pkt, nullptr);
  raw_conn->PushNextRead(std::move(req_pkt));

  // 2. Client-to-server packets (messages, stream ends, and errors)
  // arriving at a client are dropped. Each non-terminal one is answered with
  // kReceivedPacketForWrongEndpoint; the error is terminal, so it gets no
  // reply.
  auto msg_pkt =
      PacketFramer::FrameClientMessagePacket(allocator, 50u, payload);
  ASSERT_NE(msg_pkt, nullptr);
  raw_conn->PushNextRead(std::move(msg_pkt));

  auto end_pkt = PacketFramer::FrameClientStreamEndPacket(allocator, 50u);
  ASSERT_NE(end_pkt, nullptr);
  raw_conn->PushNextRead(std::move(end_pkt));

  auto err_pkt = PacketFramer::FrameClientErrorPacket(
      allocator, 50u, ProtocolError::kCancelled);
  ASSERT_NE(err_pkt, nullptr);
  raw_conn->PushNextRead(std::move(err_pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // The request, message, and stream end were each answered with
  // kReceivedPacketForWrongEndpoint, and the matching local call was completed
  // with a protocol error.
  ASSERT_EQ(raw_conn->written_packet_count(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    auto reply =
        InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(i)));
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply.type(), PacketType::Make<flags::kErrorTerminal>());
    EXPECT_EQ(reply.call_id(), 50u);
    EXPECT_EQ(reply.error(), ProtocolError::kReceivedPacketForWrongEndpoint);
  }
  EXPECT_TRUE(call->is_closed());
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());
  EXPECT_FALSE(call->peer_ended_stream());

  task.Deregister();
}

TEST(ConnectionTaskTest, StrayPacketForUnknownCallIsRejected) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  // A well-formed message packet for a call that does not exist.
  std::byte msg_data[2] = {std::byte{1}, std::byte{2}};
  auto pkt = PacketFramer::FrameServerMessagePacket(allocator, 9999u, msg_data);
  ASSERT_NE(pkt, nullptr);
  raw_conn->SetNextRead(std::move(pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  // The packet is dropped and the server is told that the call is unknown.
  // The connection stays up, since a stray packet is a normal race against
  // call teardown.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ(reply.type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(reply.call_id(), 9999u);
  EXPECT_EQ(reply.error(), ProtocolError::kReceivedPacketForUnknownCall);
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
  ASSERT_NE(response, nullptr);
  raw_conn->PushNextRead(std::move(response));
  auto error = PacketFramer::FrameServerErrorPacket(
      allocator, 9998u, ProtocolError::kCancelled);
  ASSERT_NE(error, nullptr);
  raw_conn->PushNextRead(std::move(error));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->written_packet_count(), 0u);
  EXPECT_FALSE(raw_conn->is_closed());

  task.Deregister();
}

TEST(ConnectionTaskTest, ServerRejectsStrayClientStream) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  // A client message for a call the server has already finished.
  std::byte msg_data[2] = {std::byte{1}, std::byte{2}};
  auto pkt = PacketFramer::FrameClientMessagePacket(allocator, 77u, msg_data);
  ASSERT_NE(pkt, nullptr);
  raw_conn->SetNextRead(std::move(pkt));

  task.Wake();
  dispatcher.RunUntilStalled();

  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ(reply.type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(reply.call_id(), 77u);
  EXPECT_EQ(reply.error(), ProtocolError::kReceivedPacketForUnknownCall);
  EXPECT_FALSE(raw_conn->is_closed());

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
  ASSERT_NE(msg, nullptr);
  raw_conn->PushNextRead(std::move(msg));
  auto error = PacketFramer::FrameServerErrorPacket(
      allocator, 6u, ProtocolError::kCancelled);
  ASSERT_NE(error, nullptr);
  raw_conn->PushNextRead(std::move(error));

  task.Wake();
  dispatcher.RunUntilStalled();

  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto reply =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ(reply.type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(reply.call_id(), 5u);
  EXPECT_EQ(reply.error(), ProtocolError::kReceivedPacketForWrongEndpoint);

  task.Deregister();
}

TEST(ConnectionTaskTest, UnrecognizedPacketTypeClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);
  auto call = ClientCall::Create(task, 1u);
  ASSERT_NE(call, nullptr);

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
  EXPECT_EQ(task.close_status(), RpcStatus::ProtocolError());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());

  task.Deregister();
}

TEST(ConnectionTaskTest, MalformedPacketClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);
  auto call = ClientCall::Create(task, 1u);
  ASSERT_NE(call, nullptr);

  // A complete frame that is shorter than the packet header. The peer is not
  // speaking the protocol, so the connection is torn down. Reconnecting would
  // not fix that, so the connection's calls report a protocol error rather
  // than a disconnection.
  pw::Buf corrupt_buf = pw::Buf::Allocate(allocator, 3);
  corrupt_buf[0] = std::byte{0xFF};
  corrupt_buf[1] = std::byte{0xEE};
  corrupt_buf[2] = std::byte{0xDD};
  raw_conn->SetNextRead(std::move(corrupt_buf));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(raw_conn->is_closed());
  EXPECT_EQ(task.close_status(), RpcStatus::ProtocolError());
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());

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
    std::optional<RpcStatus> status() const { return status_; }

   private:
    SendControlPacketFuture fut_;
    std::optional<RpcStatus> status_;
  };

  raw_conn->SetBlockReserveWrite(true);
  SendControlPacketFuture fut(
      conn, OutboundPacket::ClientError(88u, ProtocolError::kCancelled));
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
  PW_TEST_EXPECT_OK(*task.status());

  auto decode_res = InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(), PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res.call_id(), 88u);
  EXPECT_EQ(decode_res.error(), ProtocolError::kCancelled);

  task.Deregister();
}

TEST(ConnectionTaskTest, ConnectionCloseNotifiesActiveCalls) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  auto task = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator);
  ASSERT_NE(task, nullptr);
  auto call = ClientCall::Create(*task, 123u);
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(call->is_closed());
  task->CloseConnection(RpcStatus::ProtocolError());

  EXPECT_TRUE(task->is_closed());
  EXPECT_TRUE(call->is_closed());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());
  EXPECT_EQ(task->close_status(), RpcStatus::ProtocolError());
  task->Deregister();
}

// A peer only sends error codes that the negotiated protocol version defines,
// so a code this endpoint does not recognize is a protocol violation. It must
// still complete the call, and a pending read must report the error rather
// than a clean end of stream.
TEST(ConnectionTaskTest, UnrecognizedErrorCodeCompletesCallWithError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);

  auto call = ClientCall::Create(task, 7u);
  ASSERT_NE(call, nullptr);
  ReadTestTask read_task(*call);
  dispatcher.Post(read_task);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(read_task.result().has_value());

  // The code after the last server-origin code is unassigned.
  constexpr auto kUnassignedServerCode = static_cast<ProtocolError>(
      static_cast<uint8_t>(ProtocolError::kMethodReturnedWithoutFinishing) + 1);
  static_assert(!pw::IsValidEnum(kUnassignedServerCode));
  auto pkt = PacketFramer::FrameServerErrorPacket(
      allocator, 7u, kUnassignedServerCode);
  ASSERT_NE(pkt, nullptr);
  raw_conn->SetNextRead(std::move(pkt));
  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(call->is_completed());
  EXPECT_TRUE(call->is_closed());
  EXPECT_EQ(call->completion_status(), RpcStatus::ProtocolError());
  ASSERT_TRUE(read_task.result().has_value());
  EXPECT_EQ(read_task.result()->status(), RpcStatus::ProtocolError());

  // The violation ends only this call. Unlike a malformed packet, it does not
  // close the connection.
  EXPECT_FALSE(task.is_closed());
  EXPECT_FALSE(raw_conn->is_closed());

  read_task.Deregister();
  task.Deregister();
}

// A server interprets the code in a client's error packet as one sent by a
// client: a code that only a server may send is a protocol violation. Error
// packets are terminal, so the server sends nothing back, and each error ends
// only its own call.
TEST(ConnectionTaskTest, ClientErrorEndsServerCallWithMappedStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(EstablishedConnection{conn}, allocator, server);
  dispatcher.Post(task);

  struct Case {
    ProtocolError error;
    RpcStatus expected;
  };
  constexpr Case kCases[] = {
      {ProtocolError::kCancelled, RpcStatus::Cancelled()},
      {ProtocolError::kMethodTypeMismatch, RpcStatus::Unimplemented()},
      // Only a server may send `kUnknownMethod`.
      {ProtocolError::kUnknownMethod, RpcStatus::ProtocolError()},
  };

  constexpr Method method(1, MethodType::kBidirectionalStreaming, 0, nullptr);
  uint32_t call_id = 60;
  for (const Case& test : kCases) {
    ServerCall* server_call = ServerCall::Allocate(task, call_id, method);
    ASSERT_NE(server_call, nullptr);
    IntrusivePtr<Call> call = server_call->shared_call();

    auto pkt =
        PacketFramer::FrameClientErrorPacket(allocator, call_id, test.error);
    ASSERT_NE(pkt, nullptr);
    raw_conn->SetNextRead(std::move(pkt));
    task.Wake();
    dispatcher.RunUntilStalled();

    EXPECT_TRUE(call->is_completed()) << pw::EnumToString(test.error);
    EXPECT_EQ(call->completion_status(), test.expected)
        << pw::EnumToString(test.error);
    ++call_id;
  }

  EXPECT_EQ(raw_conn->written_packet_count(), 0u);
  EXPECT_FALSE(task.is_closed());
  EXPECT_FALSE(raw_conn->is_closed());

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
  Buf client_syn =
      HandshakePacket(HandshakePacket::Type::kSyn)
          .Encode(Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes));
  ASSERT_NE(client_syn, nullptr);
  raw_conn->SetNextRead(std::move(client_syn));

  task.Wake();
  dispatcher.RunUntilStalled();

  // Server processed SYN, sent SYN-ACK, but is awaiting ACK
  EXPECT_FALSE(task.is_handshake_complete());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Step 2: Inject client handshake ACK packet
  Buf client_ack =
      HandshakePacket(HandshakePacket::Type::kAck)
          .Encode(Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes));
  ASSERT_NE(client_ack, nullptr);
  raw_conn->SetNextRead(std::move(client_ack));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(task.is_handshake_complete());
  EXPECT_EQ(task.handshake_info().negotiated_version, 1u);

  task.Deregister();
}

TEST(ConnectionTaskTest, ControlPacketQueueOutOfMemoryDisconnects) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ClientConnectionTask task(EstablishedConnection{conn}, allocator);
  dispatcher.Post(task);
  auto call = ClientCall::Create(task, 1u);
  ASSERT_NE(call, nullptr);

  // With the transport blocked, the packet has to be queued, but the queue
  // cannot grow. Dropping a terminal packet would strand the peer, so the
  // connection closes instead. The connection is gone, so its calls report
  // kDisconnected rather than kResourceExhausted.
  raw_conn->SetBlockReserveWrite(true);
  allocator.Exhaust();
  task.QueueError(2u, ProtocolError::kCancelled);

  EXPECT_TRUE(task.is_closed());
  EXPECT_EQ(task.close_status(), RpcStatus::Disconnected());
  EXPECT_TRUE(raw_conn->is_closed());
  EXPECT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::Disconnected());

  dispatcher.RunUntilStalled();
  task.Deregister();
}

TEST(ConnectionTaskTest, ServerRoleHandshakeFailureIsProtocolError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  ServerConnectionTask task(conn, allocator, server);
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  // The client must start with a SYN; an ACK is unexpected.
  Buf client_ack =
      HandshakePacket(HandshakePacket::Type::kAck)
          .Encode(Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes));
  ASSERT_NE(client_ack, nullptr);
  raw_conn->SetNextRead(std::move(client_ack));

  task.Wake();
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(task.is_handshake_complete());
  EXPECT_TRUE(task.is_closed());
  EXPECT_TRUE(raw_conn->is_closed());
  EXPECT_EQ(task.close_status(), RpcStatus::ProtocolError());

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
