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

#include "pw_rpc2/client.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_rpc2/internal/generated_service_client.h"
#include "pw_rpc2/internal/method_info.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/service_client.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace echo::pwpb {

struct EchoRequest {
  uint32_t val = 0;
};

struct EchoResponse {
  uint32_t val = 0;
};

class EchoService {
 public:
  static constexpr uint32_t kServiceId = 0x1234;

  struct EchoUnary : ::pw::rpc2::internal::MethodInfoTag {
    static constexpr uint32_t kServiceId = EchoService::kServiceId;
    static constexpr uint32_t kMethodId = 0x5678;
    static constexpr ::pw::rpc2::MethodType kType =
        ::pw::rpc2::MethodType::kUnary;
    using Request = EchoRequest;
    using Response = EchoResponse;
  };

  class Client : public ::pw::rpc2::internal::GeneratedServiceClient {
   public:
    explicit Client(::pw::rpc2::Client client)
        : GeneratedServiceClient(std::move(client), kServiceId) {}

    ::pw::rpc2::UnaryFuture<EchoRequest, EchoResponse> EchoUnary(
        const EchoRequest& request) const {
      return CallUnary<EchoRequest, EchoResponse>(
          EchoService::EchoUnary::kMethodId, request);
    }
  };
};

}  // namespace echo::pwpb

namespace pw::rpc2::internal {

// Serializes a message whose only field is `uint32_t val` as its raw bytes.
template <typename Message>
struct Uint32ValSerializer {
  static size_t MaxEncodedSize(const Message&) { return sizeof(uint32_t); }

  static StatusWithSize Serialize(const Message& msg,
                                  span<std::byte> destination) {
    if (destination.size() < sizeof(uint32_t)) {
      return StatusWithSize::ResourceExhausted();
    }
    std::memcpy(destination.data(), &msg.val, sizeof(uint32_t));
    return StatusWithSize(OkStatus(), sizeof(uint32_t));
  }

  template <typename T>
  static Result<T> Deserialize(span<const std::byte> source) {
    if (source.size() != sizeof(uint32_t)) {
      return Status::DataLoss();
    }
    T msg;
    std::memcpy(&msg.val, source.data(), sizeof(uint32_t));
    return msg;
  }
};

template <>
struct SerializerFor<echo::pwpb::EchoRequest> {
  using type = Uint32ValSerializer<echo::pwpb::EchoRequest>;
};

template <>
struct SerializerFor<echo::pwpb::EchoResponse> {
  using type = Uint32ValSerializer<echo::pwpb::EchoResponse>;
};

}  // namespace pw::rpc2::internal

namespace pw::rpc2 {
namespace {

using EchoRequest = echo::pwpb::EchoRequest;
using EchoResponse = echo::pwpb::EchoResponse;

TEST(ClientFutureTest, CreateFromTransportSequentialConnectAndHandshake) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  test::MockTransport transport;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  async2::FutureTask task(Client::Connect(dispatcher, allocator, transport));
  dispatcher.Post(task);

  // Awaiting the transport connection.
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());

  // Once connected, the client sends its kSyn and awaits the kSynAck.
  transport.ResolveConnect(conn);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // The kSynAck completes the handshake: the client sends its kAck and
  // resolves.
  test::PushSynAck(allocator, *raw_conn);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  PW_TEST_ASSERT_OK(task.value());
  EXPECT_EQ(raw_conn->commit_count(), 2u);

  Client client = std::move(*task.value());
  EXPECT_TRUE(client.is_open());

  // The connected client can create calls.
  EXPECT_NE(internal::CallAccess::CreateCall(client), nullptr);

  test::CloseClient(client, dispatcher);
}

TEST(ClientFutureTest, CreateFromConnectionDirectHandshake) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  async2::FutureTask task(Client::Connect(dispatcher, allocator, conn));
  dispatcher.Post(task);

  // The client sends its kSyn and awaits the kSynAck.
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // The kSynAck completes the handshake: the client sends its kAck and
  // resolves.
  test::PushSynAck(allocator, *raw_conn);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  PW_TEST_ASSERT_OK(task.value());
  EXPECT_EQ(raw_conn->commit_count(), 2u);

  Client client = std::move(*task.value());
  EXPECT_TRUE(client.is_open());

  test::CloseClient(client, dispatcher);
}

TEST(ClientFutureTest, CreateFromTransportConnectFailure) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  test::MockTransport transport;
  async2::FutureTask task(Client::Connect(dispatcher, allocator, transport));
  dispatcher.Post(task);

  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());

  transport.ResolveConnect(Status::Unavailable());
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value().status(), Status::Unavailable());
}

TEST(ClientFutureTest, CreateFromConnectionHandshakeFailure) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  async2::FutureTask task(Client::Connect(dispatcher, allocator, conn));
  dispatcher.Post(task);

  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());

  // Reply with a packet that is not a valid handshake packet.
  Buf resp = Buf::Allocate(allocator, 4);
  ASSERT_FALSE(resp.empty());
  raw_conn->SetNextRead(std::move(resp));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value().status(), Status::DataLoss());
}

TEST(ClientFutureTest, DestroyedUnpendedFromConnectionClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  {
    auto fut = Client::Connect(dispatcher, allocator, conn);
    EXPECT_TRUE(fut.is_pendable());
  }
  EXPECT_TRUE(raw_conn->is_closed());
}

TEST(ClientTest, CallIdAllocationAndCopyMoveSemantics) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  Client client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);

  // Moving empties the source.
  Client moved_client(std::move(client));
  EXPECT_TRUE(moved_client.is_open());
  EXPECT_FALSE(client.is_open());  // NOLINT(bugprone-use-after-move)

  // Copying keeps both clients open and referring to the same connection.
  Client copied_client(moved_client);
  EXPECT_TRUE(copied_client.is_open());
  EXPECT_TRUE(moved_client.is_open());

  // ServiceClients store a Client and expose it via client(), and every copy
  // draws call IDs from the one underlying connection. A ServiceClient is only
  // constructed through a generated service client.
  class TestServiceClient : public internal::GeneratedServiceClient {
   public:
    TestServiceClient(Client client, uint32_t service_id)
        : internal::GeneratedServiceClient(std::move(client), service_id) {}
  };
  TestServiceClient ref1(copied_client, 1);
  TestServiceClient ref2 = ref1;
  EXPECT_TRUE(ref1.is_open());
  EXPECT_TRUE(ref1.client().is_open());
  auto call1 = internal::CallAccess::CreateCall(ref1.client());
  auto call2 = internal::CallAccess::CreateCall(ref2.client());
  auto call3 = internal::CallAccess::CreateCall(copied_client);
  ASSERT_NE(call1, nullptr);
  ASSERT_NE(call2, nullptr);
  ASSERT_NE(call3, nullptr);
  EXPECT_NE(call1->call_id(), call2->call_id());
  EXPECT_NE(call2->call_id(), call3->call_id());

  test::CloseClient(moved_client, dispatcher);
  EXPECT_FALSE(moved_client.is_open());
  EXPECT_FALSE(copied_client.is_open());
  EXPECT_FALSE(ref1.is_open());
  EXPECT_FALSE(ref2.is_open());
}

TEST(MockPeerTest, StartsConnectedWithoutHandshakeOrCalls) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  dispatcher.RunUntilStalled();

  // Client::Connect() would write a kSyn and await a kSynAck (see
  // CreateFromConnectionDirectHandshake). MakeMockPeer skips the handshake, so
  // nothing is written and the first recorded packet is the first real call.
  EXPECT_EQ(peer.connection().commit_count(), 0u);
  EXPECT_EQ(peer.connection().written_packet_count(), 0u);
  EXPECT_EQ(peer.unclaimed_packet_count(), 0u);

  // The client can allocate calls immediately.
  EXPECT_TRUE(peer.client().is_open());
  EXPECT_NE(internal::CallAccess::CreateCall(peer.client()), nullptr);
}

// MockPeer encodes and decodes typed payloads only through the generic
// internal::MaxEncodedSize/Serialize/Deserialize helpers, so it works with any
// SerializerFor backend.

TEST(MockPeerTest, DecodesTypedRequestAndSendsTypedResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  echo::pwpb::EchoService::Client client(peer.client());

  async2::FutureTask task(client.EchoUnary(EchoRequest{.val = 42}));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  // Read side: the recorded request payload decodes back to the message.
  auto call = peer.ExpectInvocation<echo::pwpb::EchoService::EchoUnary>();
  auto decoded = call.request();
  PW_TEST_ASSERT_OK(decoded);
  EXPECT_EQ(decoded->val, 42u);

  // The call is still open until the peer answers it.
  EXPECT_FALSE(task.has_value());

  // Send side: a typed response is serialized and delivered to the client.
  call.Finish(EchoResponse{.val = 99});

  ASSERT_TRUE(task.has_value());
  PW_TEST_ASSERT_OK(task.value());
  EXPECT_EQ(task.value()->val, 99u);
}

TEST(MockPeerTest, ClaimsEachInvocationExactlyOnce) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  echo::pwpb::EchoService::Client client(peer.client());

  async2::FutureTask first(client.EchoUnary(EchoRequest{.val = 1}));
  dispatcher.Post(first);
  peer.ExpectInvocation<echo::pwpb::EchoService::EchoUnary>().Finish(
      EchoResponse{.val = 11});
  ASSERT_TRUE(first.has_value());
  PW_TEST_ASSERT_OK(first.value());
  EXPECT_EQ(first.value()->val, 11u);

  // The first call is claimed, so the second ExpectInvocation() sees only the
  // new one rather than tripping on a stale request.
  async2::FutureTask second(client.EchoUnary(EchoRequest{.val = 2}));
  dispatcher.Post(second);
  peer.ExpectInvocation<echo::pwpb::EchoService::EchoUnary>().Finish(
      EchoResponse{.val = 22});
  ASSERT_TRUE(second.has_value());
  PW_TEST_ASSERT_OK(second.value());
  EXPECT_EQ(second.value()->val, 22u);

  EXPECT_EQ(peer.unclaimed_packet_count(), 0u);
}

TEST(MockPeerTest, TypedDecodeFailsOnGarbagePayload) {
  // Malformed bytes must fail to decode rather than yield a default-constructed
  // message.
  const std::byte garbage[] = {std::byte{0xFF}, std::byte{0xFF}};
  EXPECT_FALSE(test::PayloadAs<EchoRequest>(garbage).ok());
}

// Close() marks the connection closed immediately and resolves once the
// dispatcher tears it down.
TEST(ClientTest, CloseMarksClientClosedAndResolves) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  Client client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);
  ASSERT_TRUE(client.is_open());

  async2::FutureTask close_task(client.Close());
  EXPECT_FALSE(client.is_open());

  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(close_task.has_value());
  PW_TEST_EXPECT_OK(close_task.value());
}

// Closing an empty client is a no-op rather than an error, so moved-from and
// default-constructed clients can be closed unconditionally.
TEST(ClientTest, CloseOnAnEmptyClientResolvesImmediately) {
  async2::DispatcherForTest dispatcher;

  Client client;
  EXPECT_FALSE(client.is_open());

  async2::FutureTask close_task(client.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(close_task.has_value());
  PW_TEST_EXPECT_OK(close_task.value());
}

TEST(ClientTest, CloseOnAnAlreadyClosedClientResolvesImmediately) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  Client client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);

  test::CloseClient(client, dispatcher);
  ASSERT_FALSE(client.is_open());

  async2::FutureTask close_task(client.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(close_task.has_value());
  PW_TEST_EXPECT_OK(close_task.value());
}

// Closing tears down the connection, which aborts every call riding on it.
TEST(ClientTest, CloseCancelsInFlightCalls) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  Client client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);

  echo::pwpb::EchoService::Client stub{client};
  async2::FutureTask call(stub.EchoUnary(EchoRequest{.val = 7}));
  dispatcher.Post(call);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(call.has_value());

  test::CloseClient(client, dispatcher);

  ASSERT_TRUE(call.has_value());
  EXPECT_EQ(call.value().status(), Status::Cancelled());
}

// `Close()` and `CloseBlocking()` are the only client operations that may run
// off the dispatcher thread; the blocking form is for threads that are not
// driving the dispatcher themselves.
TEST(ClientTest, BlockingCloseRunsFromAnotherThread) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  Client client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);

  pw::thread::test::TestThreadContext context;
  pw::Thread thread(context.options(), [&] { client.CloseBlocking(); });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  thread.join();

  EXPECT_FALSE(client.is_open());
}

TEST(ClientTest, DroppingOneOfSeveralHandlesKeepsConnectionOpen) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  std::optional<Client> client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);

  echo::pwpb::EchoService::Client stub{*client};
  async2::FutureTask call(stub.EchoUnary(EchoRequest{.val = 7}));
  dispatcher.Post(call);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(call.has_value());

  // Destroying one Client keeps the connection open for the remaining handles
  // (such as `stub`).
  client.reset();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(stub.is_open());
  EXPECT_FALSE(raw_conn->is_closed());
  EXPECT_FALSE(call.has_value());

  Client handle = stub.client();
  test::CloseClient(handle, dispatcher);
  ASSERT_TRUE(call.has_value());
  EXPECT_EQ(call.value().status(), Status::Cancelled());
}

// Dropping the last handle closes the connection.
TEST(ClientTest, DroppingLastHandleClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  std::optional<Client> client =
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn);
  std::optional<echo::pwpb::EchoService::Client> stub(std::in_place, *client);

  async2::FutureTask call(stub->EchoUnary(EchoRequest{.val = 7}));
  dispatcher.Post(call);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(call.has_value());

  client.reset();
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(raw_conn->is_closed());
  EXPECT_FALSE(call.has_value());

  // `stub` holds the last handle.
  stub.reset();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(raw_conn->is_closed());
  ASSERT_TRUE(call.has_value());
  EXPECT_EQ(call.value().status(), Status::Cancelled());
}

}  // namespace
}  // namespace pw::rpc2
