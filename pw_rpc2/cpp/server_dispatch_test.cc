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
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/server.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

// Arms one call's ingress reservation, so that the connection task can deliver
// a message to it without backpressure.
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

TEST(ServerDispatchTest, DispatchesIncomingRequest) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  async2::DispatcherForTest dispatcher;
  ServerTask server(allocator, dispatcher);
  auto task = dispatcher.Post<ServerConnectionTask>(
      allocator, EstablishedConnection{conn}, allocator, server);
  ASSERT_NE(task, nullptr);

  std::byte payload[4] = {
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  auto pkt = PacketFramer::FrameStartUnaryPacket(
      allocator, /*call_id=*/10, /*service_id=*/20, /*method_id=*/30, payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  dispatcher.RunUntilStalled();

  // Service 20 is not registered, so the request is rejected. Receiving the
  // error at all proves the packet was decoded and routed to the server.
  ASSERT_EQ(raw_conn->written_packet_count(), 1u);
  auto response =
      InboundPacket::Decode(ConstBuf::Unowned(raw_conn->written_packet(0)));
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(response->call_id(), 10u);
  EXPECT_EQ(response->error(), ProtocolStatus::kUnknownService);

  // This connection was built by hand rather than accepted, so the server does
  // not know to close it. Unpost it here: the dispatcher outlives the server,
  // and would otherwise be the one destroying it, after the server it refers
  // to is gone.
  task->Deregister();
}

TEST(ServerDispatchTest, CallRegistriesAreScopedToTheirConnectionTask) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn1, raw_conn1] = test::MakeMockConnection(allocator);
  auto [conn2, raw_conn2] = test::MakeMockConnection(allocator);

  auto task1 = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn1}, allocator);
  auto task2 = dispatcher.Post<ClientConnectionTask>(
      allocator, EstablishedConnection{conn2}, allocator);
  ASSERT_NE(task1, nullptr);
  ASSERT_NE(task2, nullptr);

  // The same call ID on two different connections.
  auto call1 = internal::ClientCall::Create(*task1, 42u, allocator);
  ASSERT_NE(call1, nullptr);
  auto call2 = internal::ClientCall::Create(*task2, 42u, allocator);
  ASSERT_NE(call2, nullptr);

  // Deliver a message for call 42 on the first connection only.
  std::byte data[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt = PacketFramer::FrameServerMessagePacket(allocator, 42u, data);
  ASSERT_TRUE(pkt.ok());
  raw_conn1->SetNextRead(std::move(*pkt));
  task1->Wake();
  dispatcher.RunUntilStalled();

  // Only the call on that connection received it: its one-message queue is now
  // full, while the identically numbered call on the other connection is
  // untouched.
  ReserveSlotTask reserve1(*call1);
  ReserveSlotTask reserve2(*call2);
  dispatcher.Post(reserve1);
  dispatcher.Post(reserve2);
  dispatcher.RunUntilStalled();
  reserve1.Deregister();
  reserve2.Deregister();
  EXPECT_FALSE(reserve1.reserved());
  EXPECT_TRUE(reserve2.reserved());
}

TEST(ServerDispatchTest, ConnectionSeveredAbortsActiveCallsImmediately) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  {
    auto [conn, raw_conn] = test::MakeMockConnection(allocator);
    auto task = dispatcher.Post<ClientConnectionTask>(
        allocator, EstablishedConnection{conn}, allocator);
    ASSERT_NE(task, nullptr);

    auto call1 = internal::ClientCall::Create(*task, 1u, allocator);
    ASSERT_NE(call1, nullptr);

    auto call2 = internal::ClientCall::Create(*task, 2u, allocator);
    ASSERT_NE(call2, nullptr);

    task->CloseConnection(Status::Aborted());
    EXPECT_TRUE(call1->is_completed());
    EXPECT_EQ(call1->completion_status(), Status::Aborted());
    EXPECT_TRUE(call2->is_completed());
    EXPECT_EQ(call2->completion_status(), Status::Aborted());
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);
}

TEST(ServerDispatchTest, PacketDecodeRejectsTruncatedPacket) {
  allocator::test::AllocatorForTest<16384> allocator;

  // A buffer too short to hold even the common header.
  {
    pw::Buf buf =
        pw::Buf::Allocate(allocator, sizeof(internal::PacketHeader) - 1);
    ASSERT_FALSE(buf.empty());
    std::memset(buf.data(), 0, buf.size());
    auto decode_res =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
    EXPECT_EQ(decode_res.status(), Status::DataLoss());
  }

  // A buffer with a complete common header, but too short for the request
  // fields that the packet type implies.
  {
    pw::Buf buf =
        pw::Buf::Allocate(allocator, sizeof(internal::RequestWireFormat) - 1);
    ASSERT_FALSE(buf.empty());
    std::memset(buf.data(), 0, buf.size());
    buf[offsetof(internal::PacketHeader, type)] = static_cast<std::byte>(
        flags::kStart | flags::kHasPayload | flags::kStreamEnd);
    auto decode_res =
        internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
    EXPECT_EQ(decode_res.status(), Status::DataLoss());
  }
}

TEST(ServerDispatchTest, PacketDecodeRejectsUnrecognizedType) {
  allocator::test::AllocatorForTest<16384> allocator;
  pw::Buf buf = pw::Buf::Allocate(allocator, sizeof(internal::PacketHeader));
  ASSERT_FALSE(buf.empty());
  std::memset(buf.data(), 0, buf.size());
  buf[offsetof(internal::PacketHeader, type)] = std::byte{0xFF};

  auto decode_res =
      internal::InboundPacket::Decode(pw::ConstBuf(std::move(buf)));
  EXPECT_EQ(decode_res.status(), Status::InvalidArgument());
}

TEST(ServerDispatchTest, HandshakePacketDecodeVersionZeroRejection) {
  allocator::test::AllocatorForTest<16384> allocator;
  pw::Buf buf =
      pw::Buf::Allocate(allocator, internal::HandshakePacket::kWireSizeBytes);
  ASSERT_FALSE(buf.empty());
  internal::HandshakePacket packet(internal::HandshakePacket::Type::kSyn);
  EXPECT_TRUE(packet.Encode(ByteSpan(buf)).ok());
  buf[4] = static_cast<std::byte>(0);  // Version 0

  auto decode_res = internal::HandshakePacket::Decode(buf);
  EXPECT_FALSE(decode_res.ok());
  EXPECT_EQ(decode_res.status(), Status::DataLoss());
}

}  // namespace
}  // namespace pw::rpc2::internal
