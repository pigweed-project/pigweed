// Copyright 2021 The Pigweed Authors
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

#include "pw_rpc/internal/call.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "pw_rpc/dynamic_call.h"
#include "pw_rpc/internal/client_call.h"
#include "pw_rpc/internal/test_utils.h"
#include "pw_rpc/service.h"
#include "pw_rpc_private/fake_server_reader_writer.h"
#include "pw_rpc_private/test_method.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc {

class TestService : public Service {
 public:
  constexpr TestService(uint32_t id) : Service(id, method) {}

  static constexpr internal::TestMethodUnion method = internal::TestMethod(8);
};

namespace internal {
namespace {

constexpr uint32_t kChannelId = 99;
constexpr uint32_t kServiceId = 16;
constexpr uint32_t kMethodId = 8;
constexpr uint32_t kCallId = 327;
constexpr Packet kPacket(
    pwpb::PacketType::REQUEST, kChannelId, kServiceId, kMethodId, kCallId);

using ::pw::rpc::internal::test::FakeServerReader;
using ::pw::rpc::internal::test::FakeServerReaderWriter;
using ::pw::rpc::internal::test::FakeServerWriter;
using ::std::byte;
using ::testing::Test;

constexpr size_t kCallSizeWithoutDeferredDeleter =
    // IntrusiveForwardList::Item pointer
    sizeof(IntrusiveForwardList<Call>::Item) +
    // Endpoint pointer
    sizeof(Endpoint*) +
    // call_id, channel_id, service_id, method_id
    4 * sizeof(uint32_t) +
    // Packed state and properties
    sizeof(void*) +
    // on_error and on_next callbacks
    2 * sizeof(Function<void(Status)>);

#if PW_RPC_DYNAMIC_ALLOCATION
constexpr size_t AlignUp(size_t size, size_t alignment) {
  return (size + alignment - 1) / alignment * alignment;
}

// The deferred deleter for calls released by DynamicCallPtr is the last member.
// It is aligned for PW_RPC_MAKE_UNIQUE_PTR's pointer, so a pointer larger than
// std::unique_ptr may add padding before it.
static_assert(sizeof(Call) == AlignUp(kCallSizeWithoutDeferredDeleter,
                                      alignof(DeferredDeleter)) +
                                  sizeof(DeferredDeleter),
              "Unexpected padding in Call!");
#else
static_assert(sizeof(Call) == kCallSizeWithoutDeferredDeleter,
              "Unexpected padding in Call!");
#endif  // PW_RPC_DYNAMIC_ALLOCATION

static_assert(sizeof(CallProperties) == sizeof(uint8_t));

TEST(CallProperties, ValuesMatch) {
  constexpr CallProperties props_1(
      MethodType::kBidirectionalStreaming, kClientCall, kRawProto);
  static_assert(props_1.method_type() == MethodType::kBidirectionalStreaming);
  static_assert(props_1.call_type() == kClientCall);
  static_assert(props_1.callback_proto_type() == kRawProto);

  constexpr CallProperties props_2(
      MethodType::kClientStreaming, kServerCall, kProtoStruct);
  static_assert(props_2.method_type() == MethodType::kClientStreaming);
  static_assert(props_2.call_type() == kServerCall);
  static_assert(props_2.callback_proto_type() == kProtoStruct);

  constexpr CallProperties props_3(
      MethodType::kUnary, kClientCall, kProtoStruct);
  static_assert(props_3.method_type() == MethodType::kUnary);
  static_assert(props_3.call_type() == kClientCall);
  static_assert(props_3.callback_proto_type() == kProtoStruct);
}

class ServerWriterTest : public Test {
 public:
  ServerWriterTest() : context_(TestService::method.method()) {
    rpc_lock().lock();
    FakeServerWriter writer_temp(context_.get().ClaimLocked());
    rpc_lock().unlock();
    writer_ = std::move(writer_temp);
  }

  ServerContextForTest<TestService, kChannelId, kServiceId, kCallId> context_;
  FakeServerWriter writer_;
};

TEST_F(ServerWriterTest, ConstructWithContext_StartsOpen) {
  EXPECT_TRUE(writer_.active());
}

TEST_F(ServerWriterTest, Move_ClosesOriginal) {
  FakeServerWriter moved(std::move(writer_));

#ifndef __clang_analyzer__
  EXPECT_FALSE(writer_.active());
#endif  // ignore use-after-move
  EXPECT_TRUE(moved.active());
}

TEST_F(ServerWriterTest, DefaultConstruct_Closed) {
  FakeServerWriter writer;
  EXPECT_FALSE(writer.active());
}

TEST_F(ServerWriterTest, Construct_RegistersWithServer) {
  RpcLockGuard lock;
  IntrusiveForwardList<Call>::iterator call =
      context_.server().FindCall(kPacket);
  ASSERT_NE(call, context_.server().calls_end());
  EXPECT_EQ(static_cast<void*>(&*call), static_cast<void*>(&writer_));
}

TEST_F(ServerWriterTest, Destruct_RemovesFromServer) {
  {
    // Note `lock_guard` cannot be used here, because while the constructor
    // of `FakeServerWriter` requires the lock be held, the destructor acquires
    // it!
    rpc_lock().lock();
    FakeServerWriter writer(context_.get().ClaimLocked());
    rpc_lock().unlock();
  }

  RpcLockGuard lock;
  EXPECT_EQ(context_.server().FindCall(kPacket), context_.server().calls_end());
}

TEST_F(ServerWriterTest, Finish_RemovesFromServer) {
  EXPECT_EQ(OkStatus(), writer_.Finish());
  RpcLockGuard lock;
  EXPECT_EQ(context_.server().FindCall(kPacket), context_.server().calls_end());
}

TEST_F(ServerWriterTest, Finish_SendsResponse) {
  EXPECT_EQ(OkStatus(), writer_.Finish());

  ASSERT_EQ(context_.output().total_packets(), 1u);
  const Packet& packet = context_.output().last_packet();
  EXPECT_EQ(packet.type(), pwpb::PacketType::RESPONSE);
  EXPECT_EQ(packet.channel_id(), context_.channel_id());
  EXPECT_EQ(packet.service_id(), context_.service_id());
  EXPECT_EQ(packet.method_id(), context_.get().method().id());
  EXPECT_TRUE(packet.payload().empty());
  EXPECT_EQ(packet.status(), OkStatus());
}

TEST_F(ServerWriterTest, Finish_ReturnsStatusFromChannelSend) {
  context_.output().set_send_status(Status::Unauthenticated());

  // All non-OK statuses are remapped to UNKNOWN.
  EXPECT_EQ(Status::Unknown(), writer_.Finish());
}

TEST_F(ServerWriterTest, Finish) {
  ASSERT_TRUE(writer_.active());
  EXPECT_EQ(OkStatus(), writer_.Finish());
  EXPECT_FALSE(writer_.active());
  EXPECT_EQ(Status::FailedPrecondition(), writer_.Finish());
}

TEST_F(ServerWriterTest, Open_SendsPacketWithPayload) {
  constexpr byte data[] = {byte{0xf0}, byte{0x0d}};
  ASSERT_EQ(OkStatus(), writer_.Write(data));

  byte encoded[64];
  auto result = context_.server_stream(data).Encode(encoded);
  ASSERT_EQ(OkStatus(), result.status());

  ConstByteSpan payload = context_.output().last_packet().payload();
  EXPECT_EQ(sizeof(data), payload.size());
  EXPECT_EQ(0, std::memcmp(data, payload.data(), sizeof(data)));
}

TEST_F(ServerWriterTest, Open_WriteCallback_SendsPacketWithPayload) {
  constexpr byte data[] = {byte{0xaa}, byte{0xbb}, byte{0xcc}, byte{0xdd}};

  ASSERT_EQ(OkStatus(), writer_.Write([&data](ByteSpan buffer) {
    std::memcpy(buffer.data(), data, sizeof(data));
    return StatusWithSize(sizeof(data));
  }));

  byte encoded[64];
  auto result = context_.server_stream(data).Encode(encoded);
  ASSERT_EQ(OkStatus(), result.status());

  ConstByteSpan payload = context_.output().last_packet().payload();
  EXPECT_EQ(sizeof(data), payload.size());
  EXPECT_EQ(0, std::memcmp(data, payload.data(), sizeof(data)));
}

TEST_F(ServerWriterTest, Open_WriteCallback_ErrorPropagates) {
  ASSERT_EQ(Status::DataLoss(),
            writer_.Write([](ByteSpan) { return StatusWithSize::DataLoss(); }));
}

TEST_F(ServerWriterTest, Open_WriteCallback_NullptrReturnsInvalidArgument) {
  ASSERT_EQ(Status::InvalidArgument(), writer_.Write(nullptr));
}

TEST_F(ServerWriterTest, Closed_IgnoresFinish) {
  EXPECT_EQ(OkStatus(), writer_.Finish());
  EXPECT_EQ(Status::FailedPrecondition(), writer_.Finish());
}

TEST_F(ServerWriterTest, DefaultConstructor_NoClientStream) {
  FakeServerWriter writer;
  RpcLockGuard lock;
  EXPECT_FALSE(writer.as_server_call().has_client_stream());
  EXPECT_FALSE(writer.as_server_call().client_requested_completion());
}

TEST_F(ServerWriterTest, Open_NoClientStream) {
  RpcLockGuard lock;
  EXPECT_FALSE(writer_.as_server_call().has_client_stream());
  EXPECT_TRUE(writer_.as_server_call().has_server_stream());
  EXPECT_FALSE(writer_.as_server_call().client_requested_completion());
}

class ServerReaderTest : public Test {
 public:
  ServerReaderTest() : context_(TestService::method.method()) {
    rpc_lock().lock();
    FakeServerReader reader_temp(context_.get().ClaimLocked());
    rpc_lock().unlock();
    reader_ = std::move(reader_temp);
  }

  ServerContextForTest<TestService> context_;
  FakeServerReader reader_;
};

TEST_F(ServerReaderTest, DefaultConstructor_StreamClosed) {
  FakeServerReader reader;
  EXPECT_FALSE(reader.as_server_call().active());
  RpcLockGuard lock;
  EXPECT_FALSE(reader.as_server_call().client_requested_completion());
}

TEST_F(ServerReaderTest, Open_ClientStreamStartsOpen) {
  RpcLockGuard lock;
  EXPECT_TRUE(reader_.as_server_call().has_client_stream());
  EXPECT_FALSE(reader_.as_server_call().client_requested_completion());
}

TEST_F(ServerReaderTest, Close_ClosesStream) {
  EXPECT_TRUE(reader_.as_server_call().active());
  rpc_lock().lock();
  EXPECT_FALSE(reader_.as_server_call().client_requested_completion());
  rpc_lock().unlock();
  EXPECT_EQ(OkStatus(),
            reader_.as_server_call().CloseAndSendResponse(OkStatus()));

  EXPECT_FALSE(reader_.as_server_call().active());
  RpcLockGuard lock;
  EXPECT_TRUE(reader_.as_server_call().client_requested_completion());
}

TEST_F(ServerReaderTest, RequestCompletion_OnlyMakesClientNotReady) {
  EXPECT_TRUE(reader_.active());
  rpc_lock().lock();
  EXPECT_FALSE(reader_.as_server_call().client_requested_completion());
  reader_.as_server_call().HandleClientRequestedCompletion();

  EXPECT_TRUE(reader_.active());
  RpcLockGuard lock;
  EXPECT_TRUE(reader_.as_server_call().client_requested_completion());
}

class ServerReaderWriterTest : public Test {
 public:
  ServerReaderWriterTest() : context_(TestService::method.method()) {
    rpc_lock().lock();
    FakeServerReaderWriter reader_writer_temp(context_.get().ClaimLocked());
    rpc_lock().unlock();
    reader_writer_ = std::move(reader_writer_temp);
  }

  ServerContextForTest<TestService> context_;
  FakeServerReaderWriter reader_writer_;
};

TEST_F(ServerReaderWriterTest, Move_MaintainsClientStream) {
  FakeServerReaderWriter destination;

  rpc_lock().lock();
  EXPECT_FALSE(destination.as_server_call().client_requested_completion());
  rpc_lock().unlock();

  destination = std::move(reader_writer_);
  RpcLockGuard lock;
  EXPECT_TRUE(destination.as_server_call().has_client_stream());
  EXPECT_FALSE(destination.as_server_call().client_requested_completion());
}

TEST_F(ServerReaderWriterTest, Move_MovesCallbacks) {
  int calls = 0;
  reader_writer_.set_on_error([&calls](Status) { calls += 1; });
  reader_writer_.set_on_next([&calls](ConstByteSpan) { calls += 1; });
  reader_writer_.set_on_completion_requested_if_enabled(
      [&calls]() { calls += 1; });

  FakeServerReaderWriter destination(std::move(reader_writer_));
  rpc_lock().lock();
  destination.as_server_call().HandlePayload({});
  rpc_lock().lock();
  destination.as_server_call().HandleClientRequestedCompletion();
  rpc_lock().lock();
  destination.as_server_call().HandleError(Status::Unknown());

  EXPECT_EQ(calls, 2 + PW_RPC_COMPLETION_REQUEST_CALLBACK);
}

TEST_F(ServerReaderWriterTest, Move_ClearsCallAndChannelId) {
  rpc_lock().lock();
  reader_writer_.set_id(999);
  EXPECT_NE(reader_writer_.channel_id_locked(), 0u);
  rpc_lock().unlock();

  FakeServerReaderWriter destination(std::move(reader_writer_));

  RpcLockGuard lock;
  EXPECT_EQ(reader_writer_.id(), 0u);
  EXPECT_EQ(reader_writer_.channel_id_locked(), 0u);
}

TEST_F(ServerReaderWriterTest, DefaultConstructorAssign_Reset) {
  reader_writer_ = {};

  RpcLockGuard lock;
  EXPECT_EQ(reader_writer_.service_id(), 0u);
  EXPECT_EQ(reader_writer_.method_id(), 0u);
}

TEST_F(ServerReaderWriterTest, Move_SourceAwaitingCleanup_CleansUpCalls) {
  std::optional<Status> on_error_cb;
  reader_writer_.set_on_error([&on_error_cb](Status error) {
    ASSERT_FALSE(on_error_cb.has_value());
    on_error_cb = error;
  });

  rpc_lock().lock();
  context_.server().CloseCallAndMarkForCleanup(reader_writer_.as_server_call(),
                                               Status::NotFound());
  rpc_lock().unlock();

  FakeServerReaderWriter destination(std::move(reader_writer_));

  EXPECT_EQ(Status::NotFound(), on_error_cb);
}

TEST_F(ServerReaderWriterTest, Move_BothAwaitingCleanup_CleansUpCalls) {
  rpc_lock().lock();
  // Use call ID 123 so this call is distinct from the other.
  FakeServerReaderWriter destination(context_.get(123).ClaimLocked());
  rpc_lock().unlock();

  std::optional<Status> destination_on_error_cb;
  destination.set_on_error([&destination_on_error_cb](Status error) {
    ASSERT_FALSE(destination_on_error_cb.has_value());
    destination_on_error_cb = error;
  });

  std::optional<Status> source_on_error_cb;
  reader_writer_.set_on_error([&source_on_error_cb](Status error) {
    ASSERT_FALSE(source_on_error_cb.has_value());
    source_on_error_cb = error;
  });

  // Simulate these two calls being closed by another thread.
  rpc_lock().lock();
  context_.server().CloseCallAndMarkForCleanup(destination.as_server_call(),
                                               Status::NotFound());
  context_.server().CloseCallAndMarkForCleanup(reader_writer_.as_server_call(),
                                               Status::Unauthenticated());
  rpc_lock().unlock();

  destination = std::move(reader_writer_);

  EXPECT_EQ(Status::NotFound(), destination_on_error_cb);
  EXPECT_EQ(Status::Unauthenticated(), source_on_error_cb);
}

TEST_F(ServerReaderWriterTest, Close_ClearsCallAndChannelId) {
  rpc_lock().lock();
  reader_writer_.set_id(999);
  EXPECT_NE(reader_writer_.channel_id_locked(), 0u);
  rpc_lock().unlock();

  EXPECT_EQ(OkStatus(), reader_writer_.Finish());

  RpcLockGuard lock;
  EXPECT_EQ(reader_writer_.id(), 0u);
  EXPECT_EQ(reader_writer_.channel_id_locked(), 0u);
}

TEST_F(ServerReaderWriterTest,
       SetCallbacks_DestroysReplacedCallbacksOutsideRpcLock) {
  bool on_next_destroyed = false;
  bool on_error_destroyed = false;
  reader_writer_.set_on_next(
      [capture = LockAcquiringCapture(on_next_destroyed)](ConstByteSpan) {});
  reader_writer_.set_on_error(
      [capture = LockAcquiringCapture(on_error_destroyed)](Status) {});

  EXPECT_FALSE(on_next_destroyed);
  EXPECT_FALSE(on_error_destroyed);

  reader_writer_.set_on_next(nullptr);
  EXPECT_TRUE(on_next_destroyed);

  reader_writer_.set_on_error(nullptr);
  EXPECT_TRUE(on_error_destroyed);

#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  bool on_completion_destroyed = false;
  reader_writer_.set_on_completion_requested_if_enabled(
      [capture = LockAcquiringCapture(on_completion_destroyed)]() {});
  EXPECT_FALSE(on_completion_destroyed);

  reader_writer_.set_on_completion_requested_if_enabled(nullptr);
  EXPECT_TRUE(on_completion_destroyed);
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
}

TEST_F(ServerReaderWriterTest, HandleError_DestroysCallbacksOutsideRpcLock) {
  bool on_next_destroyed = false;
  bool on_error_destroyed = false;
  reader_writer_.set_on_next(
      [capture = LockAcquiringCapture(on_next_destroyed)](ConstByteSpan) {});
  reader_writer_.set_on_error(
      [capture = LockAcquiringCapture(on_error_destroyed)](Status) {});
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  bool on_completion_destroyed = false;
  reader_writer_.set_on_completion_requested_if_enabled(
      [capture = LockAcquiringCapture(on_completion_destroyed)]() {});
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK

  rpc_lock().lock();
  reader_writer_.as_server_call().HandleError(Status::Aborted());

  EXPECT_TRUE(on_next_destroyed);
  EXPECT_TRUE(on_error_destroyed);
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  EXPECT_TRUE(on_completion_destroyed);
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
}

TEST_F(ServerReaderWriterTest,
       MoveAssignTemporary_DestroysPreviousCallbacksOutsideRpcLock) {
  bool on_next_destroyed = false;
  bool on_error_destroyed = false;
  reader_writer_.set_on_next(
      [capture = LockAcquiringCapture(on_next_destroyed)](ConstByteSpan) {});
  reader_writer_.set_on_error(
      [capture = LockAcquiringCapture(on_error_destroyed)](Status) {});
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  bool on_completion_destroyed = false;
  reader_writer_.set_on_completion_requested_if_enabled(
      [capture = LockAcquiringCapture(on_completion_destroyed)]() {});
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK

  reader_writer_ = FakeServerReaderWriter();

  EXPECT_FALSE(reader_writer_.active());
  EXPECT_TRUE(on_next_destroyed);
  EXPECT_TRUE(on_error_destroyed);
#if PW_RPC_COMPLETION_REQUEST_CALLBACK
  EXPECT_TRUE(on_completion_destroyed);
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK
}

TEST_F(ServerReaderWriterTest,
       MoveAssign_PreviousCallbacksDestroyedWithMovedFromCall) {
  rpc_lock().lock();
  auto source = std::make_optional<FakeServerReaderWriter>(
      context_.get(/*id=*/123).ClaimLocked());
  rpc_lock().unlock();

  bool source_destroyed = false;
  bool destination_destroyed = false;
  source->set_on_error(
      [capture = LockAcquiringCapture(source_destroyed)](Status) {});
  reader_writer_.set_on_error(
      [capture = LockAcquiringCapture(destination_destroyed)](Status) {});

  reader_writer_ = std::move(*source);

  EXPECT_TRUE(reader_writer_.active());
  EXPECT_FALSE(source_destroyed);
  EXPECT_FALSE(destination_destroyed);

  source.reset();
  EXPECT_TRUE(destination_destroyed);

  reader_writer_.set_on_error(nullptr);
  EXPECT_TRUE(source_destroyed);
}

class FakeUnaryClientCall : public UnaryResponseClientCall {
 public:
  constexpr FakeUnaryClientCall() = default;
  FakeUnaryClientCall(LockedEndpoint& client,
                      uint32_t channel_id,
                      uint32_t service_id,
                      uint32_t method_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : UnaryResponseClientCall(client,
                                channel_id,
                                service_id,
                                method_id,
                                RawCallProps(MethodType::kUnary)) {}
  FakeUnaryClientCall(FakeUnaryClientCall&&) = default;
  FakeUnaryClientCall& operator=(FakeUnaryClientCall&&) = default;
  ~FakeUnaryClientCall() { DestroyClientCall(); }
  using Call::set_on_error;
  using ClientCall::CloseAndWaitForCallbacks;
  using UnaryResponseClientCall::set_on_completed;
};

class FakeStreamClientCall : public StreamResponseClientCall {
 public:
  constexpr FakeStreamClientCall() = default;
  FakeStreamClientCall(LockedEndpoint& client,
                       uint32_t channel_id,
                       uint32_t service_id,
                       uint32_t method_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : StreamResponseClientCall(client,
                                 channel_id,
                                 service_id,
                                 method_id,
                                 RawCallProps(MethodType::kServerStreaming)) {}
  FakeStreamClientCall(FakeStreamClientCall&&) = default;
  FakeStreamClientCall& operator=(FakeStreamClientCall&&) = default;
  ~FakeStreamClientCall() { DestroyClientCall(); }
  using Call::set_on_error;
  using ClientCall::CloseAndWaitForCallbacks;
  using StreamResponseClientCall::set_on_completed;
};

TEST(ClientCallTest, UnaryHandleCompleted_DestroysCallbacksOutsideRpcLock) {
  ClientContextForTest<> context;
  bool on_completed_destroyed = false;
  bool on_error_destroyed = false;
  FakeUnaryClientCall call =
      UnaryResponseClientCall::Start<FakeUnaryClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          [capture = LockAcquiringCapture(on_completed_destroyed)](
              ConstByteSpan, Status) {},
          [capture = LockAcquiringCapture(on_error_destroyed)](Status) {},
          {});

  EXPECT_EQ(OkStatus(), context.SendResponse(OkStatus()));
  EXPECT_FALSE(call.active());
  EXPECT_TRUE(on_completed_destroyed);
  EXPECT_TRUE(on_error_destroyed);
}

TEST(ClientCallTest, StreamHandleCompleted_DestroysCallbacksOutsideRpcLock) {
  ClientContextForTest<> context;
  bool on_next_destroyed = false;
  bool on_completed_destroyed = false;
  bool on_error_destroyed = false;
  FakeStreamClientCall call =
      StreamResponseClientCall::Start<FakeStreamClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          [capture = LockAcquiringCapture(on_next_destroyed)](ConstByteSpan) {},
          [capture = LockAcquiringCapture(on_completed_destroyed)](Status) {},
          [capture = LockAcquiringCapture(on_error_destroyed)](Status) {},
          {});

  EXPECT_EQ(OkStatus(), context.SendResponse(OkStatus()));
  EXPECT_FALSE(call.active());
  EXPECT_TRUE(on_next_destroyed);
  EXPECT_TRUE(on_completed_destroyed);
  EXPECT_TRUE(on_error_destroyed);
}

TEST(ClientCallTest,
     UnaryMoveAssignTemporary_DestroysPreviousCallbacksOutsideRpcLock) {
  ClientContextForTest<> context;
  bool on_completed_destroyed = false;
  bool on_error_destroyed = false;
  FakeUnaryClientCall call =
      UnaryResponseClientCall::Start<FakeUnaryClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          [capture = LockAcquiringCapture(on_completed_destroyed)](
              ConstByteSpan, Status) {},
          [capture = LockAcquiringCapture(on_error_destroyed)](Status) {},
          {});

  call = FakeUnaryClientCall();

  EXPECT_FALSE(call.active());
  EXPECT_TRUE(on_completed_destroyed);
  EXPECT_TRUE(on_error_destroyed);
}

TEST(ClientCallTest,
     StreamMoveAssignTemporary_DestroysPreviousCallbacksOutsideRpcLock) {
  ClientContextForTest<> context;
  bool on_next_destroyed = false;
  bool on_completed_destroyed = false;
  bool on_error_destroyed = false;
  FakeStreamClientCall call =
      StreamResponseClientCall::Start<FakeStreamClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          [capture = LockAcquiringCapture(on_next_destroyed)](ConstByteSpan) {},
          [capture = LockAcquiringCapture(on_completed_destroyed)](Status) {},
          [capture = LockAcquiringCapture(on_error_destroyed)](Status) {},
          {});

  call = FakeStreamClientCall();

  EXPECT_FALSE(call.active());
  EXPECT_TRUE(on_next_destroyed);
  EXPECT_TRUE(on_completed_destroyed);
  EXPECT_TRUE(on_error_destroyed);
}

TEST(ClientCallTest, CloseAndWaitForCallbacks_CallbacksCanBeReplaced) {
  ClientContextForTest<> context;
  FakeStreamClientCall call =
      StreamResponseClientCall::Start<FakeStreamClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          /*on_next=*/nullptr,
          /*on_completed=*/nullptr,
          /*on_error=*/nullptr,
          {});
  call.CloseAndWaitForCallbacks();
  ASSERT_FALSE(call.active());

  bool destroyed = false;
  call.set_on_error([capture = LockAcquiringCapture(destroyed)](Status) {});
  EXPECT_FALSE(destroyed);

  call.set_on_error(nullptr);
  EXPECT_TRUE(destroyed);
}

TEST(ClientCallTest, CloseAndWaitForCallbacks_MoveOntoActiveCall) {
  ClientContextForTest<> context;
  auto start = [&context] {
    return StreamResponseClientCall::Start<FakeStreamClientCall>(
        context.client(),
        context.channel_id(),
        context.service_id(),
        context.method_id(),
        /*on_next=*/nullptr,
        /*on_completed=*/nullptr,
        /*on_error=*/nullptr,
        {});
  };
  FakeStreamClientCall call_1 = start();
  FakeStreamClientCall call_2 = start();
  call_2.CloseAndWaitForCallbacks();

  call_1 = std::move(call_2);
  EXPECT_FALSE(call_1.active());

  // call_1 is still a live call object, so its callbacks can be set.
  bool destroyed = false;
  call_1.set_on_error([capture = LockAcquiringCapture(destroyed)](Status) {});
  EXPECT_FALSE(destroyed);

  call_1.set_on_error(nullptr);
  EXPECT_TRUE(destroyed);
}

#if PW_RPC_DYNAMIC_ALLOCATION
TEST_F(ServerReaderWriterTest, DynamicCall_ResetInsideOnError) {
  DynamicCallPtr<FakeServerReaderWriter> empty_call;
  EXPECT_EQ(empty_call.get(), nullptr);

  struct {
    DynamicCallPtr<FakeServerReaderWriter> dynamic_call;
    bool callback_ran = false;
  } state{MakeDynamicCall(std::move(reader_writer_))};
  EXPECT_NE(state.dynamic_call.get(), nullptr);
  auto* raw_call = &state.dynamic_call->as_server_call();
  state.dynamic_call->set_on_error([&state](Status) {
    state.dynamic_call.reset();
    state.callback_ran = true;
  });

  rpc_lock().lock();
  raw_call->HandleError(Status::Cancelled());

  EXPECT_TRUE(state.callback_ran);
  EXPECT_EQ(state.dynamic_call, nullptr);
  EXPECT_EQ(state.dynamic_call.get(), nullptr);
}

TEST_F(ServerReaderWriterTest, DynamicCall_ResetInsideOnNext) {
  struct {
    DynamicCallPtr<FakeServerReaderWriter> dynamic_call;
    bool callback_ran = false;
  } state{MakeDynamicCall(std::move(reader_writer_))};
  auto* raw_call = &state.dynamic_call->as_server_call();
  state.dynamic_call->set_on_next([&state](ConstByteSpan) {
    state.dynamic_call.reset();
    state.callback_ran = true;
  });

  rpc_lock().lock();
  raw_call->HandlePayload({});

  EXPECT_TRUE(state.callback_ran);
  EXPECT_EQ(state.dynamic_call, nullptr);
}

#if PW_RPC_COMPLETION_REQUEST_CALLBACK
TEST_F(ServerReaderWriterTest, DynamicCall_ResetInsideOnCompletionRequested) {
  struct {
    DynamicCallPtr<FakeServerReaderWriter> dynamic_call;
    bool callback_ran = false;
  } state{MakeDynamicCall(std::move(reader_writer_))};
  auto* raw_call = &state.dynamic_call->as_server_call();
  state.dynamic_call->set_on_completion_requested_if_enabled([&state]() {
    state.dynamic_call.reset();
    state.callback_ran = true;
  });

  rpc_lock().lock();
  raw_call->HandleClientRequestedCompletion();

  EXPECT_TRUE(state.callback_ran);
  EXPECT_EQ(state.dynamic_call, nullptr);
}
#endif  // PW_RPC_COMPLETION_REQUEST_CALLBACK

TEST_F(ServerReaderWriterTest,
       DynamicCall_CallbackCaptureDropsLastOwnerReference) {
  struct Owner {
    DynamicCallPtr<FakeServerReaderWriter> call;
    bool* destroyed = nullptr;
    ~Owner() {
      if (destroyed != nullptr) {
        *destroyed = true;
      }
    }
  };

  bool owner_destroyed = false;
  auto owner = std::make_unique<Owner>();
  owner->destroyed = &owner_destroyed;
  owner->call = MakeDynamicCall(std::move(reader_writer_));
  Owner* raw_owner = owner.get();
  auto* raw_call = &raw_owner->call->as_server_call();

  raw_owner->call->set_on_error([captured_owner = std::move(owner)](Status) {
    // captured_owner is owned solely by the callback and destroyed when the
    // callback is cleared.
  });
  ASSERT_FALSE(owner_destroyed);

  rpc_lock().lock();
  raw_call->HandleError(Status::Aborted());

  EXPECT_TRUE(owner_destroyed);
}

TEST_F(ServerReaderWriterTest, DynamicCall_ResetCancelsPendingCleanup) {
  auto dynamic_call_1 = MakeDynamicCall(std::move(reader_writer_));
  rpc_lock().lock();
  FakeServerReaderWriter second_writer(context_.get(/*id=*/123).ClaimLocked());
  rpc_lock().unlock();

  struct {
    DynamicCallPtr<FakeServerReaderWriter> dynamic_call_2;
    bool call_1_error_called = false;
  } state{MakeDynamicCall(std::move(second_writer))};

  bool call_2_error_called = false;
  dynamic_call_1->set_on_error([&state](Status) {
    state.call_1_error_called = true;
    // Resetting dynamic_call_2 while CleanUpCalls() is draining to_cleanup_
    // must remove dynamic_call_2 from to_cleanup_ without crashing.
    state.dynamic_call_2.reset();
  });
  state.dynamic_call_2->set_on_error(
      [&call_2_error_called](Status) { call_2_error_called = true; });

  rpc_lock().lock();
  context_.server().CloseCallAndMarkForCleanup(
      state.dynamic_call_2->as_server_call(), Status::Cancelled());
  context_.server().CloseCallAndMarkForCleanup(dynamic_call_1->as_server_call(),
                                               Status::Cancelled());
  static_cast<Endpoint&>(context_.server()).CleanUpCalls();

  EXPECT_TRUE(state.call_1_error_called);
  EXPECT_FALSE(call_2_error_called);
  EXPECT_EQ(state.dynamic_call_2, nullptr);
}

TEST_F(ServerReaderWriterTest,
       DynamicCall_ResetInsideOnNextStillCleansUpOtherCalls) {
  rpc_lock().lock();
  FakeServerReaderWriter other_writer(context_.get(/*id=*/123).ClaimLocked());
  rpc_lock().unlock();

  bool other_error_called = false;
  other_writer.set_on_error(
      [&other_error_called](Status) { other_error_called = true; });

  struct {
    DynamicCallPtr<FakeServerReaderWriter> dynamic_call;
    FakeServerReaderWriter* other_writer = nullptr;
    TestServer* server = nullptr;
  } state{MakeDynamicCall(std::move(reader_writer_)),
          &other_writer,
          &context_.server()};
  auto* raw_call = &state.dynamic_call->as_server_call();
  state.dynamic_call->set_on_next([&state](ConstByteSpan) {
    // Mark another call for cleanup while this callback runs, then release
    // this call. HandlePayload() must still drain to_cleanup_ after the
    // deferred deletion of this call.
    rpc_lock().lock();
    state.server->CloseCallAndMarkForCleanup(
        state.other_writer->as_server_call(), Status::Aborted());
    rpc_lock().unlock();
    state.dynamic_call.reset();
  });

  rpc_lock().lock();
  raw_call->HandlePayload({});

  EXPECT_EQ(state.dynamic_call, nullptr);
  EXPECT_TRUE(other_error_called);
}

TEST_F(ServerReaderWriterTest, DynamicCall_SetCallbacksAfterResetInCallback) {
  struct {
    DynamicCallPtr<FakeServerReaderWriter> dynamic_call;
    FakeServerReaderWriter* call_ptr = nullptr;
    bool callback_ran = false;
  } state{MakeDynamicCall(std::move(reader_writer_))};
  state.call_ptr = state.dynamic_call.get();
  auto* raw_call = &state.dynamic_call->as_server_call();
  state.dynamic_call->set_on_next([&state](ConstByteSpan) {
    state.dynamic_call.reset();
    // Calling set_on_error / set_on_next after reset() while the callback
    // is still running must not deadlock. The callbacks are destroyed with
    // the call when this callback returns.
    state.call_ptr->set_on_error([](Status) {});
    state.call_ptr->set_on_next([](ConstByteSpan) {});
    state.callback_ran = true;
  });

  rpc_lock().lock();
  raw_call->HandlePayload({});

  EXPECT_TRUE(state.callback_ran);
  EXPECT_EQ(state.dynamic_call, nullptr);
}

TEST(ClientCallTest, DynamicCall_ResetInsideUnaryOnCompleted) {
  ClientContextForTest<> context;
  struct {
    DynamicCallPtr<FakeUnaryClientCall> dynamic_call;
    bool completed_ran = false;
  } state;
  state.dynamic_call =
      UnaryResponseClientCall::StartDynamic<FakeUnaryClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          [&state](ConstByteSpan, Status) {
            state.dynamic_call.reset();
            state.completed_ran = true;
          },
          /*on_error=*/nullptr,
          {});

  EXPECT_EQ(OkStatus(), context.SendResponse(OkStatus()));
  EXPECT_TRUE(state.completed_ran);
  EXPECT_EQ(state.dynamic_call, nullptr);
}

TEST(ClientCallTest, DynamicCall_ResetInsideStreamOnCompleted) {
  ClientContextForTest<> context;
  struct {
    DynamicCallPtr<FakeStreamClientCall> dynamic_call;
    bool completed_ran = false;
  } state;
  state.dynamic_call =
      MakeDynamicCall(StreamResponseClientCall::Start<FakeStreamClientCall>(
          context.client(),
          context.channel_id(),
          context.service_id(),
          context.method_id(),
          /*on_next=*/nullptr,
          [&state](Status) {
            state.dynamic_call.reset();
            state.completed_ran = true;
          },
          /*on_error=*/nullptr,
          {}));

  EXPECT_EQ(OkStatus(), context.SendResponse(OkStatus()));
  EXPECT_TRUE(state.completed_ran);
  EXPECT_EQ(state.dynamic_call, nullptr);
}

// A stateless custom deleter that counts deletions.
struct CountingDeleter {
  static inline int deletions = 0;

  void operator()(FakeServerReaderWriter* call) const {
    deletions += 1;
    delete call;
  }
};
using CountingPtr = std::unique_ptr<FakeServerReaderWriter, CountingDeleter>;

TEST_F(ServerReaderWriterTest, DynamicCall_CustomSmartPointer_Reset) {
  CountingDeleter::deletions = 0;
  DynamicCallPtr call(
      CountingPtr(new FakeServerReaderWriter(std::move(reader_writer_))));
  static_assert(
      std::is_same_v<decltype(call),
                     DynamicCallPtr<FakeServerReaderWriter, CountingPtr>>);
  ASSERT_TRUE(call->active());

  call.reset();
  EXPECT_EQ(CountingDeleter::deletions, 1);
}

TEST_F(ServerReaderWriterTest,
       DynamicCall_CustomSmartPointer_ResetInsideCallback) {
  CountingDeleter::deletions = 0;
  DynamicCallPtr<FakeServerReaderWriter, CountingPtr> call(
      CountingPtr(new FakeServerReaderWriter(std::move(reader_writer_))));
  auto* raw_call = &call->as_server_call();
  call->set_on_next([&call](ConstByteSpan) {
    call.reset();
    // Deletion is deferred until this callback returns.
    EXPECT_EQ(CountingDeleter::deletions, 0);
  });

  rpc_lock().lock();
  raw_call->HandlePayload({});

  EXPECT_EQ(call, nullptr);
  EXPECT_EQ(CountingDeleter::deletions, 1);
}

TEST_F(ServerReaderWriterTest, DynamicCall_MoveAssign_ClosesPreviousCall) {
  CountingDeleter::deletions = 0;
  DynamicCallPtr call_1(
      CountingPtr(new FakeServerReaderWriter(std::move(reader_writer_))));

  rpc_lock().lock();
  auto* second =
      new FakeServerReaderWriter(context_.get(/*id=*/123).ClaimLocked());
  rpc_lock().unlock();
  DynamicCallPtr call_2{CountingPtr(second)};
  ASSERT_TRUE(call_1->active());
  ASSERT_TRUE(call_2->active());

  call_1 = std::move(call_2);
  EXPECT_EQ(CountingDeleter::deletions, 1);
  EXPECT_EQ(call_1.get(), second);
  EXPECT_EQ(call_2, nullptr);  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(call_1->active());

  call_1.reset();
  EXPECT_EQ(CountingDeleter::deletions, 2);
}

TEST_F(ServerReaderWriterTest, DynamicCall_AssignNullptr_ResetsCall) {
  CountingDeleter::deletions = 0;
  DynamicCallPtr call(
      CountingPtr(new FakeServerReaderWriter(std::move(reader_writer_))));
  ASSERT_TRUE(call->active());

  call = nullptr;
  EXPECT_EQ(call, nullptr);
  EXPECT_EQ(CountingDeleter::deletions, 1);
}

static_assert(
    std::is_same_v<decltype(DynamicCallPtr(
                       std::declval<std::unique_ptr<FakeUnaryClientCall>>())),
                   DynamicCallPtr<FakeUnaryClientCall,
                                  std::unique_ptr<FakeUnaryClientCall>>>,
    "DynamicCallPtr deduces its call type from a smart pointer");
#endif  // PW_RPC_DYNAMIC_ALLOCATION

}  // namespace
}  // namespace internal
}  // namespace pw::rpc
