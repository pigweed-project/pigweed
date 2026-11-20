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

#include "pw_transport/testing.h"

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_transport/forwarding_socket.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::transport::test {
namespace {

template <typename Base>
class StubSocket final : public Base {
 public:
  using Handle = std::conditional_t<std::is_same_v<Base, ReliableSocketImpl>,
                                    ReliableSocket,
                                    Socket>;

  static Handle Allocated(Allocator& allocator) {
    auto* raw = allocator.New<StubSocket>(allocator);
    return raw != nullptr ? raw->WrapSocket() : Handle();
  }

  explicit StubSocket(Allocator& allocator) : Base(allocator, 128) {}

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

 private:
  using Base::WrapSocket;

  async2::Poll<ConstBuf> DoRead() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return async2::Pending();
  }
  std::optional<WriteReservation> DoTryReserveWrite(size_t) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return std::nullopt;
  }
  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    this->MarkClosed();
  }
  bool DoCommitWrite(Buf&&) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return false;
  }
  void DoCancelWrite(Buf&&) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {}

  mutable sync::InterruptSpinLock lock_;
};

class TransportTestingTest : public ::testing::Test {
 protected:
  void TearDown() override {
    EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
  }

  template <typename FutureType>
  auto RunToCompletion(FutureType& future) {
    return dispatcher_.RunInTaskUntilStalled(future).value();
  }

  allocator::test::AllocatorForTest<4096> allocator_;
  async2::DispatcherForTest dispatcher_;
};

TEST_F(TransportTestingTest, MockReliableListener_AcceptThenResolve) {
  MockReliableListener listener;
  EXPECT_EQ(listener.accept_count(), 0u);
  EXPECT_FALSE(listener.has_pending_accept());

  ReliableListener::AcceptFuture accept = listener.Accept();
  EXPECT_EQ(listener.accept_count(), 1u);
  EXPECT_TRUE(listener.has_pending_accept());
  EXPECT_EQ(listener.pending_accept_count(), 1u);
  EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(accept).IsPending());

  auto pair = ForwardingSocketPair::Create(allocator_);
  listener.ResolveAccept(pair.second());
  EXPECT_FALSE(listener.has_pending_accept());

  PW_TEST_ASSERT_OK_AND_ASSIGN(ReliableSocket accepted,
                               RunToCompletion(accept));
  EXPECT_TRUE(accepted == pair.second());
}

TEST_F(TransportTestingTest,
       MockReliableListener_ResolveBeforeAcceptQueuesFifo) {
  MockReliableListener listener;
  auto pair1 = ForwardingSocketPair::Create(allocator_);
  auto pair2 = ForwardingSocketPair::Create(allocator_);

  listener.ResolveAccept(pair1.second());
  listener.ResolveAccept(Status::ResourceExhausted());
  listener.ResolveAccept(pair2.second());
  EXPECT_FALSE(listener.has_pending_accept());

  ReliableListener::AcceptFuture accept1 = listener.Accept();
  PW_TEST_ASSERT_OK_AND_ASSIGN(ReliableSocket first, RunToCompletion(accept1));
  EXPECT_TRUE(first == pair1.second());

  ReliableListener::AcceptFuture accept2 = listener.Accept();
  EXPECT_EQ(RunToCompletion(accept2).status(), Status::ResourceExhausted());

  ReliableListener::AcceptFuture accept3 = listener.Accept();
  PW_TEST_ASSERT_OK_AND_ASSIGN(ReliableSocket third, RunToCompletion(accept3));
  EXPECT_TRUE(third == pair2.second());
  EXPECT_EQ(listener.accept_count(), 3u);
}

TEST_F(TransportTestingTest, BasicMockListener_CustomMaxQueuedResults) {
  BasicMockListener<ReliableListener, 6> listener;
  static_assert(decltype(listener)::kMaxQueuedResults == 6);

  for (size_t i = 0; i < 6; ++i) {
    listener.ResolveAccept(Status::Unavailable());
  }
  for (size_t i = 0; i < 6; ++i) {
    ReliableListener::AcceptFuture accept = listener.Accept();
    EXPECT_EQ(RunToCompletion(accept).status(), Status::Unavailable());
  }
}

TEST_F(TransportTestingTest,
       MockReliableListener_MultipleConcurrentAcceptsFifo) {
  MockReliableListener listener;
  ReliableListener::AcceptFuture accept1 = listener.Accept();
  ReliableListener::AcceptFuture accept2 = listener.Accept();
  EXPECT_EQ(listener.pending_accept_count(), 2u);

  auto pair1 = ForwardingSocketPair::Create(allocator_);
  auto pair2 = ForwardingSocketPair::Create(allocator_);

  listener.ResolveAccept(pair1.second());
  EXPECT_EQ(listener.pending_accept_count(), 1u);
  PW_TEST_ASSERT_OK_AND_ASSIGN(ReliableSocket first, RunToCompletion(accept1));
  EXPECT_TRUE(first == pair1.second());
  EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(accept2).IsPending());

  listener.ResolveAccept(pair2.second());
  EXPECT_EQ(listener.pending_accept_count(), 0u);
  PW_TEST_ASSERT_OK_AND_ASSIGN(ReliableSocket second, RunToCompletion(accept2));
  EXPECT_TRUE(second == pair2.second());
}

TEST_F(TransportTestingTest,
       MockReliableListener_DestructionResolvesPendingWithFailedPrecondition) {
  ReliableListener::AcceptFuture accept1;
  ReliableListener::AcceptFuture accept2;
  {
    MockReliableListener listener;
    accept1 = listener.Accept();
    accept2 = listener.Accept();
    EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(accept1).IsPending());
    EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(accept2).IsPending());
  }
  EXPECT_EQ(RunToCompletion(accept1).status(), Status::FailedPrecondition());
  EXPECT_EQ(RunToCompletion(accept2).status(), Status::FailedPrecondition());
}

TEST_F(TransportTestingTest,
       MockReliableListener_DestructionReleasesQueuedSockets) {
  {
    MockReliableListener listener;
    ReliableSocket socket =
        StubSocket<ReliableSocketImpl>::Allocated(allocator_);
    ASSERT_TRUE(socket);
    listener.ResolveAccept(std::move(socket));
  }
  EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(TransportTestingTest, MockReliableConnector_ConnectAndResolve) {
  MockReliableConnector connector;
  EXPECT_EQ(connector.connect_count(), 0u);
  EXPECT_FALSE(connector.has_pending_connect());

  // Queued before Connect().
  connector.ResolveConnect(Status::Unavailable());
  ReliableConnector::ConnectFuture connect1 = connector.Connect();
  EXPECT_EQ(RunToCompletion(connect1).status(), Status::Unavailable());

  // Pending Connect() resolved afterwards.
  ReliableConnector::ConnectFuture connect2 = connector.Connect();
  EXPECT_TRUE(connector.has_pending_connect());
  EXPECT_EQ(connector.pending_connect_count(), 1u);

  auto pair = ForwardingSocketPair::Create(allocator_);
  connector.ResolveConnect(pair.first());
  EXPECT_FALSE(connector.has_pending_connect());
  PW_TEST_ASSERT_OK_AND_ASSIGN(ReliableSocket connected,
                               RunToCompletion(connect2));
  EXPECT_TRUE(connected == pair.first());
  EXPECT_EQ(connector.connect_count(), 2u);
}

TEST_F(TransportTestingTest,
       MockReliableConnector_DestructionResolvesPendingWithFailedPrecondition) {
  ReliableConnector::ConnectFuture connect;
  {
    MockReliableConnector connector;
    connect = connector.Connect();
    EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(connect).IsPending());
  }
  EXPECT_EQ(RunToCompletion(connect).status(), Status::FailedPrecondition());
}

TEST_F(TransportTestingTest, MockListenerAndConnector_UnreliableAcceptConnect) {
  MockListener listener;
  MockConnector connector;
  Socket socket1 = StubSocket<SocketImpl>::Allocated(allocator_);
  Socket socket2 = StubSocket<SocketImpl>::Allocated(allocator_);
  ASSERT_TRUE(socket1);
  ASSERT_TRUE(socket2);

  Listener::AcceptFuture accept = listener.Accept();
  Connector::ConnectFuture connect = connector.Connect();
  EXPECT_TRUE(listener.has_pending_accept());
  EXPECT_TRUE(connector.has_pending_connect());

  listener.ResolveAccept(socket1);
  connector.ResolveConnect(socket2);

  PW_TEST_ASSERT_OK_AND_ASSIGN(Socket accepted, RunToCompletion(accept));
  PW_TEST_ASSERT_OK_AND_ASSIGN(Socket connected, RunToCompletion(connect));
  EXPECT_TRUE(accepted == socket1);
  EXPECT_TRUE(connected == socket2);
  EXPECT_EQ(listener.accept_count(), 1u);
  EXPECT_EQ(connector.connect_count(), 1u);
}

}  // namespace
}  // namespace pw::transport::test
