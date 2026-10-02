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

#include "pw_rpc2/server.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <variant>

#include "pw_allocator/fault_injecting_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/method_invoker.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/packet_testing.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/service.h"
#include "pw_rpc2/writer.h"
#include "pw_span/span.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2 {
namespace {

namespace flags = ::pw::rpc2::internal::flags;

// Awaits a `ControlFuture` on the dispatcher. A future only makes progress
// while a task is pending it, so tests cannot simply run the dispatcher and
// inspect `is_complete()`.
class ControlTask : public async2::Task {
 public:
  explicit ControlTask(ControlFuture&& future)
      : async2::Task(PW_ASYNC_TASK_NAME("ControlTask")),
        future_(std::move(future)) {}

  [[nodiscard]] bool done() const { return done_; }

  // Only valid once `done()`.
  [[nodiscard]] Status status() const { return status_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    PW_TRY_READY_ASSIGN(status_, future_.Pend(cx));
    done_ = true;
    return async2::Ready();
  }

  ControlFuture future_;
  bool done_ = false;
  Status status_ = Status::Unknown();
};

class MockServerListener : public transport::ReliableDatagramListener {
 public:
  MockServerListener() = default;
  ~MockServerListener() override {
    accept_provider_.Resolve(Status::Cancelled());
  }

  transport::ReliableDatagramListener::AcceptFuture Accept() override {
    ++accept_calls_;
    return accept_provider_.Get();
  }

  void ResolveAccept(Result<transport::ReliableDatagramSocket> res) {
    accept_provider_.Resolve(std::move(res));
  }

  // Number of times the server has called `Accept()`; nonzero once the server
  // has registered this listener and polled it.
  size_t accept_calls() const { return accept_calls_; }

 private:
  async2::ValueProvider<Result<transport::ReliableDatagramSocket>>
      accept_provider_;
  size_t accept_calls_ = 0;
};

struct ReadyFuture {
  using value_type = void;
  bool is_pendable() const { return !completed; }
  bool is_complete() const { return completed; }
  async2::Poll<> Pend(async2::Context&) {
    completed = true;
    return async2::Ready();
  }

  bool completed = false;
};

class TestEchoService : public Service {
 public:
  explicit TestEchoService(uint32_t service_id)
      : Service(service_id, methods_),
        methods_({
            internal::Method(1u,
                             MethodType::kUnary,
                             sizeof(internal::MethodFutureImpl<ReadyFuture>),
                             &InvokeMethod1),
            internal::Method(2u,
                             MethodType::kUnary,
                             sizeof(internal::MethodFutureImpl<ReadyFuture>),
                             &InvokeMethod2),
        }) {}

  uint32_t last_dispatched_method() const { return last_dispatched_method_; }
  size_t last_payload_size() const { return last_payload_size_; }

 private:
  static internal::ProtocolStatus InvokeMethod1(Service& service,
                                                internal::ServerCall& call,
                                                ConstBuf&& request_payload) {
    auto& self = static_cast<TestEchoService&>(service);
    self.last_dispatched_method_ = 1u;
    self.last_payload_size_ = request_payload.size();

    auto responder =
        internal::CallAccess::Create<RawUnaryWriter>(call.shared_call());
    auto res_fut = responder.ReserveFinish(request_payload.size());
    return call.EmplaceFutureFromFactory<ReadyFuture>(
        [] { return ReadyFuture{}; });
  }

  static internal::ProtocolStatus InvokeMethod2(Service& service,
                                                internal::ServerCall&,
                                                ConstBuf&& request_payload) {
    auto& self = static_cast<TestEchoService&>(service);
    self.last_dispatched_method_ = 2u;
    self.last_payload_size_ = request_payload.size();
    return internal::ProtocolStatus::kInvalidRequestPayload;
  }

  std::array<internal::Method, 2> methods_;
  uint32_t last_dispatched_method_ = 0;
  size_t last_payload_size_ = 0;
};

// A future that never completes, so the `ServerCall` holding it stays open
// until something tears it down.
class StallFuture {
 public:
  using value_type = void;

  // Default constructed futures are empty and never pended.
  StallFuture() = default;

  explicit StallFuture(int* destroyed) : destroyed_(destroyed) {}

  ~StallFuture() {
    if (destroyed_ != nullptr) {
      ++(*destroyed_);
    }
  }

  StallFuture(const StallFuture&) = delete;
  StallFuture& operator=(const StallFuture&) = delete;

  StallFuture(StallFuture&& other) noexcept
      : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

  StallFuture& operator=(StallFuture&& other) noexcept {
    if (this != &other) {
      destroyed_ = std::exchange(other.destroyed_, nullptr);
    }
    return *this;
  }

  bool is_pendable() const { return true; }
  bool is_complete() const { return false; }

  async2::Poll<> Pend(async2::Context& cx) {
    PW_ASYNC_STORE_WAKER(cx, waker_, "StallFuture");
    return async2::Pending();
  }

 private:
  int* destroyed_ = nullptr;
  async2::Waker waker_;
};

// Service whose only method parks forever.
class StallingService : public Service {
 public:
  using Impl = internal::MethodFutureImpl<StallFuture>;

  explicit StallingService(uint32_t service_id)
      : Service(service_id, methods_),
        methods_({
            internal::Method(
                1u, MethodType::kUnary, sizeof(Impl), &InvokeStall),
        }) {}

  // Incremented when the parked future is destroyed, which happens only when
  // its `ServerCall` is torn down.
  int future_destructions() const { return future_destructions_; }

 private:
  static internal::ProtocolStatus InvokeStall(Service& service,
                                              internal::ServerCall& call,
                                              ConstBuf&&) {
    auto& self = static_cast<StallingService&>(service);
    return call.EmplaceFutureFromFactory<StallFuture>(
        [&] { return StallFuture(&self.future_destructions_); });
  }

  std::array<internal::Method, 1> methods_;
  int future_destructions_ = 0;
};

void EstablishServerConnection(Allocator& allocator,
                               async2::DispatcherForTest& dispatcher,
                               [[maybe_unused]] Server& server,
                               MockServerListener& transport,
                               test::MockConnection* raw_conn,
                               transport::ReliableDatagramSocket conn) {
  dispatcher.RunUntilStalled();
  transport.ResolveAccept(conn);
  dispatcher.RunUntilStalled();

  // Send client SYN
  Buf req_buf =
      Buf::Allocate(allocator, internal::HandshakePacket::kWireSizeBytes);
  auto hs_pkt =
      internal::HandshakePacket(internal::HandshakePacket::Type::kSyn);
  auto enc_res = hs_pkt.Encode(std::move(req_buf));
  PW_CHECK(enc_res.ok());
  raw_conn->SetNextRead(std::move(enc_res.value()));

  dispatcher.RunUntilStalled();
  PW_ASSERT(raw_conn->commit_count() == 1u);  // 1 handshake response (SYN-ACK)

  // Send client ACK
  Buf ack_buf =
      Buf::Allocate(allocator, internal::HandshakePacket::kWireSizeBytes);
  auto ack_pkt =
      internal::HandshakePacket(internal::HandshakePacket::Type::kAck);
  auto ack_enc = ack_pkt.Encode(std::move(ack_buf));
  PW_CHECK(ack_enc.ok());
  raw_conn->SetNextRead(std::move(ack_enc.value()));

  dispatcher.RunUntilStalled();
}

TEST(ServerTest, RegisterTransportAndAcceptConnectionWithHandshake) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  dispatcher.RunUntilStalled();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, ServiceMethodDispatchRouting) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  // Send a request packet targeting service 100, method 1
  std::byte payload[4] = {
      std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(
      allocator, /*call_id=*/5, /*service_id=*/100u, /*method_id=*/1u, payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  dispatcher.RunUntilStalled();

  EXPECT_EQ(echo_service.last_dispatched_method(), 1u);
  EXPECT_EQ(echo_service.last_payload_size(), 4u);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, ServiceNotFoundRepliesError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  size_t commits_before = raw_conn->commit_count();

  // Send a request packet targeting unregistered service 999
  std::byte payload[2] = {std::byte{1}, std::byte{2}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(
      allocator, /*call_id=*/7, /*service_id=*/999u, /*method_id=*/1u, payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  dispatcher.RunUntilStalled();

  // Server should reply with a server error packet (kUnknownService).
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  auto decode_err =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_err.ok());
  EXPECT_EQ(
      decode_err->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_err->call_id(), 7u);
  EXPECT_EQ(decode_err->error(), internal::ProtocolStatus::kUnknownService);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, MethodNotFoundRepliesError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  size_t commits_before = raw_conn->commit_count();

  // Send a request packet targeting unknown method 999
  std::byte payload[2] = {std::byte{1}, std::byte{2}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(allocator,
                                                           /*call_id=*/8,
                                                           /*service_id=*/100u,
                                                           /*method_id=*/999u,
                                                           payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  dispatcher.RunUntilStalled();

  // Server should reply with ProtocolStatus::kUnknownMethod error frame
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  auto decode_err =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_err.ok());
  EXPECT_EQ(
      decode_err->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_err->call_id(), 8u);
  EXPECT_EQ(decode_err->error(), internal::ProtocolStatus::kUnknownMethod);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, RegisterDuplicateServiceIdCrashes) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  TestEchoService duplicate_service(100u);

  // Queuing succeeds; the duplicate is detected when the dispatcher applies
  // the registrations.
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());
  EXPECT_EQ(server.RegisterService(duplicate_service), OkStatus());
  server.Start();
  EXPECT_DEATH_IF_SUPPORTED(dispatcher.RunUntilStalled(), "");

  // The server in this process really was started, so it has to be closed.
  // Closing discards the queued registrations instead of applying them.
  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, StartTwiceCrashes) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  server.Start();

  EXPECT_DEATH_IF_SUPPORTED(server.Start(), "");

  // The server in this process really was started, so it has to be closed.
  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, DestroyingUnclosedServerCrashes) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;
  MockServerListener transport;

  EXPECT_DEATH_IF_SUPPORTED(
      {
        Server server(allocator, dispatcher);
        EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
        server.Start();
      },
      "");
}

TEST(ServerTest, CloseTearsDownListenersAndConnections) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  MockServerListener transport;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  {
    Server server(allocator, dispatcher);
    EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
    server.Start();
    EstablishServerConnection(
        allocator, dispatcher, server, transport, raw_conn, conn);
    ASSERT_FALSE(raw_conn->is_closed());

    ControlTask close_task(server.Close());
    dispatcher.Post(close_task);
    dispatcher.RunUntilStalled();
    EXPECT_TRUE(close_task.done());
    EXPECT_TRUE(raw_conn->is_closed());
  }

  // Nothing the server owned may remain posted to the dispatcher.
  dispatcher.RunUntilStalled();
}

TEST(ServerTest, CloseBlockingTearsDownListenersAndConnections) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  MockServerListener transport;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);

  Server server(allocator, dispatcher);
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);
  ASSERT_FALSE(raw_conn->is_closed());

  pw::thread::test::TestThreadContext context;
  pw::Thread thread(context.options(), [&server] { server.CloseBlocking(); });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  thread.join();

  EXPECT_TRUE(raw_conn->is_closed());
}

// The listener teardown path runs on the dispatcher when `Close()` is polled,
// closing every connection task and retiring any parked `ServerCall`s so that
// nothing retains a dangling pointer to the connection.
TEST(ServerTest, CloseDrainsInFlightServerCalls) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  StallingService stalling_service(200u);
  EXPECT_EQ(server.RegisterService(stalling_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  std::byte payload[1] = {std::byte{1}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(allocator,
                                                           /*call_id=*/11,
                                                           /*service_id=*/200u,
                                                           /*method_id=*/1u,
                                                           payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  dispatcher.RunUntilStalled();

  // The call is dispatched and parked, so the connection still owns it.
  ASSERT_EQ(stalling_service.future_destructions(), 0);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());

  // Teardown on the dispatcher drained the parked call task.
  EXPECT_EQ(stalling_service.future_destructions(), 1);
  EXPECT_TRUE(raw_conn->is_closed());
}

// Every thread-safe control operation may be driven from a thread other than
// the dispatcher's, which is what the `*Blocking()` forms are for.
TEST(ServerTest, BlockingControlOperationsRunFromAnotherThread) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  MockServerListener transport;

  server.Start();

  Status register_service = Status::Unknown();
  Status register_listener = Status::Unknown();

  auto run_control_ops = [&] {
    register_service = server.RegisterService(echo_service);
    register_listener = server.RegisterListenerBlocking(transport);
    server.CloseBlocking();
  };
  pw::thread::test::TestThreadContext context;
  pw::Thread thread(context.options(),
                    [&run_control_ops] { run_control_ops(); });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  thread.join();

  EXPECT_EQ(register_service, OkStatus());
  EXPECT_EQ(register_listener, OkStatus());
  EXPECT_EQ(transport.accept_calls(), 1u);
}

TEST(ServerTest, RegisterListenerFailsWhenRecordAllocationFails) {
  allocator::test::AllocatorForTest<16384> backing_allocator;
  allocator::test::FaultInjectingAllocator failing_allocator(backing_allocator);
  async2::DispatcherForTest dispatcher;

  Server server(failing_allocator, dispatcher);
  MockServerListener transport;

  failing_allocator.DisableAllocate();
  EXPECT_EQ(server.RegisterListenerBlocking(transport),
            Status::ResourceExhausted());
}

TEST(ServerTest, RegisterMultipleListenersWhileRunningDoesNotCrash) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  MockServerListener listener1;
  MockServerListener listener2;
  MockServerListener listener3;

  server.Start();

  ControlTask reg1(server.RegisterListener(listener1));
  ControlTask reg2(server.RegisterListener(listener2));
  ControlTask reg3(server.RegisterListener(listener3));

  dispatcher.Post(reg1);
  dispatcher.Post(reg2);
  dispatcher.Post(reg3);

  dispatcher.RunUntilStalled();

  EXPECT_TRUE(reg1.done());
  EXPECT_EQ(reg1.status(), OkStatus());
  EXPECT_TRUE(reg2.done());
  EXPECT_EQ(reg2.status(), OkStatus());
  EXPECT_TRUE(reg3.done());
  EXPECT_EQ(reg3.status(), OkStatus());

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, RegistrationBeforeStart) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService service1(100u);
  TestEchoService service2(200u);
  MockServerListener listener1;
  MockServerListener listener2;

  // Service registration is queued without allocating.
  EXPECT_EQ(server.RegisterService(service1), OkStatus());
  EXPECT_EQ(server.RegisterService(service2), OkStatus());
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);

  // Before Start() there is nothing to apply listener registrations, so both
  // forms apply them on the calling thread: the async future comes back
  // already resolved.
  ControlTask listener_task(server.RegisterListener(listener1));
  EXPECT_EQ(allocator.metrics().num_allocations.value(), 1u);
  EXPECT_EQ(server.RegisterListenerBlocking(listener2), OkStatus());
  EXPECT_EQ(allocator.metrics().num_allocations.value(), 2u);

  dispatcher.Post(listener_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(listener_task.done());
  EXPECT_EQ(listener_task.status(), OkStatus());

  // Listeners only begin accepting once the server runs.
  EXPECT_EQ(listener1.accept_calls(), 0u);
  EXPECT_EQ(listener2.accept_calls(), 0u);

  server.Start();
  dispatcher.RunUntilStalled();
  EXPECT_EQ(listener1.accept_calls(), 1u);
  EXPECT_EQ(listener2.accept_calls(), 1u);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

// Closing a server that was never started completes immediately, drops the
// queued service registrations, and frees the listeners registered so far.
TEST(ServerTest, CloseBeforeStartDropsQueuedRequests) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService service(100u);
  MockServerListener listener1;
  MockServerListener listener2;

  EXPECT_EQ(server.RegisterService(service), OkStatus());
  ControlTask listener_task(server.RegisterListener(listener1));
  EXPECT_EQ(server.RegisterListenerBlocking(listener2), OkStatus());
  EXPECT_EQ(allocator.metrics().num_allocations.value(), 2u);

  ControlTask close_task(server.Close());
  dispatcher.Post(listener_task);
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(close_task.done());
  EXPECT_EQ(close_task.status(), OkStatus());
  ASSERT_TRUE(listener_task.done());
  EXPECT_EQ(listener_task.status(), OkStatus());

  // Both listener records were freed by the close.
  EXPECT_EQ(allocator.metrics().allocated_bytes.value(), 0u);
  EXPECT_EQ(server.RegisterService(service), Status::FailedPrecondition());
}

// Once a close is requested, registrations fail, and the close tears down
// every call in flight.
TEST(ServerTest, RegistrationWhileClosingFails) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  StallingService stalling_service(200u);
  TestEchoService echo_service(100u);
  EXPECT_EQ(server.RegisterService(stalling_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  std::byte payload[1] = {std::byte{1}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(allocator,
                                                           /*call_id=*/11u,
                                                           /*service_id=*/200u,
                                                           /*method_id=*/1u,
                                                           payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));
  dispatcher.RunUntilStalled();
  ASSERT_EQ(stalling_service.future_destructions(), 0);

  MockServerListener late_listener;
  ControlTask close_task(server.Close());
  EXPECT_EQ(server.RegisterService(echo_service), Status::FailedPrecondition());
  ControlTask listener_task(server.RegisterListener(late_listener));
  dispatcher.Post(close_task);
  dispatcher.Post(listener_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(close_task.done());
  EXPECT_EQ(close_task.status(), OkStatus());
  ASSERT_TRUE(listener_task.done());
  EXPECT_EQ(listener_task.status(), Status::FailedPrecondition());
  EXPECT_EQ(stalling_service.future_destructions(), 1);
}

TEST(ServerTest, ControlOperationsAfterCloseResolveImmediately) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService service(100u);
  MockServerListener listener;
  EXPECT_EQ(server.RegisterService(service), OkStatus());
  server.Start();

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(close_task.done());

  EXPECT_EQ(server.RegisterService(service), Status::FailedPrecondition());
  EXPECT_EQ(server.RegisterListenerBlocking(listener),
            Status::FailedPrecondition());

  ControlTask listener_task(server.RegisterListener(listener));
  ControlTask second_close_task(server.Close());
  dispatcher.Post(listener_task);
  dispatcher.Post(second_close_task);
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(listener_task.done());
  EXPECT_EQ(listener_task.status(), Status::FailedPrecondition());
  ASSERT_TRUE(second_close_task.done());
  EXPECT_EQ(second_close_task.status(), OkStatus());

  server.CloseBlocking();  // Also a no-op once closed.
}

// The async control operations may be submitted from a thread other than the
// dispatcher's; the resulting futures resolve once the dispatcher applies them.
TEST(ServerTest, AsyncControlOperationsSubmittedFromAnotherThread) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  MockServerListener transport;
  server.Start();

  Status register_service = Status::Unknown();
  std::optional<ControlFuture> register_listener;

  auto submit_control_ops = [&] {
    register_service = server.RegisterService(echo_service);
    register_listener = server.RegisterListener(transport);
  };
  pw::thread::test::TestThreadContext context;
  pw::Thread thread(context.options(),
                    [&submit_control_ops] { submit_control_ops(); });
  thread.join();

  EXPECT_EQ(register_service, OkStatus());

  ControlTask register_listener_task(std::move(*register_listener));
  dispatcher.Post(register_listener_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(register_listener_task.done());
  EXPECT_EQ(register_listener_task.status(), OkStatus());
  EXPECT_EQ(transport.accept_calls(), 1u);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, AsyncControlOperationsWhenAllocatorExhausted) {
  allocator::test::AllocatorForTest<16384> backing_allocator;
  allocator::test::FaultInjectingAllocator failing_allocator(backing_allocator);
  async2::DispatcherForTest dispatcher;

  Server server(failing_allocator, dispatcher);
  TestEchoService echo_service(100u);
  MockServerListener transport;
  server.Start();

  // Service registration does not allocate and succeeds even when the
  // allocator is exhausted; only the listener's record allocation fails.
  failing_allocator.DisableAllocate();
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());
  ControlTask register_listener_task(server.RegisterListener(transport));
  dispatcher.Post(register_listener_task);
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(register_listener_task.done());
  EXPECT_EQ(register_listener_task.status(), Status::ResourceExhausted());
  EXPECT_EQ(transport.accept_calls(), 0u);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, DroppingQueuedRegisterListenerFutureWithdrawsRequest) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  MockServerListener listener1;
  MockServerListener listener2;
  server.Start();

  ControlFuture reg1 = server.RegisterListener(listener1);
  {
    // Queue a second registration behind reg1, then drop its future before
    // the dispatcher drains the inbox.
    ControlFuture reg2 = server.RegisterListener(listener2);
  }

  ControlTask task1(std::move(reg1));
  dispatcher.Post(task1);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task1.done());
  EXPECT_EQ(task1.status(), OkStatus());

  // Only listener1 was registered: one record was allocated and only
  // listener1 has been asked to accept.
  EXPECT_EQ(allocator.metrics().num_allocations.value(), 1u);
  EXPECT_EQ(listener1.accept_calls(), 1u);
  EXPECT_EQ(listener2.accept_calls(), 0u);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, MovingQueuedControlFuturesPreservesRequests) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  MockServerListener listener1;
  MockServerListener listener2;
  server.Start();

  ControlFuture first = server.RegisterListener(listener1);
  ControlFuture second = server.RegisterListener(listener2);
  ControlFuture close = server.Close();

  // Move the queued futures around, both by construction and by assignment,
  // while they are still linked into the server's lists.
  ControlFuture moved_second = std::move(second);
  ControlFuture moved_close;
  moved_close = std::move(close);
  ControlFuture moved_first = std::move(first);

  ControlTask task1(std::move(moved_first));
  ControlTask task2(std::move(moved_second));
  ControlTask task3(std::move(moved_close));
  dispatcher.Post(task1);
  dispatcher.Post(task2);
  dispatcher.Post(task3);
  dispatcher.RunUntilStalled();

  // The close was requested before the listener requests were applied, so
  // they fail; the close itself succeeds.
  EXPECT_TRUE(task1.done());
  EXPECT_EQ(task1.status(), Status::FailedPrecondition());
  EXPECT_TRUE(task2.done());
  EXPECT_EQ(task2.status(), Status::FailedPrecondition());
  EXPECT_TRUE(task3.done());
  EXPECT_EQ(task3.status(), OkStatus());
}

TEST(ServerTest, CallAllocationFailureRepliesError) {
  allocator::test::AllocatorForTest<16384> backing_allocator;
  allocator::test::FaultInjectingAllocator failing_allocator(backing_allocator);
  async2::DispatcherForTest dispatcher;

  Server server(failing_allocator, dispatcher);
  TestEchoService echo_service(100u);
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(backing_allocator);
  EstablishServerConnection(
      backing_allocator, dispatcher, server, transport, raw_conn, conn);

  const size_t commits_before = raw_conn->commit_count();

  std::byte payload[1] = {std::byte{1}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(backing_allocator,
                                                           /*call_id=*/12u,
                                                           /*service_id=*/100u,
                                                           /*method_id=*/1u,
                                                           payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));

  failing_allocator.DisableAllocate();
  dispatcher.RunUntilStalled();
  failing_allocator.EnableAllocate();

  EXPECT_EQ(echo_service.last_dispatched_method(), 0u);
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  auto decode_err =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_err.ok());
  EXPECT_EQ(
      decode_err->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_err->call_id(), 12u);
  EXPECT_EQ(decode_err->error(),
            internal::ProtocolStatus::kFailedToAllocateCall);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, ListenerKeepsAcceptingAfterConnectionAllocationFailure) {
  allocator::test::AllocatorForTest<16384> backing_allocator;
  allocator::test::FaultInjectingAllocator failing_allocator(backing_allocator);
  async2::DispatcherForTest dispatcher;

  Server server(failing_allocator, dispatcher);
  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();
  dispatcher.RunUntilStalled();

  // The first connection cannot be served and is dropped.
  transport::ReliableDatagramSocket dropped_conn =
      test::MakeMockConnection(backing_allocator).first;
  failing_allocator.DisableAllocate();
  transport.ResolveAccept(std::move(dropped_conn));
  dispatcher.RunUntilStalled();
  failing_allocator.EnableAllocate();

  // With no control operation or other wake in between, the listener must
  // already have re-armed `Accept()`, so the next connection is served.
  auto [conn, raw_conn] = test::MakeMockConnection(backing_allocator);
  transport.ResolveAccept(conn);
  dispatcher.RunUntilStalled();

  Buf syn_buf = Buf::Allocate(backing_allocator,
                              internal::HandshakePacket::kWireSizeBytes);
  auto syn = internal::HandshakePacket(internal::HandshakePacket::Type::kSyn)
                 .Encode(std::move(syn_buf));
  ASSERT_TRUE(syn.ok());
  raw_conn->SetNextRead(std::move(*syn));
  dispatcher.RunUntilStalled();

  EXPECT_EQ(raw_conn->commit_count(), 1u);  // SYN-ACK

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

TEST(ServerTest, InvalidRequestPayloadRepliesError) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  Server server(allocator, dispatcher);
  TestEchoService echo_service(100u);
  EXPECT_EQ(server.RegisterService(echo_service), OkStatus());

  MockServerListener transport;
  EXPECT_EQ(server.RegisterListenerBlocking(transport), OkStatus());
  server.Start();

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  EstablishServerConnection(
      allocator, dispatcher, server, transport, raw_conn, conn);

  const size_t commits_before = raw_conn->commit_count();

  // Method 2 rejects every request payload.
  std::byte payload[1] = {std::byte{1}};
  auto pkt = internal::PacketFramer::FrameStartUnaryPacket(allocator,
                                                           /*call_id=*/13u,
                                                           /*service_id=*/100u,
                                                           /*method_id=*/2u,
                                                           payload);
  ASSERT_TRUE(pkt.ok());
  raw_conn->SetNextRead(std::move(*pkt));
  dispatcher.RunUntilStalled();

  EXPECT_EQ(echo_service.last_dispatched_method(), 2u);
  EXPECT_EQ(raw_conn->commit_count(), commits_before + 1u);
  auto decode_err =
      internal::InboundPacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(decode_err.ok());
  EXPECT_EQ(
      decode_err->type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(decode_err->call_id(), 13u);
  EXPECT_EQ(decode_err->error(),
            internal::ProtocolStatus::kInvalidRequestPayload);

  ControlTask close_task(server.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(close_task.done());
}

// Service with a bidirectional-streaming method that records the size of every
// message it reads, then drops its writer, which ends the RPC.
class StreamRecordingService : public Service {
 public:
  explicit StreamRecordingService(uint32_t service_id)
      : Service(service_id, methods_),
        methods_({
            internal::RawFutureMethodInvoker<
                RecordFuture,
                MethodType::kBidirectionalStreaming>::
                CreateMethod<StreamRecordingService>(1u),
        }) {}

  span<const size_t> message_sizes() const {
    return span(message_sizes_).first(message_count_);
  }
  bool finished() const { return finished_; }
  Status end_status() const { return end_status_; }

  // True once the method's future has been destroyed, whether it ran to
  // completion or its call was aborted and torn down.
  bool frame_destroyed() const { return frame_destroyed_; }

 private:
  class RecordFuture {
   public:
    using value_type = void;

    RecordFuture() = default;
    RecordFuture(RecordFuture&&) = default;
    RecordFuture& operator=(RecordFuture&&) = default;

    RecordFuture(StreamRecordingService& service,
                 RawReader reader,
                 RawWriter writer)
        : service_(&service),
          reader_(std::move(reader)),
          writer_(std::move(writer)),
          read_future_(reader_.Read()) {}

    ~RecordFuture() { service_->frame_destroyed_ = true; }

    bool is_pendable() const { return !completed_; }
    bool is_complete() const { return completed_; }

    async2::Poll<> Pend(async2::Context& cx) {
      while (true) {
        PW_TRY_READY_ASSIGN(Result<ConstBuf> msg, read_future_.Pend(cx));
        if (!msg.ok()) {
          service_->end_status_ = msg.status();
          service_->finished_ = true;
          completed_ = true;
          return async2::Ready();
        }
        PW_CHECK_UINT_LT(service_->message_count_,
                         service_->message_sizes_.size());
        service_->message_sizes_[service_->message_count_++] = msg->size();
        read_future_ = reader_.Read();
      }
    }

   private:
    StreamRecordingService* service_ = nullptr;
    RawReader reader_;
    RawWriter writer_;
    RawReadFuture read_future_;
    bool completed_ = false;
  };

  std::array<internal::Method, 1> methods_;
  std::array<size_t, 4> message_sizes_{};
  size_t message_count_ = 0;
  bool finished_ = false;
  bool frame_destroyed_ = false;
  Status end_status_;
};

class ServerStartPacketTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kEchoServiceId = 100u;
  static constexpr uint32_t kStreamServiceId = 200u;

  ServerStartPacketTest() {
    PW_CHECK_OK(server_.RegisterService(echo_service_));
    PW_CHECK_OK(server_.RegisterService(stream_service_));
    PW_CHECK_OK(server_.RegisterListenerBlocking(transport_));
    server_.Start();

    auto [conn, raw_conn] = test::MakeMockConnection(allocator_);
    raw_conn_ = raw_conn;
    EstablishServerConnection(
        allocator_, dispatcher_, server_, transport_, raw_conn_, conn);
  }

  ~ServerStartPacketTest() override {
    ControlTask close_task(server_.Close());
    dispatcher_.Post(close_task);
    dispatcher_.RunUntilStalled();
    EXPECT_TRUE(close_task.done());
  }

  void Receive(Result<Buf>&& packet) {
    PW_CHECK_OK(packet.status());
    raw_conn_->SetNextRead(std::move(*packet));
    dispatcher_.RunUntilStalled();
  }

  void ReceiveStart(internal::PacketType type,
                    uint32_t call_id,
                    uint32_t service_id,
                    ConstByteSpan payload = {}) {
    Receive(internal::PacketFramer::FrameStartPacket(
        allocator_, type, call_id, service_id, /*method_id=*/1u, payload));
  }

  // Decodes the most recent packet the server sent.
  internal::InboundPacket LastSent() {
    auto decoded =
        internal::InboundPacket::Decode(raw_conn_->last_written_buf());
    PW_CHECK_OK(decoded.status());
    return std::move(*decoded);
  }

  allocator::test::AllocatorForTest<8192> allocator_;
  async2::DispatcherForTest dispatcher_;
  Server server_{allocator_, dispatcher_};
  TestEchoService echo_service_{kEchoServiceId};
  StreamRecordingService stream_service_{kStreamServiceId};
  MockServerListener transport_;
  test::MockConnection* raw_conn_ = nullptr;

  static constexpr std::byte kPayload[4] = {
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
};

TEST_F(ServerStartPacketTest,
       StartStreamToUnaryMethodRepliesMethodTypeMismatch) {
  size_t commits_before = raw_conn_->commit_count();
  ReceiveStart(internal::PacketType::Make<flags::kStart>(),
               /*call_id=*/1,
               kEchoServiceId);

  EXPECT_EQ(echo_service_.last_dispatched_method(), 0u);
  ASSERT_EQ(raw_conn_->commit_count(), commits_before + 1u);
  internal::InboundPacket pkt = LastSent();
  EXPECT_EQ(
      pkt.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(pkt.call_id(), 1u);
  EXPECT_EQ(pkt.error(), internal::ProtocolStatus::kMethodTypeMismatch);
}

TEST_F(ServerStartPacketTest,
       StartStreamWithPayloadToUnaryMethodRepliesMethodTypeMismatch) {
  size_t commits_before = raw_conn_->commit_count();
  ReceiveStart(internal::PacketType::Make<flags::kStart, flags::kHasPayload>(),
               /*call_id=*/2,
               kEchoServiceId,
               kPayload);

  EXPECT_EQ(echo_service_.last_dispatched_method(), 0u);
  ASSERT_EQ(raw_conn_->commit_count(), commits_before + 1u);
  internal::InboundPacket pkt = LastSent();
  EXPECT_EQ(
      pkt.type(),
      (internal::PacketType::Make<flags::kServer, flags::kErrorTerminal>()));
  EXPECT_EQ(pkt.call_id(), 2u);
  EXPECT_EQ(pkt.error(), internal::ProtocolStatus::kMethodTypeMismatch);
}

TEST_F(ServerStartPacketTest, StreamEndWithoutPayloadToUnaryMethodIsMismatch) {
  size_t commits_before = raw_conn_->commit_count();
  ReceiveStart(internal::PacketType::Make<flags::kStart, flags::kStreamEnd>(),
               /*call_id=*/3,
               kEchoServiceId);

  EXPECT_EQ(echo_service_.last_dispatched_method(), 0u);
  ASSERT_EQ(raw_conn_->commit_count(), commits_before + 1u);
  EXPECT_EQ(LastSent().error(), internal::ProtocolStatus::kMethodTypeMismatch);
}

TEST_F(ServerStartPacketTest, StartStreamThenMessagesToStreamingMethod) {
  ReceiveStart(internal::PacketType::Make<flags::kStart>(),
               /*call_id=*/4,
               kStreamServiceId);
  EXPECT_TRUE(stream_service_.message_sizes().empty());
  EXPECT_FALSE(stream_service_.finished());

  Receive(internal::PacketFramer::FrameClientMessagePacket(
      allocator_, /*call_id=*/4, span(kPayload).first(2)));
  Receive(internal::PacketFramer::FrameClientStreamEndPacket(allocator_,
                                                             /*call_id=*/4));

  ASSERT_EQ(stream_service_.message_sizes().size(), 1u);
  EXPECT_EQ(stream_service_.message_sizes()[0], 2u);
  EXPECT_TRUE(stream_service_.finished());
  EXPECT_EQ(stream_service_.end_status(), Status::OutOfRange());

  // Dropping the writer terminates the RPC.
  internal::InboundPacket pkt = LastSent();
  EXPECT_EQ(pkt.type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));
  EXPECT_EQ(pkt.call_id(), 4u);
}

TEST_F(ServerStartPacketTest, StartStreamWithPayloadDeliversInitialMessage) {
  ReceiveStart(internal::PacketType::Make<flags::kStart, flags::kHasPayload>(),
               /*call_id=*/5,
               kStreamServiceId,
               kPayload);
  ASSERT_EQ(stream_service_.message_sizes().size(), 1u);
  EXPECT_EQ(stream_service_.message_sizes()[0], sizeof(kPayload));
  EXPECT_FALSE(stream_service_.finished());

  Receive(internal::PacketFramer::FrameClientMessagePacket(
      allocator_, /*call_id=*/5, span(kPayload).first(2)));
  Receive(internal::PacketFramer::FrameClientStreamEndPacket(allocator_,
                                                             /*call_id=*/5));

  ASSERT_EQ(stream_service_.message_sizes().size(), 2u);
  EXPECT_EQ(stream_service_.message_sizes()[1], 2u);
  EXPECT_TRUE(stream_service_.finished());
  EXPECT_EQ(stream_service_.end_status(), Status::OutOfRange());
}

TEST_F(ServerStartPacketTest, RequestToStreamingMethodDeliversMessageAndEnd) {
  Receive(internal::PacketFramer::FrameStartUnaryPacket(allocator_,
                                                        /*call_id=*/6,
                                                        kStreamServiceId,
                                                        /*method_id=*/1u,
                                                        kPayload));

  ASSERT_EQ(stream_service_.message_sizes().size(), 1u);
  EXPECT_EQ(stream_service_.message_sizes()[0], sizeof(kPayload));
  EXPECT_TRUE(stream_service_.finished());
  EXPECT_EQ(stream_service_.end_status(), Status::OutOfRange());

  internal::InboundPacket pkt = LastSent();
  EXPECT_EQ(pkt.type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));
  EXPECT_EQ(pkt.call_id(), 6u);
}

TEST_F(ServerStartPacketTest, StartWithStreamEndClosesStreamImmediately) {
  ReceiveStart(internal::PacketType::Make<flags::kStart, flags::kStreamEnd>(),
               /*call_id=*/7,
               kStreamServiceId);

  EXPECT_TRUE(stream_service_.message_sizes().empty());
  EXPECT_TRUE(stream_service_.finished());
  EXPECT_EQ(stream_service_.end_status(), Status::OutOfRange());

  internal::InboundPacket pkt = LastSent();
  EXPECT_EQ(pkt.type(),
            (internal::PacketType::Make<flags::kServer, flags::kOkTerminal>()));
  EXPECT_EQ(pkt.call_id(), 7u);
}

// A client that detects that the server's responses don't match the method's
// type reports it, which aborts the call on the server too. Like any aborted
// call, it is torn down without resuming the method, and nothing is sent back.
TEST_F(ServerStartPacketTest, ClientMethodTypeMismatchAbortsServerCall) {
  ReceiveStart(internal::PacketType::Make<flags::kStart>(),
               /*call_id=*/8,
               kStreamServiceId);
  ASSERT_FALSE(stream_service_.frame_destroyed());
  const size_t commits_before = raw_conn_->commit_count();

  Receive(internal::PacketFramer::FrameClientErrorPacket(
      allocator_,
      /*call_id=*/8,
      internal::ProtocolStatus::kMethodTypeMismatch));

  EXPECT_TRUE(stream_service_.frame_destroyed());
  EXPECT_FALSE(stream_service_.finished());
  EXPECT_EQ(raw_conn_->commit_count(), commits_before);
}

}  // namespace
}  // namespace pw::rpc2
