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
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_bytes/endian.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/client_connection_task.h"
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

TEST(RawReaderWriterTest, RequireValidCallAndExposeCallId) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ClientConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 123u);
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
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 77u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // 1. Reserve 10 bytes, write 3 bytes, and commit 3 bytes.
  async2::FutureTask task1(writer.ReserveWrite(10));
  dispatcher.Post(task1);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task1.value().ok());
  WriteReservation& res1 = *task1.value();
  EXPECT_GE(res1.size(), 10u);
  EXPECT_EQ(static_cast<size_t>(res1.end() - res1.begin()), res1.size());
  EXPECT_EQ(static_cast<size_t>(res1.cend() - res1.cbegin()), res1.size());
  res1[0] = std::byte(1);
  res1[1] = std::byte(2);
  res1[2] = std::byte(3);
  PW_TEST_EXPECT_OK(res1.Commit(3));
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode1 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode1, nullptr);
  EXPECT_EQ(decode1.type(), internal::PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decode1.call_id(), 77u);
  ASSERT_EQ(decode1.payload().size(), 3u);
  EXPECT_EQ(decode1.payload()[0], std::byte(1));
  EXPECT_EQ(decode1.payload()[1], std::byte(2));
  EXPECT_EQ(decode1.payload()[2], std::byte(3));

  // 2. Write second message from borrowed bytes
  const std::byte msg2[2] = {std::byte(10), std::byte(20)};
  async2::FutureTask task2(writer.WriteCopy(ConstBuf::Unowned(msg2)));
  dispatcher.Post(task2);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task2.has_value() && task2.value().ok());
  EXPECT_EQ(raw_conn->commit_count(), 2u);

  auto decode2 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode2, nullptr);
  EXPECT_EQ(decode2.type(), internal::PacketType::Make<flags::kHasPayload>());
  EXPECT_EQ(decode2.call_id(), 77u);
  EXPECT_EQ(decode2.payload().size(), 2u);
  EXPECT_EQ(decode2.payload()[0], std::byte(10));
  EXPECT_EQ(decode2.payload()[1], std::byte(20));

  // 3. Finish stream
  async2::FutureTask task3(writer.Finish());
  dispatcher.Post(task3);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task3.has_value() && task3.value().ok());
  EXPECT_TRUE(writer.is_closed());
  EXPECT_EQ(raw_conn->commit_count(), 3u);

  auto decode3 = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode3, nullptr);
  EXPECT_EQ(decode3.type(), internal::PacketType::Make<flags::kStreamEnd>());

  task1.Deregister();
  task2.Deregister();
  task3.Deregister();
}

// Raw writers are the typed writers with `ConstBuf` messages.
static_assert(std::is_same_v<RawWriter, Writer<ConstBuf>>);
static_assert(std::is_same_v<RawUnaryWriter, UnaryWriter<ConstBuf>>);

// Every writer copies bytes with `WriteCopy()` or `FinishCopy()`. Raw writers
// have no `Write()` or `Finish()` that takes a message.
template <typename W, typename Arg, typename = void>
constexpr bool kHasWrite = false;
template <typename W, typename Arg>
constexpr bool kHasWrite<
    W,
    Arg,
    std::void_t<decltype(std::declval<W&>().Write(std::declval<Arg>()))>> =
    true;

template <typename W, typename Arg, typename = void>
constexpr bool kHasFinish = false;
template <typename W, typename Arg>
constexpr bool kHasFinish<
    W,
    Arg,
    std::void_t<decltype(std::declval<W&>().Finish(std::declval<Arg>()))>> =
    true;

static_assert(!kHasWrite<RawWriter, ConstBuf>);
static_assert(!kHasWrite<RawWriter, const ConstBuf&>);
static_assert(!kHasFinish<RawUnaryWriter, ConstBuf>);
static_assert(!kHasFinish<RawUnaryWriter, const ConstBuf&>);

// `ConstBuf` is move-only, so an owned message must be moved in.
template <typename W, typename = void>
constexpr bool kWriteCopiesLvalue = false;
template <typename W>
constexpr bool
    kWriteCopiesLvalue<W,
                       std::void_t<decltype(std::declval<W&>().WriteCopy(
                           std::declval<ConstBuf&>()))>> = true;

template <typename W, typename = void>
constexpr bool kFinishCopiesLvalue = false;
template <typename W>
constexpr bool
    kFinishCopiesLvalue<W,
                        std::void_t<decltype(std::declval<W&>().FinishCopy(
                            std::declval<ConstBuf&>()))>> = true;

static_assert(!kWriteCopiesLvalue<RawWriter>);
static_assert(!kFinishCopiesLvalue<RawUnaryWriter>);
static_assert(
    std::is_same_v<decltype(std::declval<RawWriter&>().WriteCopy(ConstBuf())),
                   WriteFuture<ConstBuf>>);
static_assert(std::is_same_v<
              decltype(std::declval<RawUnaryWriter&>().FinishCopy(ConstBuf())),
              WriteFuture<ConstBuf>>);

// Typed writers serialize messages with `Write()` or `Finish()` and also copy
// encoded messages from a `ConstBuf`.
struct TypedMessage {
  int value;
};

static_assert(kHasWrite<Writer<TypedMessage>, const TypedMessage&>);
static_assert(kHasWrite<Writer<TypedMessage>, TypedMessage>);
static_assert(kHasFinish<UnaryWriter<TypedMessage>, const TypedMessage&>);
static_assert(kHasFinish<UnaryWriter<TypedMessage>, TypedMessage>);
static_assert(!kWriteCopiesLvalue<Writer<TypedMessage>>);
static_assert(!kFinishCopiesLvalue<UnaryWriter<TypedMessage>>);
static_assert(std::is_same_v<decltype(std::declval<Writer<TypedMessage>&>()
                                          .WriteCopy(ConstBuf())),
                             WriteFuture<ConstBuf>>);
static_assert(std::is_same_v<decltype(std::declval<UnaryWriter<TypedMessage>&>()
                                          .FinishCopy(ConstBuf())),
                             WriteFuture<ConstBuf>>);

// Messages whose serializer reports `kHasCallbacks = true` can only be moved
// into `Write()` or `Finish()`, never copied from an lvalue.
struct CallbackTypedMessage {
  struct Serializer {
    static constexpr bool kHasCallbacks = true;
  };
  int value;
};

static_assert(internal::kHasCallbacks<CallbackTypedMessage>);
static_assert(!internal::kIsCopyableMessage<CallbackTypedMessage>);
static_assert(
    !kHasWrite<Writer<CallbackTypedMessage>, const CallbackTypedMessage&>);
static_assert(!kHasWrite<Writer<CallbackTypedMessage>, CallbackTypedMessage&>);
static_assert(kHasWrite<Writer<CallbackTypedMessage>, CallbackTypedMessage>);
static_assert(!kHasFinish<UnaryWriter<CallbackTypedMessage>,
                          const CallbackTypedMessage&>);
static_assert(
    !kHasFinish<UnaryWriter<CallbackTypedMessage>, CallbackTypedMessage&>);
static_assert(
    kHasFinish<UnaryWriter<CallbackTypedMessage>, CallbackTypedMessage>);

TEST(RawWriterTest, WriteCopyReleasesOwnedMessageWhenSent) {
  allocator::test::AllocatorForTest<4096> allocator;
  allocator::test::AllocatorForTest<256> message_allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 78u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  constexpr std::byte kMessage[] = {std::byte(4), std::byte(5), std::byte(6)};
  Buf message = Buf::AllocateCopy(message_allocator, kMessage);
  ASSERT_NE(message, nullptr);
  ASSERT_GT(message_allocator.metrics().allocated_bytes.value(), 0u);

  // `task` keeps the `WriteFuture` alive after it completes.
  async2::FutureTask task(writer.WriteCopy(std::move(message)));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value() && task.value().ok());
  EXPECT_EQ(message_allocator.metrics().allocated_bytes.value(), 0u);

  auto decode = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode, nullptr);
  EXPECT_EQ(decode.call_id(), 78u);
  ASSERT_EQ(decode.payload().size(), sizeof(kMessage));
  EXPECT_EQ(std::memcmp(decode.payload().data(), kMessage, sizeof(kMessage)),
            0);

  task.Deregister();
}

TEST(RawWriterTest, WriteCopyEmptyBracesSendsEmptyMessage) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 79u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  async2::FutureTask task(writer.WriteCopy({}));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value() && task.value().ok());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto decode = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode, nullptr);
  EXPECT_EQ(decode.call_id(), 79u);
  EXPECT_EQ(decode.payload().size(), 0u);

  task.Deregister();
}

TEST(RawWriterTest, CancelledReservationReleasesWithoutCommit) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 88u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  async2::FutureTask task(writer.ReserveWrite(10));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(task.value().ok());
  auto res = std::move(task.value().value());

  // Explicitly drop reservation without committing
  res.Drop();
  EXPECT_EQ(res.Commit(0), RpcStatus::WriteClosed());
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  task.Deregister();
}

TEST(RawWriterTest, OversizeReservationFailsWithoutClosingWriter) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  const size_t max_size = conn.max_write_message_size_bytes();
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 89u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // The packet header pushes this over the transport's limit.
  async2::FutureTask oversize(writer.ReserveWrite(max_size));
  dispatcher.Post(oversize);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(oversize.value().status(), RpcStatus::MessageTooLarge());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_FALSE(writer.is_closed());

  // A size that would overflow `sizeof(PacketHeader) + max_payload_size` must
  // also fail with kMessageTooLarge rather than wrapping around.
  async2::FutureTask overflow(
      writer.ReserveWrite(std::numeric_limits<size_t>::max()));
  dispatcher.Post(overflow);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(overflow.value().status(), RpcStatus::MessageTooLarge());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_FALSE(writer.is_closed());

  // The writer can still send messages that fit.
  async2::FutureTask fits(writer.ReserveWrite(3));
  dispatcher.Post(fits);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(fits.value().ok());
  PW_TEST_EXPECT_OK(fits.value()->Commit(3));
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  oversize.Deregister();
  overflow.Deregister();
  fits.Deregister();
}

TEST(RawWriterTest, WriterErrorTransmitsErrorPacket) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 99u);
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
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(),
            internal::PacketType::Make<flags::kErrorTerminal>());
  EXPECT_EQ(decode_res.call_id(), 99u);
  EXPECT_EQ(decode_res.error(), internal::ProtocolError::kCancelled);
}

TEST(RawWriterTest, CancelBeforeStartSendsNothing) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 99u);
  ASSERT_NE(call, nullptr);
  internal::Call& raw_call = *call;

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  // The start packet was never sent, so the server does not know the call.
  writer.Cancel();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(writer.is_closed());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_TRUE(raw_call.is_completed());
  EXPECT_EQ(raw_call.completion_status(), RpcStatus::Cancelled());
}

// Destroying a `Writer` ends the outbound stream normally. This is the
// asymmetric counterpart to `UnaryWriter`, which cancels instead.
TEST(RawWriterTest, DestructorEndsStreamNormally) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 55u);
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
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(decode_res.type(), internal::PacketType::Make<flags::kStreamEnd>());
  EXPECT_EQ(decode_res.call_id(), 55u);
}

// A unary call owes the client-facing side exactly one response, so an
// unfinished `UnaryWriter` cancels rather than ending the stream.
TEST(RawWriterTest, UnaryWriterDestructorCancels) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;
  internal::ServerTask server(allocator, dispatcher);

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = allocator.MakeShared<internal::ServerConnectionTask>(
      internal::EstablishedConnection{std::move(conn)}, allocator, server);
  auto call = test::StartServerCall(*connection_task, 56u);

  {
    auto responder = internal::CallAccess::Create<RawUnaryWriter>(call);
    EXPECT_FALSE(responder.is_closed());
  }

  EXPECT_TRUE(call->is_write_closed());
  ASSERT_EQ(raw_conn->commit_count(), 1u);

  auto decode_res =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decode_res, nullptr);
  EXPECT_EQ(
      decode_res.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res.call_id(), 56u);
  EXPECT_EQ(decode_res.error(),
            internal::ProtocolError::kDroppedWithoutResponse);
}

// Once the stream has been finished explicitly, the destructor must not send a
// second stream end.
TEST(RawWriterTest, DestructorAfterFinishDoesNotResend) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 57u);
  ASSERT_NE(call, nullptr);

  {
    auto writer = internal::CallAccess::Create<RawWriter>(call);
    async2::FutureTask task(writer.Finish());
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    ASSERT_TRUE(task.has_value() && task.value().ok());
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
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 58u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  async2::FutureTask message(writer.ReserveWrite(2));
  dispatcher.Post(message);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(message.value().ok());

  async2::FutureTask finish(writer.Finish());
  dispatcher.Post(finish);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(finish.has_value() && finish.value().ok());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  EXPECT_EQ(message.value()->Commit(2), RpcStatus::WriteClosed());
  EXPECT_EQ(raw_conn->commit_count(), 1u);
  auto decoded = internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded.type(), internal::PacketType::Make<flags::kStreamEnd>());

  message.Deregister();
  finish.Deregister();
}

// A message reserved before the call was cancelled reports the cancellation
// when committed, and is not sent.
TEST(RawWriterTest, MessageCommittedAfterCancelReportsCancellation) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 59u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));

  async2::FutureTask message(writer.ReserveWrite(2));
  dispatcher.Post(message);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(message.value().ok());

  writer.Cancel();
  EXPECT_EQ(message.value()->Commit(2), RpcStatus::Cancelled());
  dispatcher.RunUntilStalled();
  // The call was never started, so nothing at all reaches the peer.
  EXPECT_EQ(raw_conn->commit_count(), 0u);

  message.Deregister();
}

TEST(RawReaderTest, ReadsPayloadBufFromCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 99u);
  ASSERT_NE(call, nullptr);
  auto reader = internal::CallAccess::Create<RawReader>(call);

  async2::FutureTask task(reader.Read());
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());

  Buf msg = Buf::Allocate(allocator, 4);
  std::memset(msg.data(), 0x55, 4);

  EXPECT_TRUE(call->ReserveMessageSlot());
  call->OnMessage(ConstBuf(std::move(msg)));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value() && task.value().ok());
  EXPECT_EQ(task.value().value().size(), 4u);

  task.Deregister();
}

// Reads a single message, expecting the read to resolve within this call.
template <typename Fut>
void ReadOneMessage(async2::DispatcherForTest& dispatcher,
                    Allocator& allocator,
                    internal::Call& call,
                    Fut read_fut,
                    size_t expected_size) {
  async2::FutureTask task(std::move(read_fut));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  Buf msg = Buf::Allocate(allocator, expected_size);
  std::memset(msg.data(), 0x11, expected_size);
  ASSERT_TRUE(call.ReserveMessageSlot());
  call.OnMessage(ConstBuf(std::move(msg)));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value() && task.value().ok());
  EXPECT_EQ(task.value().value().size(), expected_size);
  task.Deregister();
}

TEST(RawReaderTest, SequentialReadsReuseTheCall) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 101u);
  ASSERT_NE(call, nullptr);
  auto reader = internal::CallAccess::Create<RawReader>(call);

  // Each read completes before the next one starts, so only one read is ever
  // outstanding on the call.
  ReadOneMessage(dispatcher, allocator, *call, reader.Read(), 4u);
  ReadOneMessage(dispatcher, allocator, *call, reader.Read(), 2u);

  // A completed future that is still alive must not block the next read.
  RawReadFuture read_fut = reader.Read();
  async2::FutureTask task(std::move(read_fut));
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();

  Buf msg = Buf::Allocate(allocator, 3);
  std::memset(msg.data(), 0x22, 3);
  ASSERT_TRUE(call->ReserveMessageSlot());
  call->OnMessage(ConstBuf(std::move(msg)));
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value() && task.value().ok());

  // `task` still owns the completed future here.
  RawReadFuture next_fut = reader.Read();
  EXPECT_TRUE(next_fut.is_pendable());

  task.Deregister();
}

TEST(RawResponseFutureTest, ResolvesToResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 102u);
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
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 104u);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  {
    async2::FutureTask task(internal::CallAccess::Create<RawResponseFuture>(
        IntrusivePtr<internal::Call>(call)));
    dispatcher.Post(task);
    dispatcher.RunUntilStalled();
    EXPECT_FALSE(task.has_value());
    task.Deregister();
  }
  // Nothing is left to observe the response, so the call is cancelled.
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(call->is_completed());
  EXPECT_EQ(call->completion_status(), RpcStatus::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), 1u);
}

TEST(RawResponseFutureTest, CancelResolvesPendingFutureToCancelled) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);

  auto call = internal::ClientCall::Create(*connection_task, 103u);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  auto future =
      internal::CallAccess::Create<RawResponseFuture>(std::move(call));
  async2::FutureTask task(future);
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.has_value());

  future.Cancel();
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task.value().status(), RpcStatus::Cancelled());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Cancelling again, after the call has ended, does nothing.
  future.Cancel();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);
}

TEST(RawResponseFutureTest, CancelWithoutCallIsNoOp) {
  RawResponseFuture response;
  EXPECT_FALSE(response.is_pendable());
  response.Cancel();
  EXPECT_FALSE(response.is_pendable());
}

TEST(RawWriterTest, PollingOnWrongDispatcherCrashes) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;
  async2::DispatcherForTest wrong_dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 105u);
  ASSERT_NE(call, nullptr);

  auto writer = internal::CallAccess::Create<RawWriter>(std::move(call));
  auto fut = writer.Finish();
  EXPECT_DEATH_IF_SUPPORTED(
      wrong_dispatcher.RunInTaskUntilStalled(fut).IgnorePoll(), "");
}

TEST(RawReaderTest, PollingOnWrongDispatcherCrashes) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;
  async2::DispatcherForTest wrong_dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 106u);
  ASSERT_NE(call, nullptr);

  auto reader = internal::CallAccess::Create<RawReader>(std::move(call));
  auto fut = reader.Read();
  EXPECT_DEATH_IF_SUPPORTED(
      wrong_dispatcher.RunInTaskUntilStalled(fut).IgnorePoll(), "");
}

TEST(RawResponseFutureTest, PollingOnWrongDispatcherCrashes) {
  allocator::test::AllocatorForTest<4096> allocator;
  async2::DispatcherForTest dispatcher;
  async2::DispatcherForTest wrong_dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  auto connection_task = dispatcher.Post<internal::ClientConnectionTask>(
      allocator, internal::EstablishedConnection{std::move(conn)}, allocator);
  auto call = internal::ClientCall::Create(*connection_task, 107u);
  ASSERT_NE(call, nullptr);
  call->MarkStarted();

  auto fut = internal::CallAccess::Create<RawResponseFuture>(std::move(call));
  EXPECT_DEATH_IF_SUPPORTED(
      wrong_dispatcher.RunInTaskUntilStalled(fut).IgnorePoll(), "");
}

}  // namespace
}  // namespace pw::rpc2
