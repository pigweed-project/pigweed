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

#include "pw_rpc2/internal/method.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_bytes/span.h"
#include "pw_rpc2/internal/method_future.h"
#include "pw_rpc2/internal/method_invoker.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/service.h"
#include "pw_transport/socket.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

class MockFuture {
 public:
  using value_type = void;

  explicit MockFuture(int pends_before_ready = 0)
      : pends_remaining_(pends_before_ready) {}

  bool is_pendable() const { return pends_remaining_ >= 0; }
  bool is_complete() const { return pends_remaining_ == 0; }

  async2::Poll<void> Pend(async2::Context& cx) {
    if (pends_remaining_ <= 0) {
      return async2::Ready();
    }
    --pends_remaining_;
    if (pends_remaining_ == 0) {
      return async2::Ready();
    }
    cx.ReEnqueue();
    return async2::Pending();
  }

 private:
  int pends_remaining_;
};

struct StubMsg {
  int value = 0;

  struct Serializer {
    static size_t MaxEncodedSize(const StubMsg&) { return sizeof(int); }
    static pw::StatusWithSize Serialize(const StubMsg& msg,
                                        pw::span<std::byte> dest) {
      if (dest.size() < sizeof(int)) {
        return pw::StatusWithSize::ResourceExhausted();
      }
      std::memcpy(dest.data(), &msg.value, sizeof(int));
      return pw::StatusWithSize(pw::OkStatus(), sizeof(int));
    }
    template <typename T>
    static pw::Result<StubMsg> Deserialize(pw::span<const std::byte> source) {
      if (source.size() < sizeof(int)) {
        return pw::Status::DataLoss();
      }
      StubMsg msg;
      std::memcpy(&msg.value, source.data(), sizeof(int));
      return msg;
    }
  };
};

class TestService : public Service {
 public:
  TestService() : Service(1, {}) {}

  MockFuture RawUnaryReserveFuture(pw::ConstBuf request,
                                   RawUnaryWriter responder) {
    last_request_size_ = request.size();
    auto res_fut = responder.ReserveFinish(request.size());
    return MockFuture(1);
  }

  MockFuture RawServerStreamingFuture(pw::ConstBuf request, RawWriter writer) {
    last_request_size_ = request.size();
    (void)writer;
    return MockFuture(1);
  }

  MockFuture RawClientStreamingFuture(RawReader reader,
                                      RawUnaryWriter responder) {
    (void)reader;
    (void)responder;
    return MockFuture(1);
  }

  MockFuture RawBidiStreamingFuture(RawReader reader, RawWriter writer) {
    (void)reader;
    (void)writer;
    return MockFuture(1);
  }

  MockFuture TypedUnaryFuture(const StubMsg& req,
                              UnaryWriter<StubMsg> responder) {
    last_typed_value_ = req.value;
    (void)responder;
    return MockFuture(1);
  }

  void OnRawUnary(size_t size) { last_request_size_ = size; }

  // A const method taking its request by value. Exercises the const and
  // by-value axes of `MethodTraits` end to end.
  MockFuture TypedUnaryFutureConst(StubMsg req,
                                   UnaryWriter<StubMsg> responder) const {
    last_const_value_ = req.value;
    (void)responder;
    return MockFuture(1);
  }

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
  async2::Coro<void> RawUnaryCoro(async2::CoroContext,
                                  pw::ConstBuf request,
                                  RawUnaryWriter responder) {
    last_request_size_ = request.size();
    auto res_fut = responder.ReserveFinish(request.size());
    co_return;
  }

  async2::Coro<void> RawServerStreamingCoro(async2::CoroContext,
                                            pw::ConstBuf request,
                                            RawWriter writer) {
    last_request_size_ = request.size();
    (void)writer;
    co_return;
  }

  async2::Coro<void> RawClientStreamingCoro(async2::CoroContext,
                                            RawReader reader,
                                            RawUnaryWriter responder) {
    (void)reader;
    (void)responder;
    co_return;
  }

  async2::Coro<void> RawBidiStreamingCoro(async2::CoroContext,
                                          RawReader reader,
                                          RawWriter writer) {
    (void)reader;
    (void)writer;
    co_return;
  }

  async2::Coro<void> TypedUnaryCoro(async2::CoroContext,
                                    StubMsg req,
                                    UnaryWriter<StubMsg> responder) {
    last_typed_value_ = req.value;
    (void)responder;
    co_return;
  }
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

  size_t last_request_size() const { return last_request_size_; }
  int last_typed_value() const { return last_typed_value_; }
  int last_const_value() const { return last_const_value_; }

 private:
  size_t last_request_size_ = 0;
  int last_typed_value_ = 0;
  mutable int last_const_value_ = 0;
};

// Signature classification (`MethodTraits`) is checked in
// method_traits_test.cc, which needs only `method_traits.h`.

class StatelessUnaryFuture {
 public:
  using value_type = void;
  StatelessUnaryFuture() = default;
  StatelessUnaryFuture(pw::ConstBuf request, RawUnaryWriter responder)
      : request_len_(request.size()) {
    (void)responder;
  }
  bool is_pendable() const { return !done_; }
  bool is_complete() const { return done_; }
  async2::Poll<void> Pend(async2::Context&) {
    done_ = true;
    return async2::Ready();
  }
  size_t request_len() const { return request_len_; }

 private:
  size_t request_len_ = 0;
  bool done_ = false;
};

class StatefulUnaryFuture {
 public:
  using value_type = void;
  StatefulUnaryFuture() = default;
  StatefulUnaryFuture(TestService& svc,
                      pw::ConstBuf request,
                      RawUnaryWriter responder) {
    (void)responder;
    svc.OnRawUnary(request.size());
  }
  bool is_pendable() const { return !done_; }
  bool is_complete() const { return done_; }
  async2::Poll<void> Pend(async2::Context&) {
    done_ = true;
    return async2::Ready();
  }

 private:
  bool done_ = false;
};

class MethodInvokerTest : public ::testing::Test {
 protected:
  MethodInvokerTest() {
    auto [conn, raw_conn] = test::MakeMockConnection(conn_alloc_);
    connection_ = conn;
    raw_conn_ = raw_conn;
    connection_task_ = conn_alloc_.MakeShared<ServerConnectionTask>(
        EstablishedConnection{connection_}, conn_alloc_, server_);
  }

  ~MethodInvokerTest() override {
    // The dispatcher is declared before the objects the connection task
    // refers to, so it can no longer unpost the task on the way out. Do it
    // here instead, while the server and the allocators are still alive.
    if (connection_task_ != nullptr) {
      connection_task_->Deregister();
    }
  }

  /// Allocates a server call and registers it with the connection, exactly as
  /// `ServerConnectionTask::HandleIncomingRequest()` does.
  ServerCall& AdoptCall(uint32_t call_id, const Method& method) {
    auto call_res =
        ServerCall::Allocate(*connection_task_, call_id, method, alloc_);
    PW_CHECK_OK(call_res.status());
    return **call_res;
  }

  /// Invokes `method` on `call` and starts the task that runs it, exactly
  /// as `ServerConnectionTask::HandleIncomingRequest()` does.
  ProtocolStatus InvokeIntoCall(const Method& method,
                                ServerCall& call,
                                pw::ConstBuf&& payload) {
    const ProtocolStatus error =
        method.Invoke(service_, call, std::move(payload));
    if (error == ProtocolStatus::kOk) {
      dispatcher_.Post(call);
    }
    return error;
  }

  /// Runs the dispatcher, which polls the connection and every call started on
  /// it.
  ///
  /// A connection never completes on its own --- it is always waiting for the
  /// next packet --- so this runs until the dispatcher stalls rather than to
  /// completion.
  void RunConnection() {
    if (!posted_) {
      dispatcher_.PostShared(connection_task_);
      posted_ = true;
    }
    dispatcher_.RunUntilStalled();
  }

  pw::allocator::test::AllocatorForTest<4096> conn_alloc_;
  pw::allocator::test::AllocatorForTest<4096> alloc_;
  // Declared before the server, which is bound to it for the server's life.
  async2::DispatcherForTest dispatcher_;
  ServerTask server_{conn_alloc_, dispatcher_};
  transport::ReliableDatagramSocket connection_;
  test::MockConnection* raw_conn_ = nullptr;
  SharedPtr<ServerConnectionTask> connection_task_;
  TestService service_;
  bool posted_ = false;
};

TEST_F(MethodInvokerTest, CreateMethodForFutures) {
  using RawUnary =
      RawMethodInvoker<&TestService::RawUnaryReserveFuture, MethodType::kUnary>;
  constexpr Method unary = RawUnary::CreateMethod<TestService>(1);
  EXPECT_EQ(unary.id(), 1u);
  EXPECT_EQ(unary.future_storage_size(),
            sizeof(async2::internal::BoxedFutureImpl<void, MockFuture>));

  using RawServerStreaming =
      RawMethodInvoker<&TestService::RawServerStreamingFuture,
                       MethodType::kServerStreaming>;
  constexpr Method s_stream = RawServerStreaming::CreateMethod<TestService>(2);
  EXPECT_EQ(s_stream.id(), 2u);
  EXPECT_EQ(s_stream.future_storage_size(),
            sizeof(async2::internal::BoxedFutureImpl<void, MockFuture>));

  using RawClientStreaming =
      RawMethodInvoker<&TestService::RawClientStreamingFuture,
                       MethodType::kClientStreaming>;
  constexpr Method c_stream = RawClientStreaming::CreateMethod<TestService>(3);
  EXPECT_EQ(c_stream.id(), 3u);
  EXPECT_EQ(c_stream.future_storage_size(),
            sizeof(async2::internal::BoxedFutureImpl<void, MockFuture>));

  using RawBidi = RawMethodInvoker<&TestService::RawBidiStreamingFuture,
                                   MethodType::kBidirectionalStreaming>;
  constexpr Method bidi = RawBidi::CreateMethod<TestService>(4);
  EXPECT_EQ(bidi.id(), 4u);
  EXPECT_EQ(bidi.future_storage_size(),
            sizeof(async2::internal::BoxedFutureImpl<void, MockFuture>));
}

TEST_F(MethodInvokerTest, InvokeRawUnaryFutureIntoCall) {
  using Invoker =
      RawMethodInvoker<&TestService::RawUnaryReserveFuture, MethodType::kUnary>;
  constexpr Method method = Invoker::CreateMethod<TestService>(1);

  ServerCall& call = AdoptCall(/*call_id=*/1, method);

  auto buf = pw::Buf::Allocate(conn_alloc_, 8);
  pw::ConstBuf payload(std::move(buf));
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);
  EXPECT_EQ(service_.last_request_size(), 8u);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeTypedUnaryFutureIntoCall) {
  using Invoker = MethodInvoker<&TestService::TypedUnaryFuture,
                                MethodType::kUnary,
                                StubMsg,
                                StubMsg>;
  constexpr Method method = Invoker::CreateMethod<TestService>(3);

  ServerCall& call = AdoptCall(/*call_id=*/3, method);

  int val = 12345;
  auto buf = pw::Buf::Allocate(conn_alloc_, sizeof(int));
  std::memcpy(buf.data(), &val, sizeof(int));
  pw::ConstBuf payload(std::move(buf));

  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);
  EXPECT_EQ(service_.last_typed_value(), 12345);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeConstMethodWithRequestByValue) {
  using Invoker = MethodInvoker<&TestService::TypedUnaryFutureConst,
                                MethodType::kUnary,
                                StubMsg,
                                StubMsg>;
  constexpr Method method = Invoker::CreateMethod<TestService>(14);

  ServerCall& call = AdoptCall(/*call_id=*/14, method);

  int val = 4242;
  auto buf = pw::Buf::Allocate(conn_alloc_, sizeof(int));
  std::memcpy(buf.data(), &val, sizeof(int));
  pw::ConstBuf payload(std::move(buf));

  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);
  EXPECT_EQ(service_.last_const_value(), 4242);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeTypedUnaryFutureDeserializationFailure) {
  using Invoker = MethodInvoker<&TestService::TypedUnaryFuture,
                                MethodType::kUnary,
                                StubMsg,
                                StubMsg>;
  constexpr Method method = Invoker::CreateMethod<TestService>(4);

  ServerCall& call = AdoptCall(/*call_id=*/4, method);

  // Provide only 2 bytes when sizeof(int) = 4 is required.
  auto buf = pw::Buf::Allocate(conn_alloc_, 2);
  pw::ConstBuf payload(std::move(buf));

  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kInvalidRequestPayload);

  // The server retires a call whose invocation failed, which frees it: no
  // future was ever set, so there is nothing else holding it.
  connection_task_->ForceRetireAllCalls();
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeRawServerStreamingFutureIntoCall) {
  using Invoker = RawMethodInvoker<&TestService::RawServerStreamingFuture,
                                   MethodType::kServerStreaming>;
  constexpr Method method = Invoker::CreateMethod<TestService>(6);

  ServerCall& call = AdoptCall(/*call_id=*/6, method);

  auto buf = pw::Buf::Allocate(conn_alloc_, 4);
  pw::ConstBuf payload(std::move(buf));
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeRawClientStreamingFutureIntoCall) {
  using Invoker = RawMethodInvoker<&TestService::RawClientStreamingFuture,
                                   MethodType::kClientStreaming>;
  constexpr Method method = Invoker::CreateMethod<TestService>(7);

  ServerCall& call = AdoptCall(/*call_id=*/7, method);

  pw::ConstBuf payload;
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeRawBidiStreamingFutureIntoCall) {
  using Invoker = RawMethodInvoker<&TestService::RawBidiStreamingFuture,
                                   MethodType::kBidirectionalStreaming>;
  constexpr Method method = Invoker::CreateMethod<TestService>(8);

  ServerCall& call = AdoptCall(/*call_id=*/8, method);

  pw::ConstBuf payload;
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, FutureMethodInvokerCreateMethod) {
  using Invoker =
      RawFutureMethodInvoker<StatelessUnaryFuture, MethodType::kUnary>;
  constexpr Method method = Invoker::CreateMethod<TestService>(1);
  EXPECT_EQ(method.id(), 1u);
  EXPECT_EQ(
      method.future_storage_size(),
      sizeof(async2::internal::BoxedFutureImpl<void, StatelessUnaryFuture>));
}

TEST_F(MethodInvokerTest, InvokeStatelessTypeMethodIntoCall) {
  using Invoker =
      RawFutureMethodInvoker<StatelessUnaryFuture, MethodType::kUnary>;
  constexpr Method method = Invoker::CreateMethod<TestService>(12);

  ServerCall& call = AdoptCall(/*call_id=*/12, method);

  auto buf = pw::Buf::Allocate(conn_alloc_, 4);
  pw::ConstBuf payload(std::move(buf));
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeStatefulTypeMethodIntoCall) {
  using Invoker =
      RawFutureMethodInvoker<StatefulUnaryFuture, MethodType::kUnary>;
  constexpr Method method = Invoker::CreateMethod<TestService>(13);

  ServerCall& call = AdoptCall(/*call_id=*/13, method);

  auto buf = pw::Buf::Allocate(conn_alloc_, 6);
  pw::ConstBuf payload(std::move(buf));
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);
  EXPECT_EQ(service_.last_request_size(), 6u);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST(MethodTest, StorageSizeBounds) {
  constexpr auto stub_invoke =
      [](Service&, ServerCall&, ConstBuf&&) -> ProtocolStatus {
    return ProtocolStatus::kOk;
  };

  constexpr Method m1(101, MethodType::kUnary, 0, stub_invoke);
  static_assert(m1.id() == 101);
  static_assert(m1.future_storage_size() == 0);
  EXPECT_EQ(m1.id(), 101u);
  EXPECT_EQ(m1.future_storage_size(), 0u);

  constexpr Method m2(102, MethodType::kServerStreaming, 128, stub_invoke);
  static_assert(m2.id() == 102);
  static_assert(m2.future_storage_size() == 128);
  EXPECT_EQ(m2.id(), 102u);
  EXPECT_EQ(m2.future_storage_size(), 128u);

  // The maximum supported size.
  constexpr Method m3(103,
                      MethodType::kBidirectionalStreaming,
                      Method::kMaxSizeBytes,
                      stub_invoke);
  static_assert(m3.id() == 103);
  static_assert(m3.future_storage_size() == Method::kMaxSizeBytes);
  EXPECT_EQ(m3.id(), 103u);
  EXPECT_EQ(m3.future_storage_size(), Method::kMaxSizeBytes);
}

TEST(MethodTest, StoresMethodType) {
  constexpr auto stub_invoke =
      [](Service&, ServerCall&, ConstBuf&&) -> ProtocolStatus {
    return ProtocolStatus::kOk;
  };

  constexpr Method unary(1, MethodType::kUnary, 0, stub_invoke);
  constexpr Method server_stream(
      2, MethodType::kServerStreaming, 0, stub_invoke);
  constexpr Method client_stream(
      3, MethodType::kClientStreaming, 0, stub_invoke);
  constexpr Method bidi(4, MethodType::kBidirectionalStreaming, 0, stub_invoke);

  static_assert(unary.type() == MethodType::kUnary);
  static_assert(server_stream.type() == MethodType::kServerStreaming);
  static_assert(client_stream.type() == MethodType::kClientStreaming);
  static_assert(bidi.type() == MethodType::kBidirectionalStreaming);

  static_assert(!HasClientStream(unary.type()));
  static_assert(!HasServerStream(unary.type()));
  static_assert(!HasClientStream(server_stream.type()));
  static_assert(HasServerStream(server_stream.type()));
  static_assert(HasClientStream(client_stream.type()));
  static_assert(!HasServerStream(client_stream.type()));
  static_assert(HasClientStream(bidi.type()));
  static_assert(HasServerStream(bidi.type()));
}

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

TEST_F(MethodInvokerTest, CreateMethodForCoros) {
  using RawUnaryCoro =
      RawMethodInvoker<&TestService::RawUnaryCoro, MethodType::kUnary>;
  constexpr Method unary = RawUnaryCoro::CreateMethod<TestService>(1);
  EXPECT_EQ(unary.id(), 1u);
  EXPECT_EQ(unary.future_storage_size(),
            sizeof(MethodFutureImpl<async2::Coro<void>>));

  using RawBidiCoro = RawMethodInvoker<&TestService::RawBidiStreamingCoro,
                                       MethodType::kBidirectionalStreaming>;
  constexpr Method bidi = RawBidiCoro::CreateMethod<TestService>(2);
  EXPECT_EQ(bidi.id(), 2u);
  EXPECT_EQ(bidi.future_storage_size(),
            sizeof(MethodFutureImpl<async2::Coro<void>>));
}

TEST_F(MethodInvokerTest, InvokeRawUnaryCoroIntoCall) {
  using Invoker =
      RawMethodInvoker<&TestService::RawUnaryCoro, MethodType::kUnary>;
  constexpr Method method = Invoker::CreateMethod<TestService>(2);

  ServerCall& call = AdoptCall(/*call_id=*/2, method);

  auto buf = pw::Buf::Allocate(conn_alloc_, 16);
  pw::ConstBuf payload(std::move(buf));
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(service_.last_request_size(), 16u);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeTypedUnaryCoroIntoCall) {
  using Invoker = MethodInvoker<&TestService::TypedUnaryCoro,
                                MethodType::kUnary,
                                StubMsg,
                                StubMsg>;
  constexpr Method method = Invoker::CreateMethod<TestService>(5);

  ServerCall& call = AdoptCall(/*call_id=*/5, method);

  int val = 9999;
  auto buf = pw::Buf::Allocate(conn_alloc_, sizeof(int));
  std::memcpy(buf.data(), &val, sizeof(int));
  pw::ConstBuf payload(std::move(buf));

  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(service_.last_typed_value(), 9999);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeRawServerStreamingCoroIntoCall) {
  using Invoker = RawMethodInvoker<&TestService::RawServerStreamingCoro,
                                   MethodType::kServerStreaming>;
  constexpr Method method = Invoker::CreateMethod<TestService>(9);

  ServerCall& call = AdoptCall(/*call_id=*/9, method);

  auto buf = pw::Buf::Allocate(conn_alloc_, 4);
  pw::ConstBuf payload(std::move(buf));
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeRawClientStreamingCoroIntoCall) {
  using Invoker = RawMethodInvoker<&TestService::RawClientStreamingCoro,
                                   MethodType::kClientStreaming>;
  constexpr Method method = Invoker::CreateMethod<TestService>(10);

  ServerCall& call = AdoptCall(/*call_id=*/10, method);

  pw::ConstBuf payload;
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest, InvokeRawBidiStreamingCoroIntoCall) {
  using Invoker = RawMethodInvoker<&TestService::RawBidiStreamingCoro,
                                   MethodType::kBidirectionalStreaming>;
  constexpr Method method = Invoker::CreateMethod<TestService>(11);

  ServerCall& call = AdoptCall(/*call_id=*/11, method);

  pw::ConstBuf payload;
  ProtocolStatus error = InvokeIntoCall(method, call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kOk);

  RunConnection();

  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(MethodInvokerTest,
       CoroutineFrameAllocationFailureReturnsResourceExhaustedWithoutCancel) {
  using UnaryCoroInvoker =
      RawMethodInvoker<&TestService::RawUnaryCoro, MethodType::kUnary>;
  constexpr Method unary_method =
      UnaryCoroInvoker::CreateMethod<TestService>(20);

  ServerCall& unary_call = AdoptCall(/*call_id=*/20, unary_method);
  auto buf = pw::Buf::Allocate(conn_alloc_, 4);
  pw::ConstBuf payload(std::move(buf));

  // Exhaust the connection allocator so coroutine frame allocation fails.
  conn_alloc_.Exhaust();
  ProtocolStatus error =
      InvokeIntoCall(unary_method, unary_call, std::move(payload));
  EXPECT_EQ(error, ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning);
  // The responder destructor must NOT have closed the write side or queued a
  // CANCELLED packet, leaving the call open for ServerConnectionTask to send
  // RESOURCE_EXHAUSTED.
  EXPECT_FALSE(unary_call.is_write_closed());
  EXPECT_EQ(raw_conn_->written_packet_count(), 0u);
  // ServerConnectionTask queues the error and closes the write side before
  // retiring a call whose invocation failed. Closing it here keeps retirement
  // from ending the call itself.
  unary_call.CloseWrite();
  connection_task_->ForceRetireAllCalls();

  using StreamCoroInvoker =
      RawMethodInvoker<&TestService::RawServerStreamingCoro,
                       MethodType::kServerStreaming>;
  constexpr Method stream_method =
      StreamCoroInvoker::CreateMethod<TestService>(21);

  ServerCall& stream_call = AdoptCall(/*call_id=*/21, stream_method);
  pw::ConstBuf stream_payload;
  ProtocolStatus stream_error =
      InvokeIntoCall(stream_method, stream_call, std::move(stream_payload));
  EXPECT_EQ(stream_error,
            ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning);
  // The writer destructor must NOT have closed the write side or queued a
  // StreamEnd (EOF) packet.
  EXPECT_FALSE(stream_call.is_write_closed());
  EXPECT_EQ(raw_conn_->written_packet_count(), 0u);
  // ServerConnectionTask queues the error and closes the write side before
  // retiring a call whose invocation failed. Closing it here keeps retirement
  // from ending the call itself.
  stream_call.CloseWrite();
  connection_task_->ForceRetireAllCalls();
}

#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

}  // namespace
}  // namespace pw::rpc2::internal
