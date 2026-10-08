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
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_rpc2/internal/method_future.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/service.h"
#include "pw_rpc2/writer.h"
#include "pw_transport/socket.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

// `ServerCall` keeps a pointer to the `Method` it was dispatched to, which
// supplies the method ID and the size and alignment of the trailing future
// storage. Real methods live in a service's static method table; these tests
// build one-off descriptors instead.
constexpr ProtocolStatus NoInvoke(Service&, ServerCall&, ConstBuf&&) {
  return ProtocolStatus::kOk;
}

constexpr Method TestMethod(uint32_t id, size_t size) {
  return Method(id, MethodType::kBidirectionalStreaming, size, NoInvoke);
}

class MockFuture {
 public:
  using value_type = void;

  explicit MockFuture(int pends_before_ready = 1,
                      int* destruction_counter = nullptr,
                      async2::Waker* waker_out = nullptr,
                      int* pend_counter = nullptr)
      : pends_remaining_(pends_before_ready),
        destruction_counter_(destruction_counter),
        waker_out_(waker_out),
        pend_counter_(pend_counter) {}

  ~MockFuture() {
    if (destruction_counter_ != nullptr) {
      ++(*destruction_counter_);
    }
  }

  MockFuture(const MockFuture&) = delete;
  MockFuture& operator=(const MockFuture&) = delete;

  MockFuture(MockFuture&& other) noexcept
      : pends_remaining_(other.pends_remaining_),
        destruction_counter_(
            std::exchange(other.destruction_counter_, nullptr)),
        waker_out_(std::exchange(other.waker_out_, nullptr)),
        pend_counter_(std::exchange(other.pend_counter_, nullptr)) {}

  MockFuture& operator=(MockFuture&& other) noexcept {
    if (this != &other) {
      if (destruction_counter_ != nullptr) {
        ++(*destruction_counter_);
      }
      pends_remaining_ = other.pends_remaining_;
      destruction_counter_ = std::exchange(other.destruction_counter_, nullptr);
      waker_out_ = std::exchange(other.waker_out_, nullptr);
      pend_counter_ = std::exchange(other.pend_counter_, nullptr);
    }
    return *this;
  }

  bool is_pendable() const { return pends_remaining_ >= 0; }
  bool is_complete() const { return pends_remaining_ == 0; }

  async2::Poll<void> Pend(async2::Context& cx) {
    if (pend_counter_ != nullptr) {
      ++(*pend_counter_);
    }
    if (waker_out_ != nullptr) {
      PW_ASYNC_STORE_WAKER(cx, *waker_out_, "MockFuture waker");
      return async2::Pending();
    }
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
  int* destruction_counter_ = nullptr;
  async2::Waker* waker_out_ = nullptr;
  int* pend_counter_ = nullptr;
};

class ServerCallTest : public ::testing::Test {
 protected:
  ServerCallTest() {
    auto [conn, raw_conn] = test::MakeMockConnection(conn_alloc_);
    connection_ = conn;
    raw_conn_ = raw_conn;
    connection_task_ = conn_alloc_.MakeShared<ServerConnectionTask>(
        EstablishedConnection{connection_}, conn_alloc_, server_);
  }

  ~ServerCallTest() override {
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

  /// Starts the task that runs a call's method, as a connection does once it
  /// has invoked a method into the call.
  void StartCall(ServerCall& call) { dispatcher_.Post(call); }

  /// Runs the dispatcher, which polls the connection and every call started on
  /// it.
  ///
  /// A connection never completes on its own --- it is always waiting for the
  /// next packet --- so these tests run until the dispatcher stalls rather
  /// than to completion.
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
  bool posted_ = false;
};

TEST_F(ServerCallTest, SizingAndOffsetCalculations) {
  static_assert(alignof(ServerCall) == alignof(std::max_align_t));
  constexpr size_t kFutureSize = 64;

  size_t total = ServerCall::TotalAllocationSize(kFutureSize);
  EXPECT_EQ(total, sizeof(ServerCall) + kFutureSize);
}

TEST_F(ServerCallTest, RecordsIdsAndPlacesFutureStorageAfterServerCall) {
  constexpr size_t kFutureSize = 32;

  static constexpr Method kMethod = TestMethod(99, kFutureSize);
  ServerCall& call = AdoptCall(/*call_id=*/7, kMethod);

  EXPECT_EQ(call.call_id(), 7u);
  EXPECT_EQ(call.method_id(), 99u);

  std::byte* storage = call.future_storage();
  EXPECT_EQ(storage, reinterpret_cast<std::byte*>(&call) + sizeof(ServerCall));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(storage) % alignof(ServerCall), 0u);
}

TEST_F(ServerCallTest, CallRunsToCompletionAndIsRetired) {
  using ImplType = MethodFutureImpl<MockFuture>;

  int dtor_count = 0;
  static constexpr Method kMethod = TestMethod(100, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/100, kMethod);
  EXPECT_EQ(call.EmplaceFutureFromFactory<MockFuture>(
                [&] { return MockFuture(2, &dtor_count); }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  // Retiring the call destroyed the user's future and freed the call, since
  // no handle outlived the method.
  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

// A handle the method kept hold of shares ownership of the call, so the call
// outlives its retirement rather than dangling.
TEST_F(ServerCallTest, EscapedHandleKeepsTheCallAlive) {
  using ImplType = MethodFutureImpl<MockFuture>;

  int dtor_count = 0;
  static constexpr Method kMethod = TestMethod(101, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/101, kMethod);
  EXPECT_EQ(call.EmplaceFutureFromFactory<MockFuture>(
                [&] { return MockFuture(1, &dtor_count); }),
            ProtocolStatus::kOk);
  StartCall(call);

  IntrusivePtr<Call> escaped = call.shared_call();
  EXPECT_NE(escaped, nullptr);
  EXPECT_EQ(escaped.get(), static_cast<Call*>(&call));

  RunConnection();

  // The future is gone, but the call itself is not: `escaped` still refers to
  // it, detached from the connection so that writing through it fails rather
  // than touching a dead connection.
  EXPECT_EQ(dtor_count, 1);
  EXPECT_NE(alloc_.GetAllocated(), 0u);
  EXPECT_EQ(escaped->ReserveWrite(0, 8).status(), Status::Unavailable());

  escaped = nullptr;
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(ServerCallTest, CompletedCallIsRetiredEvenIfItsFutureNeverFinishes) {
  using ImplType = MethodFutureImpl<MockFuture>;

  int dtor_count = 0;
  static constexpr Method kMethod = TestMethod(102, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/102, kMethod);

  // Passing a waker makes the future park forever.
  async2::Waker waker;
  EXPECT_EQ(call.EmplaceFutureFromFactory<MockFuture>(
                [&] { return MockFuture(10, &dtor_count, &waker); }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();
  EXPECT_EQ(dtor_count, 0);

  call.Complete(Status::Cancelled());

  RunConnection();
  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

// Regression test: a `ServerCall` holds an unowned pointer to its
// `ConnectionTask`, so tearing the connection down must retire the call.
// Otherwise a later poll would reach freed memory through `QueueError()` or
// `~Writer()`.
TEST_F(ServerCallTest, ConnectionTaskDestructionRetiresServerCalls) {
  using ImplType = MethodFutureImpl<MockFuture>;

  int dtor_count = 0;
  static constexpr Method kMethod = TestMethod(104, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/104, kMethod);

  async2::Waker waker;
  EXPECT_EQ(call.EmplaceFutureFromFactory<MockFuture>(
                [&] { return MockFuture(10, &dtor_count, &waker); }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();
  ASSERT_EQ(dtor_count, 0);  // Parked, waiting on a waker nothing will fire.

  // Unposting the connection drops the dispatcher's reference, and this test
  // holds the only other one, so this destroys it.
  connection_task_->Deregister();
  connection_task_ = nullptr;

  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

// Regression test: this is the point of giving every call its own task. A
// connection wakes on every packet it reads or writes; if it polled the calls
// it owns, each of those wakes would pend every method running on the
// connection, whether or not anything that method waits on had happened.
TEST_F(ServerCallTest, WakingTheConnectionDoesNotPollCallMethods) {
  using ImplType = MethodFutureImpl<MockFuture>;

  int pend_count = 0;
  static constexpr Method kMethod = TestMethod(106, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/106, kMethod);

  // Parks on a waker only this test can fire.
  async2::Waker waker;
  EXPECT_EQ(call.EmplaceFutureFromFactory<MockFuture>([&] {
    return MockFuture(10, /*destruction_counter=*/nullptr, &waker, &pend_count);
  }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();
  ASSERT_EQ(pend_count, 1);

  // Stands in for anything that advances the connection: an inbound packet, a
  // write completing, a control packet being queued.
  connection_task_->Wake();
  RunConnection();
  EXPECT_EQ(pend_count, 1);

  // What the method is actually waiting on does reach it.
  std::move(waker).Wake();
  RunConnection();
  EXPECT_EQ(pend_count, 2);
}

class FutureHoldingWriter {
 public:
  using value_type = void;

  FutureHoldingWriter() = default;
  explicit FutureHoldingWriter(RawWriter writer) : writer_(std::move(writer)) {}

  bool is_pendable() const { return !done_; }
  bool is_complete() const { return done_; }

  async2::Poll<void> Pend(async2::Context&) {
    done_ = true;
    return async2::Ready();
  }

 private:
  std::optional<RawWriter> writer_;
  bool done_ = false;
};

// Regression test: `ServerCall::DoPend()` must not call `CloseWrite()` when the
// method future resolves. The future's local variables (such as `Writer` or
// `UnaryWriter` handles) are destroyed in `ServerCall::Retire()` ->
// `ClearUserFuture()`, which must happen while `is_write_closed()` is still
// false so their destructors can send `kStreamEnd` or `kError(CANCELLED)`.
TEST_F(ServerCallTest, RetireDestroysUserFutureBeforeClosingWrite) {
  using ImplType = MethodFutureImpl<FutureHoldingWriter>;

  static constexpr Method kMethod = TestMethod(105, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/105, kMethod);

  auto writer = CallAccess::Create<RawWriter>(call.shared_call());
  EXPECT_EQ(call.EmplaceFutureFromFactory<FutureHoldingWriter>(
                [&] { return FutureHoldingWriter(std::move(writer)); }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res = InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            (PacketType::Make<flags::kServer, flags::kOkTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 105u);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

// Checks that the only packet the server sent ends `call_id` with `error`.
void ExpectOnlyServerError(test::MockConnection& raw_conn,
                           uint32_t call_id,
                           ProtocolStatus error) {
  EXPECT_EQ(raw_conn.commit_count(), 1u);
  auto decoded = InboundPacket::Decode(raw_conn.last_written_buf());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decoded->call_id(), call_id);
  EXPECT_EQ(decoded->error(), error);
}

// A `Writer` that escapes its method cannot keep the call going once the
// method returns. The client is told the RPC ended rather than being left to
// wait for a stream end that will never come.
TEST_F(ServerCallTest, EscapedWriterIsCancelledWhenMethodReturns) {
  using ImplType = MethodFutureImpl<MockFuture>;
  static constexpr Method kMethod = TestMethod(109, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/109, kMethod);

  auto escaped = CallAccess::Create<RawWriter>(call.shared_call());
  EXPECT_EQ(
      call.EmplaceFutureFromFactory<MockFuture>([] { return MockFuture(1); }),
      ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  ExpectOnlyServerError(*raw_conn_, 109u, ProtocolStatus::kCancelled);
  EXPECT_TRUE(escaped.is_closed());

  // The call is already over, so dropping the escaped writer sends nothing.
  {
    auto dropped = std::move(escaped);
  }
  RunConnection();
  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(ServerCallTest, EscapedUnaryWriterIsDroppedWithoutResponse) {
  using ImplType = MethodFutureImpl<MockFuture>;
  static constexpr Method kMethod = TestMethod(110, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/110, kMethod);

  auto escaped = CallAccess::Create<RawUnaryWriter>(call.shared_call());
  EXPECT_EQ(
      call.EmplaceFutureFromFactory<MockFuture>([] { return MockFuture(1); }),
      ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  ExpectOnlyServerError(
      *raw_conn_, 110u, ProtocolStatus::kDroppedWithoutResponse);
  EXPECT_TRUE(escaped.is_closed());

  {
    auto dropped = std::move(escaped);
  }
  RunConnection();
  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(ServerCallTest, EmplaceInvalidFutureCrashes) {
  using ImplType = MethodFutureImpl<MockFuture>;
  static constexpr Method kMethod = TestMethod(107, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/107, kMethod);

  EXPECT_DEATH_IF_SUPPORTED(
      (void)call.EmplaceFutureFromFactory<MockFuture>(
          []() { return MockFuture(/*pends_before_ready=*/-1); }),
      "");

  connection_task_->ForceRetireAllCalls();
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

// Coroutine execution on ServerCall
async2::Coro<void> EchoCoro(async2::CoroContext, MockFuture fut) {
  co_await fut;
  co_return;
}

async2::Coro<void> ImmediateChildCoro(async2::CoroContext) { co_return; }

async2::Coro<void> FailingChildCoro(async2::CoroContext,
                                    Allocator& failing_alloc,
                                    RawUnaryWriter responder) {
  (void)responder;
  co_await ImmediateChildCoro(async2::CoroContext(failing_alloc));
  co_return;
}

TEST_F(ServerCallTest, CoroutineExecutionInServerCall) {
  using ImplType = MethodFutureImpl<async2::Coro<void>>;
  int dtor_count = 0;
  static constexpr Method kMethod = TestMethod(103, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/103, kMethod);

  EXPECT_EQ(call.EmplaceFutureFromFactory<async2::Coro<void>>([&] {
    return EchoCoro(async2::CoroContext(alloc_), MockFuture(2, &dtor_count));
  }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  EXPECT_EQ(dtor_count, 1);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

TEST_F(ServerCallTest,
       NestedCoroAllocationFailureSendsResourceExhaustedInsteadOfDropCancel) {
  using ImplType = MethodFutureImpl<async2::Coro<void>>;
  pw::allocator::test::AllocatorForTest<64> failing_alloc;
  failing_alloc.Exhaust();
  static constexpr Method kMethod = TestMethod(108, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/108, kMethod);

  EXPECT_EQ(call.EmplaceFutureFromFactory<async2::Coro<void>>([&] {
    return FailingChildCoro(
        async2::CoroContext(alloc_),
        failing_alloc,
        CallAccess::Create<RawUnaryWriter>(call.shared_call()));
  }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decode_res = InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_TRUE(decode_res.ok());
  EXPECT_EQ(decode_res->type(),
            (PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_res->call_id(), 108u);
  EXPECT_EQ(decode_res->error(),
            ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

async2::Coro<void> RespondThenKeepWorking(async2::CoroContext,
                                          RawUnaryWriter writer,
                                          async2::Waker& waker,
                                          int& dtor_count,
                                          bool& ran_after_response) {
  auto reservation = co_await writer.ReserveFinish(1);
  if (reservation.ok()) {
    (*reservation)[0] = std::byte{0x42};
    PW_CHECK_OK(reservation->Commit(1));
  }
  co_await MockFuture(10, &dtor_count, &waker);
  ran_after_response = true;
}

// Pins documented behavior: once a server method commits its final response,
// the call is retired and the method's coroutine frame is destroyed wherever
// it is suspended. Work after the response never runs.
TEST_F(ServerCallTest, WorkAfterFinalResponseIsDestroyedWithTheMethod) {
  using ImplType = MethodFutureImpl<async2::Coro<void>>;
  static constexpr Method kMethod = TestMethod(111, sizeof(ImplType));
  ServerCall& call = AdoptCall(/*call_id=*/111, kMethod);

  async2::Waker waker;
  int dtor_count = 0;
  bool ran_after_response = false;
  EXPECT_EQ(call.EmplaceFutureFromFactory<async2::Coro<void>>([&] {
    return RespondThenKeepWorking(
        async2::CoroContext(alloc_),
        CallAccess::Create<RawUnaryWriter>(call.shared_call()),
        waker,
        dtor_count,
        ran_after_response);
  }),
            ProtocolStatus::kOk);
  StartCall(call);

  RunConnection();

  // Only the response was sent; retiring the call did not add an error.
  EXPECT_EQ(raw_conn_->commit_count(), 1u);
  auto decoded = InboundPacket::Decode(raw_conn_->last_written_buf());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->type(),
            (PacketType::Make<flags::kServer,
                              flags::kHasPayload,
                              flags::kOkTerminal>()));

  // The coroutine was destroyed while suspended after the response.
  EXPECT_EQ(dtor_count, 1);
  EXPECT_FALSE(ran_after_response);
  EXPECT_EQ(alloc_.GetAllocated(), 0u);
}

#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

}  // namespace
}  // namespace pw::rpc2::internal
