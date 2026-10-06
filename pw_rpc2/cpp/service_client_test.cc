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

#include "pw_rpc2/service_client.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_bytes/span.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/generated_service_client.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_status/status_with_size.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2 {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

static_assert(!std::is_copy_constructible_v<RawReadFuture>);
static_assert(!std::is_copy_assignable_v<RawReadFuture>);
static_assert(std::is_move_constructible_v<RawReadFuture>);
static_assert(std::is_move_assignable_v<RawReadFuture>);

static_assert(!std::is_copy_constructible_v<ReadFuture<pw::ConstBuf>>);
static_assert(!std::is_copy_assignable_v<ReadFuture<pw::ConstBuf>>);
static_assert(std::is_move_constructible_v<ReadFuture<pw::ConstBuf>>);
static_assert(std::is_move_assignable_v<ReadFuture<pw::ConstBuf>>);

static_assert(
    !std::is_copy_constructible_v<UnaryFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    !std::is_copy_assignable_v<UnaryFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    std::is_move_constructible_v<UnaryFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    std::is_move_assignable_v<UnaryFuture<pw::ConstBuf, pw::ConstBuf>>);

static_assert(!std::is_copy_constructible_v<
              ServerStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    !std::is_copy_assignable_v<ServerStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(std::is_move_constructible_v<
              ServerStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    std::is_move_assignable_v<ServerStreamFuture<pw::ConstBuf, pw::ConstBuf>>);

static_assert(!std::is_copy_constructible_v<
              ClientStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    !std::is_copy_assignable_v<ClientStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(std::is_move_constructible_v<
              ClientStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    std::is_move_assignable_v<ClientStreamFuture<pw::ConstBuf, pw::ConstBuf>>);

static_assert(!std::is_copy_constructible_v<
              BidiStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    !std::is_copy_assignable_v<BidiStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    std::is_move_constructible_v<BidiStreamFuture<pw::ConstBuf, pw::ConstBuf>>);
static_assert(
    std::is_move_assignable_v<BidiStreamFuture<pw::ConstBuf, pw::ConstBuf>>);

static_assert(
    std::is_same_v<decltype(std::declval<RawUnaryReservation&>().Commit(0)),
                   Result<RawResponseFuture>>);
static_assert(std::is_same_v<
              decltype(std::declval<RawServerStreamReservation&>().Commit(0)),
              Result<RawReader>>);
static_assert(
    std::is_same_v<ClientStreamCall<pw::ConstBuf>::Writer, RawWriter>);
static_assert(std::is_same_v<ClientStreamCall<pw::ConstBuf>::ResponseFuture,
                             RawResponseFuture>);
static_assert(std::is_same_v<BidiStreamCall<pw::ConstBuf>::Writer, RawWriter>);
static_assert(
    std::is_same_v<BidiStreamCall<pw::ConstBuf>::Reader, Reader<pw::ConstBuf>>);

static_assert(!std::is_copy_constructible_v<RawResponseFuture>);
static_assert(std::is_move_constructible_v<RawResponseFuture>);
static_assert(std::is_move_assignable_v<RawResponseFuture>);

// The call accessors preserve the value category and constness of the call
// object.
template <typename Call>
constexpr bool kWriterPreservesQualifiers =
    std::is_same_v<decltype(std::declval<Call&>().writer()),
                   typename Call::Writer&> &&
    std::is_same_v<decltype(std::declval<const Call&>().writer()),
                   const typename Call::Writer&> &&
    std::is_same_v<decltype(std::declval<Call&&>().writer()),
                   typename Call::Writer&&> &&
    std::is_same_v<decltype(std::declval<const Call&&>().writer()),
                   const typename Call::Writer&&>;
static_assert(kWriterPreservesQualifiers<ClientStreamCall<pw::ConstBuf>>);
static_assert(kWriterPreservesQualifiers<BidiStreamCall<pw::ConstBuf>>);

template <typename Call>
constexpr bool kReaderPreservesQualifiers =
    std::is_same_v<decltype(std::declval<Call&>().reader()),
                   typename Call::Reader&> &&
    std::is_same_v<decltype(std::declval<const Call&>().reader()),
                   const typename Call::Reader&> &&
    std::is_same_v<decltype(std::declval<Call&&>().reader()),
                   typename Call::Reader&&> &&
    std::is_same_v<decltype(std::declval<const Call&&>().reader()),
                   const typename Call::Reader&&>;
static_assert(kReaderPreservesQualifiers<BidiStreamCall<pw::ConstBuf>>);

template <typename Call>
constexpr bool kResponsePreservesQualifiers =
    std::is_same_v<decltype(std::declval<Call&>().response()),
                   typename Call::ResponseFuture&> &&
    std::is_same_v<decltype(std::declval<const Call&>().response()),
                   const typename Call::ResponseFuture&> &&
    std::is_same_v<decltype(std::declval<Call&&>().response()),
                   typename Call::ResponseFuture&&> &&
    std::is_same_v<decltype(std::declval<const Call&&>().response()),
                   const typename Call::ResponseFuture&&>;
static_assert(kResponsePreservesQualifiers<ClientStreamCall<pw::ConstBuf>>);

static_assert(std::is_default_constructible_v<RawUnaryReserveFuture>);
static_assert(std::is_default_constructible_v<RawServerStreamReserveFuture>);
static_assert(!std::is_copy_constructible_v<RawUnaryReserveFuture>);
static_assert(std::is_move_constructible_v<RawUnaryReserveFuture>);
static_assert(!std::is_default_constructible_v<RawUnaryReservation>);
static_assert(!std::is_default_constructible_v<RawServerStreamReservation>);
static_assert(!std::is_copy_constructible_v<RawUnaryReservation>);
static_assert(std::is_move_constructible_v<RawUnaryReservation>);

static_assert(!std::is_default_constructible_v<ServiceClient>);
static_assert(!std::is_constructible_v<ServiceClient, Client, uint32_t>);

class TestServiceClient : public internal::GeneratedServiceClient {
 public:
  // An empty service client, which refers to no connection.
  TestServiceClient() = default;

  TestServiceClient(Client client, uint32_t service_id)
      : internal::GeneratedServiceClient(std::move(client), service_id) {}

  using internal::GeneratedServiceClient::CallBidiStream;
  using internal::GeneratedServiceClient::CallClientStream;
  using internal::GeneratedServiceClient::CallServerStream;
  using internal::GeneratedServiceClient::CallServerStreamRaw;
  using internal::GeneratedServiceClient::CallUnary;
  using internal::GeneratedServiceClient::CallUnaryRaw;
};

// A request whose serializer always fails with `INVALID_ARGUMENT`.
struct UnserializableRequest {
  struct Serializer {
    static size_t MaxEncodedSize(const UnserializableRequest&) { return 4; }
    static StatusWithSize Serialize(const UnserializableRequest&, ByteSpan) {
      return StatusWithSize::InvalidArgument();
    }
  };
};

// Frames a server packet of any type that is neither a start nor an error
// packet.
Result<pw::Buf> FrameServerPacket(pw::Allocator& allocator,
                                  internal::PacketType type,
                                  uint32_t call_id,
                                  pw::ConstByteSpan payload = {}) {
  PW_CHECK(type.is_server());
  return internal::PacketFramer::FramePacket(allocator, type, call_id, payload);
}

// Returns the call ID of the most recent packet the client sent.
uint32_t LastSentCallId(test::MockConnection& raw_conn) {
  auto decoded = internal::InboundPacket::Decode(raw_conn.last_written_buf());
  PW_CHECK_OK(decoded.status());
  return decoded->call_id();
}

// Checks that the most recent packet the client sent terminates `call_id` with
// `error`.
void ExpectSentError(test::MockConnection& raw_conn,
                     uint32_t call_id,
                     internal::ProtocolStatus error) {
  auto decoded = internal::InboundPacket::Decode(raw_conn.last_written_buf());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->type(),
            internal::PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decoded->call_id(), call_id);
  EXPECT_EQ(decoded->error(), error);
}

// Polls a future that it owns and keeps reachable, so that tests can cancel the
// future while the task waits on it.
template <typename Fut>
class FutureOwningTask : public async2::Task {
 public:
  explicit FutureOwningTask(Fut future) : future_(std::move(future)) {}
  ~FutureOwningTask() override { Deregister(); }

  Fut& future() { return future_; }
  const std::optional<typename Fut::value_type>& result() const {
    return result_;
  }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = future_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    result_.emplace(std::move(*poll));
    return async2::Ready();
  }

  Fut future_;
  std::optional<typename Fut::value_type> result_;
};

template <typename Fut>
FutureOwningTask(Fut future) -> FutureOwningTask<Fut>;

// Server packets that cannot answer a unary or client-streaming call, which
// takes exactly one packet with a payload that completes the RPC.
constexpr internal::PacketType kNotSingleResponse[] = {
    internal::PacketType::Make<flags::kServer, flags::kHasPayload>(),
    internal::PacketType::Make<flags::kServer, flags::kStreamEnd>(),
    internal::PacketType::
        Make<flags::kServer, flags::kHasPayload, flags::kStreamEnd>(),
    internal::PacketType::Make<flags::kServer, flags::kOkTerminal>(),
};

constexpr internal::PacketType kUnaryRequestType = internal::PacketType::
    Make<flags::kStart, flags::kHasPayload, flags::kStreamEnd>();

TEST(ServiceClientTest, UnaryCallSendsRequestFirstThenReadsResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);

  pw::Buf req_buf = pw::Buf::Allocate(allocator, 3);
  req_buf[0] = std::byte(10);
  req_buf[1] = std::byte(20);
  req_buf[2] = std::byte(30);

  TestServiceClient test_service(client, 100u);
  async2::FutureTask task(test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(
      200u, pw::ConstBuf(std::move(req_buf))));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 3u);  // 2 handshake + 1 request
  auto decode_req =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_req.ok());
  EXPECT_EQ(decode_req->type(), kUnaryRequestType);
  EXPECT_EQ(decode_req->service_id(), 100u);
  EXPECT_EQ(decode_req->method_id(), 200u);
  EXPECT_EQ(decode_req->payload().size(), 3u);

  EXPECT_FALSE(task.has_value());

  pw::Buf resp_payload = pw::Buf::Allocate(allocator, 4);
  std::memset(resp_payload.data(), 0xAA, 4);
  auto framed_resp = internal::PacketFramer::FrameResponsePacket(
      allocator, /*call_id=*/1, pw::ConstByteSpan(resp_payload));
  ASSERT_TRUE(framed_resp.ok());
  raw_conn->SetNextRead(std::move(*framed_resp));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  PW_TEST_ASSERT_OK(task.value());
  EXPECT_EQ(task.value()->size(), 4u);
}

// A default-constructed future is empty: it must report itself as neither
// pendable nor complete, so that polling it is detectably a programming error.
TEST(ServiceClientTest, DefaultConstructedUnaryFutureIsNotPendable) {
  UnaryFuture<pw::ConstBuf, pw::ConstBuf> fut;
  EXPECT_FALSE(fut.is_pendable());
  EXPECT_FALSE(fut.is_complete());
}

// A unary call future is pendable until it resolves, and then reports itself
// complete and no longer pendable.
TEST(ServiceClientTest, UnaryFutureReportsCompleteOnceResolved) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);

  pw::Buf req_buf = pw::Buf::Allocate(allocator, 1);
  req_buf[0] = std::byte(7);

  TestServiceClient test_service(client, 100u);
  async2::FutureTask task(test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(
      200u, pw::ConstBuf(std::move(req_buf))));

  EXPECT_TRUE(task.is_pendable());
  EXPECT_FALSE(task.is_complete());

  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  pw::Buf resp_payload = pw::Buf::Allocate(allocator, 2);
  std::memset(resp_payload.data(), 0xBB, 2);
  auto framed_resp = internal::PacketFramer::FrameResponsePacket(
      allocator, /*call_id=*/1, pw::ConstByteSpan(resp_payload));
  ASSERT_TRUE(framed_resp.ok());
  raw_conn->SetNextRead(std::move(*framed_resp));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  PW_TEST_ASSERT_OK(task.value());

  EXPECT_TRUE(task.is_complete());
  EXPECT_FALSE(task.is_pendable());
}

TEST(ServiceClientTest, ServerStreamingReceivesMessagesAndStreamEnd) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(
          300u, pw::ConstBuf()));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  Reader<pw::ConstBuf> reader = std::move(*call_task.value());

  async2::FutureTask read1(reader.Read());
  dispatcher.Post(read1);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(read1.has_value());

  std::byte chunk1[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt1 =
      internal::PacketFramer::FrameServerMessagePacket(allocator, 1u, chunk1);
  ASSERT_TRUE(pkt1.ok());
  raw_conn->SetNextRead(std::move(*pkt1));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read1.has_value());
  PW_TEST_ASSERT_OK(read1.value());
  EXPECT_EQ(read1.value()->size(), 3u);

  async2::FutureTask read2(reader.Read());
  dispatcher.Post(read2);
  std::byte chunk2[2] = {std::byte{10}, std::byte{20}};
  auto pkt2 =
      internal::PacketFramer::FrameServerMessagePacket(allocator, 1u, chunk2);
  ASSERT_TRUE(pkt2.ok());
  raw_conn->SetNextRead(std::move(*pkt2));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read2.has_value());
  PW_TEST_ASSERT_OK(read2.value());
  EXPECT_EQ(read2.value()->size(), 2u);

  async2::FutureTask read3(reader.Read());
  dispatcher.Post(read3);
  auto end_pkt = internal::PacketFramer::FrameServerFinishPacket(allocator, 1u);
  ASSERT_TRUE(end_pkt.ok());
  raw_conn->SetNextRead(std::move(*end_pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read3.has_value());
  EXPECT_EQ(read3.value().status(), Status::OutOfRange());
}

TEST(ServiceClientTest, ClientStreamingSendsChunksThenReadsResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  ClientStreamCall<> call = std::move(*call_task.value());

  async2::FutureTask write1(call.writer().ReserveWrite(2));
  dispatcher.Post(write1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write1.has_value());
  PW_TEST_ASSERT_OK(write1.value());
  write1.value()->data()[0] = std::byte{1};
  write1.value()->data()[1] = std::byte{2};
  PW_TEST_EXPECT_OK(write1.value()->Commit(2));

  async2::FutureTask write2(call.writer().ReserveWrite(2));
  dispatcher.Post(write2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write2.has_value());
  PW_TEST_ASSERT_OK(write2.value());
  write2.value()->data()[0] = std::byte{3};
  write2.value()->data()[1] = std::byte{4};
  PW_TEST_EXPECT_OK(write2.value()->Commit(2));

  // Finish the client stream.
  async2::FutureTask finish_task(call.writer().Finish());
  dispatcher.Post(finish_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(finish_task.has_value());
  PW_TEST_ASSERT_OK(finish_task.value());

  async2::FutureTask resp_task(std::move(call.response()));
  dispatcher.Post(resp_task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(resp_task.has_value());

  pw::Buf resp_data = pw::Buf::Allocate(allocator, 4);
  std::memset(resp_data.data(), 0xEE, 4);
  auto framed_resp = internal::PacketFramer::FrameResponsePacket(
      allocator, /*call_id=*/1, pw::ConstByteSpan(resp_data));
  ASSERT_TRUE(framed_resp.ok());
  raw_conn->SetNextRead(std::move(*framed_resp));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(resp_task.has_value());
  PW_TEST_ASSERT_OK(resp_task.value());
  EXPECT_EQ(resp_task.value()->size(), 4u);
}

TEST(ServiceClientTest, BidirectionalStreamingConcurrentReadWrite) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  BidiStreamCall<> call = std::move(*call_task.value());

  async2::FutureTask write_task(call.writer().ReserveWrite(2));
  dispatcher.Post(write_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write_task.has_value());
  PW_TEST_ASSERT_OK(write_task.value());
  write_task.value()->data()[0] = std::byte{10};
  write_task.value()->data()[1] = std::byte{20};
  PW_TEST_EXPECT_OK(write_task.value()->Commit(2));

  async2::FutureTask read_task(call.reader().Read());
  dispatcher.Post(read_task);
  std::byte srv_msg[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt =
      internal::PacketFramer::FrameServerMessagePacket(allocator, 1u, srv_msg);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task.has_value());
  PW_TEST_ASSERT_OK(read_task.value());
  EXPECT_EQ(read_task.value()->size(), 3u);

  async2::FutureTask finish_task(call.writer().Finish());
  dispatcher.Post(finish_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(finish_task.has_value());
  PW_TEST_ASSERT_OK(finish_task.value());

  async2::FutureTask end_read_task(call.reader().Read());
  dispatcher.Post(end_read_task);
  auto end_pkt = internal::PacketFramer::FrameServerFinishPacket(allocator, 1u);
  ASSERT_TRUE(end_pkt.ok());
  raw_conn->SetNextRead(std::move(*end_pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(end_read_task.has_value());
  EXPECT_EQ(end_read_task.value().status(), Status::OutOfRange());
}

// A server finish packet terminates the RPC, so an open client writer is closed
// too.
TEST(ServiceClientTest, BidirectionalServerStreamEndClosesClientWriter) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  BidiStreamCall<> call = std::move(*call_task.value());

  async2::FutureTask end_read_task(call.reader().Read());
  dispatcher.Post(end_read_task);
  auto end_pkt = internal::PacketFramer::FrameServerFinishPacket(allocator, 1u);
  ASSERT_TRUE(end_pkt.ok());
  raw_conn->SetNextRead(std::move(*end_pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(end_read_task.has_value());
  EXPECT_EQ(end_read_task.value().status(), Status::OutOfRange());

  const size_t commits_before = raw_conn->commit_count();

  async2::FutureTask write_task(call.writer().ReserveWrite(2));
  dispatcher.Post(write_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write_task.has_value());
  EXPECT_FALSE(write_task.value().ok());

  // The client does not end a stream for a call that is already over.
  {
    auto dropped = std::move(call.writer());
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

// A server message that also ends the server's stream without terminating the
// RPC (0x0B) delivers the message, but the reader stays open until the terminal
// packet. The C++ server does not send this packet, but it is valid on the
// wire.
TEST(ServiceClientTest, ServerFinalMessageWaitsForTerminalPacket) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(
          300u, pw::ConstBuf()));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  Reader<pw::ConstBuf> reader = std::move(*call_task.value());

  std::byte chunk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt = FrameServerPacket(allocator,
                               internal::PacketType::Make<flags::kServer,
                                                          flags::kHasPayload,
                                                          flags::kStreamEnd>(),
                               1u,
                               chunk);
  ASSERT_TRUE(pkt.ok());

  async2::FutureTask read_task(reader.Read());
  dispatcher.Post(read_task);
  raw_conn->SetNextRead(std::move(*pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task.has_value());
  PW_TEST_ASSERT_OK(read_task.value());
  EXPECT_EQ(read_task.value()->size(), sizeof(chunk));

  async2::FutureTask end_read_task(reader.Read());
  dispatcher.Post(end_read_task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(end_read_task.has_value());

  auto finish = internal::PacketFramer::FrameServerFinishPacket(allocator, 1u);
  ASSERT_TRUE(finish.ok());
  raw_conn->SetNextRead(std::move(*finish));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(end_read_task.has_value());
  EXPECT_EQ(end_read_task.value().status(), Status::OutOfRange());
}

// After a server half-close (0x09), the client drops any further messages and
// reports the status from the terminal packet that follows.
TEST(ServiceClientTest, ServerStreamEndThenErrorReportsError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  BidiStreamCall<> call = std::move(*call_task.value());
  const uint32_t call_id = LastSentCallId(*raw_conn);

  async2::FutureTask read_task(call.reader().Read());
  dispatcher.Post(read_task);

  auto stream_end = FrameServerPacket(
      allocator,
      internal::PacketType::Make<flags::kServer, flags::kStreamEnd>(),
      call_id);
  ASSERT_TRUE(stream_end.ok());
  raw_conn->SetNextRead(std::move(*stream_end));
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(read_task.has_value());

  std::byte chunk[1] = {std::byte{1}};
  auto late_message = internal::PacketFramer::FrameServerMessagePacket(
      allocator, call_id, chunk);
  ASSERT_TRUE(late_message.ok());
  raw_conn->SetNextRead(std::move(*late_message));
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(read_task.has_value());

  // The client's stream is still open until the RPC ends.
  async2::FutureTask write_task(call.writer().ReserveWrite(1));
  dispatcher.Post(write_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write_task.has_value());
  PW_TEST_EXPECT_OK(write_task.value().status());

  auto error = internal::PacketFramer::FrameServerErrorPacket(
      allocator, call_id, internal::ProtocolStatus::kInternal);
  ASSERT_TRUE(error.ok());
  raw_conn->SetNextRead(std::move(*error));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task.has_value());
  EXPECT_EQ(read_task.value().status(), Status::Internal());
}

// A unary call takes exactly one packet that carries the response and ends the
// RPC. Any other server packet means the server thinks the method streams its
// responses, so the client fails the call rather than taking a streamed message
// as the response. If the packet did not already end the RPC, the client tells
// the server why.
TEST(ServiceClientTest, UnaryCallRejectsResponseThatIsNotSingleResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  constexpr std::byte kPayload[2] = {std::byte{1}, std::byte{2}};
  for (internal::PacketType type : kNotSingleResponse) {
    async2::FutureTask task(test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(
        200u, pw::ConstBuf()));
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    const uint32_t call_id = LastSentCallId(*raw_conn);

    auto pkt = FrameServerPacket(
        allocator,
        type,
        call_id,
        type.has_payload() ? pw::ConstByteSpan(kPayload) : pw::ConstByteSpan());
    ASSERT_TRUE(pkt.ok());
    raw_conn->SetNextRead(std::move(*pkt));
    dispatcher.RunUntilStalled();

    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task.value().status(), Status::FailedPrecondition());
    if (type ==
        internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()) {
      auto last = internal::InboundPacket::Decode(raw_conn->last_written_buf());
      ASSERT_TRUE(last.ok());
      EXPECT_EQ(last->type(), kUnaryRequestType);
      EXPECT_EQ(last->call_id(), call_id);
    } else {
      ExpectSentError(
          *raw_conn, call_id, internal::ProtocolStatus::kMethodTypeMismatch);
    }
  }
}

TEST(ServiceClientTest, ClientStreamingCallRejectsStreamedResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  ClientStreamCall<> call = std::move(*call_task.value());
  const uint32_t call_id = LastSentCallId(*raw_conn);

  async2::FutureTask resp_task(std::move(call.response()));
  dispatcher.Post(resp_task);

  std::byte srv_msg[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto pkt = internal::PacketFramer::FrameServerMessagePacket(
      allocator, call_id, srv_msg);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(resp_task.has_value());
  EXPECT_EQ(resp_task.value().status(), Status::FailedPrecondition());
  ExpectSentError(
      *raw_conn, call_id, internal::ProtocolStatus::kMethodTypeMismatch);

  // The call is over, so the client's stream is closed too.
  async2::FutureTask write_task(call.writer().ReserveWrite(2));
  dispatcher.Post(write_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write_task.has_value());
  EXPECT_EQ(write_task.value().status(), Status::FailedPrecondition());
}

// A streaming call accepts a single terminal response: it is unambiguously a
// stream of one message, which a streaming server may send.
TEST(ServiceClientTest, ServerStreamingCallAcceptsSingleResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(
          300u, pw::ConstBuf()));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  Reader<pw::ConstBuf> reader = std::move(*call_task.value());
  const uint32_t call_id = LastSentCallId(*raw_conn);

  async2::FutureTask read_task(reader.Read());
  dispatcher.Post(read_task);
  std::byte resp[2] = {std::byte{7}, std::byte{8}};
  auto pkt =
      internal::PacketFramer::FrameResponsePacket(allocator, call_id, resp);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read_task.has_value());
  PW_TEST_ASSERT_OK(read_task.value());
  EXPECT_EQ(read_task.value()->size(), sizeof(resp));

  async2::FutureTask end_read_task(reader.Read());
  dispatcher.Post(end_read_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(end_read_task.has_value());
  EXPECT_EQ(end_read_task.value().status(), Status::OutOfRange());

  // Nothing was reported to the server.
  EXPECT_EQ(LastSentCallId(*raw_conn), call_id);
  auto last = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(last.ok());
  EXPECT_EQ(last->type(), kUnaryRequestType);
}

TEST(ServiceClientTest, ServerErrorResolvesClientFutureWithStatus) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  pw::Buf req_buf = pw::Buf::Allocate(allocator, 2);
  async2::FutureTask task(test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(
      600u, pw::ConstBuf(std::move(req_buf))));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  // The server reports ProtocolStatus::kUnknownMethod.
  auto err_pkt = internal::PacketFramer::FrameServerErrorPacket(
      allocator,
      /*call_id=*/1,
      internal::ProtocolStatus::kUnknownMethod);
  ASSERT_TRUE(err_pkt.ok());
  raw_conn->SetNextRead(std::move(*err_pkt));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value().status(), Status::NotFound());
}

TEST(ServiceClientTest, ClientDropHandleEndsClientStream) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  size_t initial_commits = raw_conn->commit_count();

  // Keep the response future alive: dropping it would cancel the call.
  std::optional<RawResponseFuture> response;
  uint32_t call_id = 0;
  {
    async2::FutureTask call_task(
        test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(700u));
    dispatcher.Post(call_task);
    dispatcher.RunUntilStalled();

    ASSERT_TRUE(call_task.has_value());
    PW_TEST_ASSERT_OK(call_task.value());
    ClientStreamCall<pw::ConstBuf, pw::ConstBuf> stream =
        std::move(*call_task.value());
    response.emplace(std::move(stream).response());
    call_id = LastSentCallId(*raw_conn);

    EXPECT_EQ(raw_conn->commit_count(), initial_commits + 1u);  // +1 request
    // stream.writer() is destroyed at end of scope without Finish().
  }

  // Destruction of an unclosed Writer ends the outbound stream normally.
  EXPECT_EQ(raw_conn->commit_count(), initial_commits + 2u);
  auto decode_end =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_end.ok());
  EXPECT_EQ(decode_end->type(),
            internal::PacketType::Make<flags::kStreamEnd>());

  // Dropping the response future before the server answers cancels the call.
  response.reset();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), initial_commits + 3u);
  ExpectSentError(*raw_conn, call_id, internal::ProtocolStatus::kCancelled);
}

// Cancelling through the writer completes the call locally, so a pending read
// resolves instead of waiting for a server reply that will never come.
TEST(ServiceClientTest, BidiWriterCancelResolvesPendingRead) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  BidiStreamCall<> call = std::move(*call_task.value());

  async2::FutureTask read_task(call.reader().Read());
  dispatcher.Post(read_task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(read_task.has_value());

  const size_t commits_before = raw_conn->commit_count();
  call.writer().Cancel();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(read_task.has_value());
  EXPECT_EQ(read_task.value().status(), Status::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  ExpectSentError(*raw_conn, 1u, internal::ProtocolStatus::kCancelled);

  // The call is over, so dropping the handles sends nothing more.
  {
    auto dropped = std::move(call);
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
}

TEST(ServiceClientTest, ClientStreamWriterCancelResolvesResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  ClientStreamCall<> call = std::move(*call_task.value());

  async2::FutureTask resp_task(std::move(call.response()));
  dispatcher.Post(resp_task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(resp_task.has_value());

  call.writer().Cancel();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(resp_task.has_value());
  EXPECT_EQ(resp_task.value().status(), Status::Cancelled());
  ExpectSentError(*raw_conn, 1u, internal::ProtocolStatus::kCancelled);
}

// A client that drops a server stream's reader before the stream ends has no
// further interest in the call, so the server is told to stop.
TEST(ServiceClientTest, DroppingServerStreamReaderCancelsCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(
          300u, pw::ConstBuf()));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  std::optional<Reader<pw::ConstBuf>> reader(std::move(*call_task.value()));

  const size_t commits_before = raw_conn->commit_count();
  reader.reset();
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  ExpectSentError(*raw_conn, 1u, internal::ProtocolStatus::kCancelled);
}

// Dropping a reader after the stream ended normally sends nothing.
TEST(ServiceClientTest, DroppingFinishedServerStreamReaderSendsNothing) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(
          300u, pw::ConstBuf()));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  std::optional<Reader<pw::ConstBuf>> reader(std::move(*call_task.value()));

  async2::FutureTask end_read(reader->Read());
  dispatcher.Post(end_read);
  auto end_pkt = internal::PacketFramer::FrameServerFinishPacket(allocator, 1u);
  ASSERT_TRUE(end_pkt.ok());
  raw_conn->SetNextRead(std::move(*end_pkt));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(end_read.has_value());
  EXPECT_EQ(end_read.value().status(), Status::OutOfRange());

  const size_t commits_before = raw_conn->commit_count();
  reader.reset();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, ReaderCancelResolvesPendingReadAndCancelsCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(
          300u, pw::ConstBuf()));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  std::optional<Reader<pw::ConstBuf>> reader(std::move(*call_task.value()));

  async2::FutureTask read_task(reader->Read());
  dispatcher.Post(read_task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(read_task.has_value());

  const size_t commits_before = raw_conn->commit_count();
  reader->Cancel();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(read_task.has_value());
  EXPECT_EQ(read_task.value().status(), Status::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  ExpectSentError(*raw_conn, 1u, internal::ProtocolStatus::kCancelled);

  // A second cancel, or dropping the reader, sends nothing more.
  reader->Cancel();
  reader.reset();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
}

// Dropping a unary call after its request was sent cancels the call.
TEST(ServiceClientTest, DroppingUnaryCallAfterRequestCancelsCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  size_t commits_after_request = 0;
  {
    async2::FutureTask task(test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(
        200u, pw::ConstBuf()));
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    EXPECT_FALSE(task.has_value());
    commits_after_request = raw_conn->commit_count();
  }
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), commits_after_request + 1u);
  ExpectSentError(*raw_conn, 1u, internal::ProtocolStatus::kCancelled);
}

// Nothing is sent until a call future is polled, so dropping one unpolled
// leaves no trace on the wire.
TEST(ServiceClientTest, DroppingUnpolledCallFuturesWritesNothing) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  {
    auto unary = test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {});
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);

  {
    auto server_stream =
        test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(300u, {});
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);

  {
    auto client_stream =
        test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u);
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);

  {
    auto bidi = test_service.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u);
  }
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, CallsOnClosedClientFailWithUnavailable) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  test::CloseClient(client, dispatcher);
  ASSERT_FALSE(test_service.is_open());
  const size_t commits_before = raw_conn->commit_count();

  async2::FutureTask unary(
      test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {}));
  async2::FutureTask server_stream(
      test_service.CallServerStream<pw::ConstBuf, pw::ConstBuf>(300u, {}));
  async2::FutureTask client_stream(
      test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u));
  async2::FutureTask bidi(
      test_service.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u));
  dispatcher.Post(unary);
  dispatcher.Post(server_stream);
  dispatcher.Post(client_stream);
  dispatcher.Post(bidi);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(unary.has_value());
  EXPECT_EQ(unary.value().status(), Status::Unavailable());
  ASSERT_TRUE(server_stream.has_value());
  EXPECT_EQ(server_stream.value().status(), Status::Unavailable());
  ASSERT_TRUE(client_stream.has_value());
  EXPECT_EQ(client_stream.value().status(), Status::Unavailable());
  ASSERT_TRUE(bidi.has_value());
  EXPECT_EQ(bidi.value().status(), Status::Unavailable());

  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, CallsOnEmptyServiceClientFailWithUnavailable) {
  async2::DispatcherForTest dispatcher;
  TestServiceClient empty;
  ASSERT_FALSE(empty.is_open());

  async2::FutureTask unary(
      empty.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {}));
  async2::FutureTask server_stream(
      empty.CallServerStream<pw::ConstBuf, pw::ConstBuf>(300u, {}));
  async2::FutureTask client_stream(
      empty.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u));
  async2::FutureTask bidi(
      empty.CallBidiStream<pw::ConstBuf, pw::ConstBuf>(500u));
  async2::FutureTask unary_raw(empty.CallUnaryRaw(200u, 4));
  async2::FutureTask server_stream_raw(empty.CallServerStreamRaw(300u, 4));
  dispatcher.Post(unary);
  dispatcher.Post(server_stream);
  dispatcher.Post(client_stream);
  dispatcher.Post(bidi);
  dispatcher.Post(unary_raw);
  dispatcher.Post(server_stream_raw);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(unary.has_value());
  EXPECT_EQ(unary.value().status(), Status::Unavailable());
  ASSERT_TRUE(server_stream.has_value());
  EXPECT_EQ(server_stream.value().status(), Status::Unavailable());
  ASSERT_TRUE(client_stream.has_value());
  EXPECT_EQ(client_stream.value().status(), Status::Unavailable());
  ASSERT_TRUE(bidi.has_value());
  EXPECT_EQ(bidi.value().status(), Status::Unavailable());
  ASSERT_TRUE(unary_raw.has_value());
  EXPECT_EQ(unary_raw.value().status(), Status::Unavailable());
  ASSERT_TRUE(server_stream_raw.has_value());
  EXPECT_EQ(server_stream_raw.value().status(), Status::Unavailable());
}

TEST(ServiceClientTest, UnaryRequestTooLargeForTransportFailsWithoutWriting) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  pw::Buf request = pw::Buf::Allocate(
      allocator, raw_conn->max_write_message_size_bytes() + 1);
  ASSERT_FALSE(request.empty());
  async2::FutureTask task(test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(
      200u, pw::ConstBuf(std::move(request))));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value().status(), Status::ResourceExhausted());
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, RawRequestSizeThatOverflowsFailsWithoutWriting) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  // Adding the packet header to this size would wrap around.
  async2::FutureTask unary(
      test_service.CallUnaryRaw(200u, std::numeric_limits<size_t>::max()));
  async2::FutureTask server_stream(test_service.CallServerStreamRaw(
      300u, std::numeric_limits<size_t>::max()));
  dispatcher.Post(unary);
  dispatcher.Post(server_stream);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(unary.has_value());
  EXPECT_EQ(unary.value().status(), Status::ResourceExhausted());
  ASSERT_TRUE(server_stream.has_value());
  EXPECT_EQ(server_stream.value().status(), Status::ResourceExhausted());
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, RequestSerializationFailureFailsCallWithoutWriting) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  async2::FutureTask unary(
      test_service.CallUnary<UnserializableRequest, pw::ConstBuf>(200u, {}));
  async2::FutureTask server_stream(
      test_service.CallServerStream<UnserializableRequest, pw::ConstBuf>(300u,
                                                                         {}));
  dispatcher.Post(unary);
  dispatcher.Post(server_stream);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(unary.has_value());
  EXPECT_EQ(unary.value().status(), Status::InvalidArgument());
  ASSERT_TRUE(server_stream.has_value());
  EXPECT_EQ(server_stream.value().status(), Status::InvalidArgument());
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, RawUnaryCallWritesRequestIntoReservation) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask reservation(test_service.CallUnaryRaw(200u, 3));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());
  ASSERT_GE(reservation.value()->size(), 3u);
  reservation.value()->data()[0] = std::byte{1};
  reservation.value()->data()[1] = std::byte{2};
  reservation.value()->data()[2] = std::byte{3};
  Result<RawResponseFuture> response_future = reservation.value()->Commit(3);
  PW_TEST_ASSERT_OK(response_future);

  auto request = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request->type(), kUnaryRequestType);
  EXPECT_EQ(request->service_id(), 100u);
  EXPECT_EQ(request->method_id(), 200u);
  EXPECT_EQ(request->payload().size(), 3u);
  const uint32_t call_id = request->call_id();

  async2::FutureTask response(std::move(*response_future));
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(response.has_value());

  std::byte resp[4] = {};
  auto framed_resp =
      internal::PacketFramer::FrameResponsePacket(allocator, call_id, resp);
  ASSERT_TRUE(framed_resp.ok());
  raw_conn->SetNextRead(std::move(*framed_resp));
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  EXPECT_EQ(response.value()->size(), sizeof(resp));
}

TEST(ServiceClientTest, RawServerStreamCallWritesRequestIntoReservation) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask reservation(test_service.CallServerStreamRaw(300u, 2));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());
  ASSERT_GE(reservation.value()->size(), 2u);
  reservation.value()->data()[0] = std::byte{4};
  reservation.value()->data()[1] = std::byte{5};
  Result<RawReader> reader = reservation.value()->Commit(2);
  PW_TEST_ASSERT_OK(reader);

  auto request = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request->type(), kUnaryRequestType);
  EXPECT_EQ(request->service_id(), 100u);
  EXPECT_EQ(request->method_id(), 300u);
  EXPECT_EQ(request->payload().size(), 2u);
  const uint32_t call_id = request->call_id();

  async2::FutureTask read(reader->Read());
  dispatcher.Post(read);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(read.has_value());

  std::byte message[3] = {};
  auto framed_message = internal::PacketFramer::FrameServerMessagePacket(
      allocator, call_id, message);
  ASSERT_TRUE(framed_message.ok());
  raw_conn->SetNextRead(std::move(*framed_message));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(read.has_value());
  PW_TEST_ASSERT_OK(read.value());
  EXPECT_EQ(read.value()->size(), sizeof(message));

  async2::FutureTask end_read(reader->Read());
  dispatcher.Post(end_read);
  auto finish =
      internal::PacketFramer::FrameServerFinishPacket(allocator, call_id);
  ASSERT_TRUE(finish.ok());
  raw_conn->SetNextRead(std::move(*finish));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(end_read.has_value());
  EXPECT_EQ(end_read.value().status(), Status::OutOfRange());
}

TEST(ServiceClientTest,
     DroppingRawRequestReservationWithoutCommitSendsNothing) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  async2::FutureTask reservation(test_service.CallUnaryRaw(200u, 3));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());
  reservation.value()->Drop();
  EXPECT_EQ(reservation.value()->Commit(0).status(),
            Status::FailedPrecondition());
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest,
     DestroyingRawServerStreamReservationWithoutCommitSendsNothing) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  {
    async2::FutureTask reservation(test_service.CallServerStreamRaw(300u, 2));
    dispatcher.Post(reservation);
    dispatcher.RunUntilStalled();
    ASSERT_TRUE(reservation.has_value());
    PW_TEST_ASSERT_OK(reservation.value());
  }  // The reservation is destroyed without being committed.
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

// The response future or reader returned by `Commit()` is the only handle to
// the call, so dropping it before the response arrives cancels the call.
TEST(ServiceClientTest, DroppingHandleFromCommitCancelsCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  auto expect_cancel_on_reader_drop = [&, &mock_conn = *raw_conn](
                                          auto reservation_future) {
    async2::FutureTask reservation(std::move(reservation_future));
    dispatcher.Post(reservation);
    dispatcher.RunUntilStalled();
    ASSERT_TRUE(reservation.has_value());
    PW_TEST_ASSERT_OK(reservation.value());
    uint32_t call_id = 0;
    size_t commits_before = 0;
    {
      auto reader = reservation.value()->Commit(0);
      PW_TEST_ASSERT_OK(reader);
      call_id = LastSentCallId(mock_conn);
      commits_before = mock_conn.commit_count();
    }  // Drop the reader before the server responds.

    dispatcher.RunUntilStalled();
    EXPECT_EQ(mock_conn.commit_count(), commits_before + 1u);
    ExpectSentError(mock_conn, call_id, internal::ProtocolStatus::kCancelled);
  };

  expect_cancel_on_reader_drop(test_service.CallUnaryRaw(200u, 1));
  expect_cancel_on_reader_drop(test_service.CallServerStreamRaw(300u, 1));
}

TEST(ServiceClientTest, CancelUnaryCallBeforeSendingSendsNothing) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  FutureOwningTask task(
      test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {}));
  task.future().Cancel();
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), Status::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, CancelUnaryCallAfterSendingCancelsOnServer) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  FutureOwningTask task(
      test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {}));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(task.result().has_value());
  const uint32_t call_id = LastSentCallId(*raw_conn);
  const size_t commits_before = raw_conn->commit_count();

  task.future().Cancel();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), Status::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  ExpectSentError(*raw_conn, call_id, internal::ProtocolStatus::kCancelled);

  // Cancelling a finished call does nothing.
  task.future().Cancel();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
}

TEST(ServiceClientTest, CancelUnaryCallAfterResponseDoesNothing) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  FutureOwningTask task(
      test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {}));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(task.result().has_value());

  std::byte resp[2] = {};
  auto framed_resp = internal::PacketFramer::FrameResponsePacket(
      allocator, LastSentCallId(*raw_conn), resp);
  ASSERT_TRUE(framed_resp.ok());
  raw_conn->SetNextRead(std::move(*framed_resp));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.result().has_value());
  PW_TEST_ASSERT_OK(task.result().value());
  const size_t commits_before = raw_conn->commit_count();

  task.future().Cancel();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, CancelRawResponseFutureCancelsOnServer) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask reservation(test_service.CallUnaryRaw(200u, 1));
  dispatcher.Post(reservation);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(reservation.has_value());
  PW_TEST_ASSERT_OK(reservation.value());
  Result<RawResponseFuture> response_future = reservation.value()->Commit(0);
  PW_TEST_ASSERT_OK(response_future);
  const uint32_t call_id = LastSentCallId(*raw_conn);

  FutureOwningTask task(std::move(*response_future));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(task.result().has_value());

  task.future().Cancel();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), Status::Cancelled());
  ExpectSentError(*raw_conn, call_id, internal::ProtocolStatus::kCancelled);
}

TEST(ServiceClientTest, CancelClientStreamResponseCancelsOnServer) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask call_task(
      test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(400u));
  dispatcher.Post(call_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call_task.has_value());
  PW_TEST_ASSERT_OK(call_task.value());
  const uint32_t call_id = LastSentCallId(*raw_conn);

  FutureOwningTask task(std::move(*call_task.value()).response());
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  ASSERT_FALSE(task.result().has_value());

  task.future().Cancel();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.result().has_value());
  EXPECT_EQ(task.result()->status(), Status::Cancelled());
  ExpectSentError(*raw_conn, call_id, internal::ProtocolStatus::kCancelled);
}

// The transport resolves a pending reservation to nothing only when its socket
// closes, so a call still waiting to send its start packet fails with
// `UNAVAILABLE`.
TEST(ServiceClientTest,
     SocketClosingWhileStartPacketPendingFailsWithUnavailable) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);
  const size_t commits_before = raw_conn->commit_count();

  raw_conn->SetBlockReserveWrite(true);
  async2::FutureTask task(
      test_service.CallUnary<pw::ConstBuf, pw::ConstBuf>(200u, {}));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());

  auto closed = raw_conn->Close();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value().status(), Status::Unavailable());
  EXPECT_EQ(raw_conn->commit_count(), commits_before);
}

TEST(ServiceClientTest, ClientStreamSendsStartStreamFirst) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  test::ScopedClient client(
      test::ConnectMockClient(dispatcher, allocator, conn, *raw_conn),
      dispatcher);
  TestServiceClient test_service(client, 100u);

  async2::FutureTask task(
      test_service.CallClientStream<pw::ConstBuf, pw::ConstBuf>(200u));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  // Polling the RequestFuture sends a START packet with no payload.
  EXPECT_EQ(raw_conn->commit_count(), 3u);  // 2 handshake + 1 start
  auto decode_req =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_req.ok());
  EXPECT_EQ(decode_req->type(), internal::PacketType::Make<flags::kStart>());
  EXPECT_FALSE(decode_req->type().has_payload());
  EXPECT_EQ(decode_req->type().close_mode(), internal::CloseMode::kOpen);
  EXPECT_EQ(decode_req->service_id(), 100u);
  EXPECT_EQ(decode_req->method_id(), 200u);
  EXPECT_EQ(decode_req->payload().size(), 0u);

  ASSERT_TRUE(task.has_value());
  PW_TEST_ASSERT_OK(task.value());
  ClientStreamCall<> call = std::move(*task.value());

  // Subsequent writes send message packets, not START packets.
  async2::FutureTask write_task(call.writer().ReserveWrite(2));
  dispatcher.Post(write_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(write_task.has_value());
  PW_TEST_ASSERT_OK(write_task.value());
  write_task.value()->data()[0] = std::byte(1);
  write_task.value()->data()[1] = std::byte(2);
  PW_TEST_EXPECT_OK(write_task.value()->Commit(2));

  auto decode_msg =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_msg.ok());
  EXPECT_EQ(decode_msg->type(),
            internal::PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decode_msg->payload().size(), 2u);
}

}  // namespace
}  // namespace pw::rpc2
