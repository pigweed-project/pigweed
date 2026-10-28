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

#include "pw_transport/forwarding_socket.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::transport {
namespace {

std::string_view AsString(const ConstBuf& buf) {
  return std::string_view(reinterpret_cast<const char*>(buf.data()),
                          buf.size());
}

class ForwardingDatagramSocketTest : public ::testing::Test {
 protected:
  void TearDown() override {
    EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
  }

  template <typename FutureType>
  auto RunToCompletion(FutureType& future) {
    return dispatcher_.RunInTaskUntilStalled(future).value();
  }

  bool TrySend(const ReliableSocket& socket, std::string_view data) {
    std::optional<WriteReservation> reservation =
        socket.TryReserveWrite(data.size());
    if (!reservation.has_value()) {
      return false;
    }
    if (!data.empty()) {
      std::memcpy(reservation->data(), data.data(), data.size());
    }
    return reservation->Commit(data.size());
  }

  ConstBuf Receive(const ReliableSocket& socket) {
    ReadFuture read = socket.Read();
    return RunToCompletion(read);
  }

  allocator::test::AllocatorForTest<4096> allocator_;
  async2::DispatcherForTest dispatcher_;
};

TEST_F(ForwardingDatagramSocketTest, DefaultConstructed_IsNull) {
  ForwardingDatagramSocketPair pair;
  EXPECT_FALSE(pair.first());
  EXPECT_FALSE(pair.second());
  pair.Close();
}

TEST_F(ForwardingDatagramSocketTest, SendAndReceive_BothDirections) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  ASSERT_TRUE(TrySend(pair.first(), "hello"));
  ConstBuf request = Receive(pair.second());
  ASSERT_TRUE(request != nullptr);
  EXPECT_EQ(AsString(request), "hello");

  ASSERT_TRUE(TrySend(pair.second(), "world"));
  ConstBuf response = Receive(pair.first());
  ASSERT_TRUE(response != nullptr);
  EXPECT_EQ(AsString(response), "world");
}

TEST_F(ForwardingDatagramSocketTest, SendAndReceive_ForwardsZeroCopy) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  std::optional<WriteReservation> reservation =
      pair.first().TryReserveWrite(16);
  ASSERT_TRUE(reservation.has_value());
  const std::byte* reserved_ptr = reservation->data();
  std::memcpy(reservation->data(), "zero-copy", 9);
  ASSERT_TRUE(reservation->Commit(9));

  ConstBuf received = Receive(pair.second());
  ASSERT_TRUE(received != nullptr);
  EXPECT_EQ(received.data(), reserved_ptr);
  EXPECT_EQ(AsString(received), "zero-copy");
}

TEST_F(ForwardingDatagramSocketTest, SendAndReceive_EmptyDatagram) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 2});

  ASSERT_TRUE(TrySend(pair.first(), ""));
  ASSERT_TRUE(TrySend(pair.first(), "after"));

  ConstBuf empty = Receive(pair.second());
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty.empty());
  EXPECT_FALSE(empty.owned());
  EXPECT_EQ(empty.size(), 0u);

  ConstBuf after = Receive(pair.second());
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(AsString(after), "after");
}

TEST_F(ForwardingDatagramSocketTest,
       SendAndReceive_NonEmptyReservationCommittedAsEmpty) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  std::optional<WriteReservation> reservation =
      pair.first().TryReserveWrite(16);
  ASSERT_TRUE(reservation.has_value());
  ASSERT_TRUE(reservation->Commit(0));

  ConstBuf received = Receive(pair.second());
  ASSERT_NE(received, nullptr);
  EXPECT_TRUE(received.empty());
  EXPECT_TRUE(received.owned());
  EXPECT_EQ(received.size(), 0u);
}

TEST_F(ForwardingDatagramSocketTest, SendAndReceive_ManyDatagramsInOrder) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 4});

  constexpr std::array<std::string_view, 4> kDatagrams = {
      "first", "second", "third", "fourth"};
  for (std::string_view datagram : kDatagrams) {
    ASSERT_TRUE(TrySend(pair.first(), datagram));
  }
  for (std::string_view datagram : kDatagrams) {
    ConstBuf received = Receive(pair.second());
    ASSERT_TRUE(received != nullptr);
    EXPECT_EQ(AsString(received), datagram);
  }
}

TEST_F(ForwardingDatagramSocketTest, TryReserveWrite_LimitedByQueueDepth) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 2});

  std::optional<WriteReservation> first = pair.first().TryReserveWrite(4);
  std::optional<WriteReservation> second = pair.first().TryReserveWrite(4);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(pair.first().TryReserveWrite(4).has_value());

  // Empty datagram reservations also occupy a queue slot.
  EXPECT_FALSE(pair.first().TryReserveWrite(0).has_value());

  // Canceling a reservation frees a slot immediately.
  first->Cancel();
  std::optional<WriteReservation> zero = pair.first().TryReserveWrite(0);
  ASSERT_TRUE(zero.has_value());
  EXPECT_FALSE(pair.first().TryReserveWrite(0).has_value());
  EXPECT_FALSE(pair.first().TryReserveWrite(4).has_value());
  zero->Cancel();

  first = pair.first().TryReserveWrite(4);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(pair.first().TryReserveWrite(4).has_value());

  // Committing transfers the slot to the peer's read queue until read.
  std::memcpy(first->data(), "abcd", 4);
  ASSERT_TRUE(first->Commit(4));
  EXPECT_FALSE(pair.first().TryReserveWrite(4).has_value());

  // Reading from the peer releases the slot.
  ConstBuf received = Receive(pair.second());
  ASSERT_TRUE(received != nullptr);
  EXPECT_EQ(AsString(received), "abcd");
  EXPECT_TRUE(pair.first().TryReserveWrite(4).has_value());
}

TEST_F(ForwardingDatagramSocketTest, ReserveWrite_WakesWhenPeerReads) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  ASSERT_TRUE(TrySend(pair.first(), "first"));

  async2::FutureTask<ReserveWriteFuture> reserve_task(
      pair.first().ReserveWrite(6));
  dispatcher_.Post(reserve_task);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(reserve_task.has_value());

  ConstBuf first = Receive(pair.second());
  ASSERT_TRUE(first != nullptr);
  EXPECT_EQ(AsString(first), "first");

  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(reserve_task.has_value());
  ASSERT_TRUE(reserve_task.value().has_value());
  std::memcpy(reserve_task.value()->data(), "second", 6);
  ASSERT_TRUE(reserve_task.value()->Commit(6));

  ConstBuf second = Receive(pair.second());
  ASSERT_TRUE(second != nullptr);
  EXPECT_EQ(AsString(second), "second");
}

TEST_F(ForwardingDatagramSocketTest,
       ReserveWrite_WakesWhenReservationCanceled) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 1});

  WriteReservation blocker = pair.first().TryReserveWrite(0).value();

  async2::FutureTask<ReserveWriteFuture> reserve_task(
      pair.first().ReserveWrite(4));
  dispatcher_.Post(reserve_task);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(reserve_task.has_value());

  blocker.Cancel();
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(reserve_task.has_value());
  ASSERT_TRUE(reserve_task.value().has_value());
}

TEST_F(ForwardingDatagramSocketTest,
       ReserveWrite_MoveAssignOverResolvedFutureReleasesReservation) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  WriteReservation blocker = pair.first().TryReserveWrite(0).value();
  ReserveWriteFuture resolved = pair.first().ReserveWrite(4);
  EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(resolved).IsPending());

  // Canceling resolves `resolved` with a reservation that is never taken.
  blocker.Cancel();

  // Both sockets share a lock. Assigning over `resolved` must cancel its
  // reservation without holding that lock.
  resolved = pair.second().ReserveWrite(4);

  // The canceled reservation's slot is available again.
  EXPECT_TRUE(pair.first().TryReserveWrite(4).has_value());
  std::optional<WriteReservation> reservation = RunToCompletion(resolved);
  EXPECT_TRUE(reservation.has_value());
}

TEST_F(ForwardingDatagramSocketTest, Read_WakesWhenPeerCommits) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  async2::FutureTask<ReadFuture> read_task(pair.second().Read());
  dispatcher_.Post(read_task);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(read_task.has_value());

  ASSERT_TRUE(TrySend(pair.first(), "wake up"));
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(read_task.has_value());
  ASSERT_TRUE(read_task.value() != nullptr);
  EXPECT_EQ(AsString(read_task.value()), "wake up");
}

TEST_F(ForwardingDatagramSocketTest, DefaultOptions_MaxSizes) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  constexpr size_t kDefault =
      ForwardingDatagramSocketOptions::kDefaultMaxDatagramSizeBytes;
  EXPECT_EQ(pair.first().max_write_message_size_bytes(), kDefault);
  EXPECT_EQ(pair.first().max_read_message_size_bytes(), kDefault);
  EXPECT_EQ(pair.second().max_write_message_size_bytes(), kDefault);
  EXPECT_EQ(pair.second().max_read_message_size_bytes(), kDefault);
}

TEST_F(ForwardingDatagramSocketTest, Options_MaxSizesPerDirection) {
  auto pair = ForwardingDatagramSocketPair::Create(
      allocator_,
      {.max_first_to_second_size_bytes = 64,
       .max_second_to_first_size_bytes = 32});

  EXPECT_EQ(pair.first().max_write_message_size_bytes(), 64u);
  EXPECT_EQ(pair.second().max_read_message_size_bytes(), 64u);
  EXPECT_EQ(pair.second().max_write_message_size_bytes(), 32u);
  EXPECT_EQ(pair.first().max_read_message_size_bytes(), 32u);

  EXPECT_TRUE(pair.first().TryReserveWrite(64).has_value());
  EXPECT_TRUE(pair.second().TryReserveWrite(32).has_value());
}

TEST_F(ForwardingDatagramSocketTest,
       TryReserveWrite_ReturnsNulloptWhenAllocatorExhausted) {
  auto pair = ForwardingDatagramSocketPair::Create(
      allocator_, {.max_first_to_second_size_bytes = 8192});

  EXPECT_FALSE(pair.first().TryReserveWrite(8192).has_value());
  EXPECT_TRUE(pair.first().TryReserveWrite(0).has_value());
  EXPECT_TRUE(pair.first().TryReserveWrite(8).has_value());
}

TEST_F(ForwardingDatagramSocketTest,
       ReserveWrite_RetriedWhenReservationCanceledAfterAllocationFailure) {
  // Each buffer is too large for two to fit in `allocator_` at once.
  constexpr size_t kLargeSize = 2500;
  auto pair = ForwardingDatagramSocketPair::Create(
      allocator_,
      {.queue_depth = 2, .max_first_to_second_size_bytes = kLargeSize});

  std::optional<WriteReservation> blocker =
      pair.first().TryReserveWrite(kLargeSize);
  ASSERT_TRUE(blocker.has_value());

  // There is queue capacity, but no memory, so the reservation waits.
  async2::FutureTask<ReserveWriteFuture> reserve_task(
      pair.first().ReserveWrite(kLargeSize));
  dispatcher_.Post(reserve_task);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(reserve_task.has_value());

  // Canceling frees the blocker's buffer before the pending reservation is
  // retried, so the retry succeeds.
  blocker->Cancel();
  dispatcher_.RunUntilStalled();
  ASSERT_TRUE(reserve_task.has_value());
  ASSERT_TRUE(reserve_task.value().has_value());
  EXPECT_EQ(reserve_task.value()->size(), kLargeSize);
}

TEST_F(ForwardingDatagramSocketTest, TryCreate_FailsWhenAllocatorExhausted) {
  allocator::NullAllocator null_alloc;
  Result<ForwardingDatagramSocketPair> result =
      ForwardingDatagramSocketPair::TryCreate(null_alloc);
  EXPECT_EQ(result.status(), Status::ResourceExhausted());

  // Also test when the allocator does not have enough space for the queues.
  Result<ForwardingDatagramSocketPair> queue_fail =
      ForwardingDatagramSocketPair::TryCreate(allocator_,
                                              {.queue_depth = 1000});
  EXPECT_EQ(queue_fail.status(), Status::ResourceExhausted());
  EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(ForwardingDatagramSocketTest, Close_WakesAndClosesPeerWhenQueueEmpty) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 1});

  WriteReservation blocker = pair.second().TryReserveWrite(0).value();

  async2::FutureTask<ReadFuture> read_task(pair.second().Read());
  async2::FutureTask<ReserveWriteFuture> write_task(
      pair.second().ReserveWrite(4));
  async2::FutureTask<CloseFuture> closed_task(pair.second().WhenClosed());
  dispatcher_.Post(read_task);
  dispatcher_.Post(write_task);
  dispatcher_.Post(closed_task);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(read_task.has_value());
  EXPECT_FALSE(write_task.has_value());
  EXPECT_FALSE(closed_task.is_complete());

  CloseFuture close_first = pair.first().Close();
  RunToCompletion(close_first);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(read_task.has_value());
  EXPECT_TRUE(read_task.value() == nullptr);
  ASSERT_TRUE(write_task.has_value());
  EXPECT_FALSE(write_task.value().has_value());
  EXPECT_TRUE(closed_task.is_complete());
  EXPECT_FALSE(pair.second().TryReserveWrite(4).has_value());
}

TEST_F(ForwardingDatagramSocketTest, Close_CommittedDatagramsCanStillBeRead) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 3});

  constexpr std::array<std::string_view, 3> kDatagrams = {
      "one", "two", "three"};
  for (std::string_view datagram : kDatagrams) {
    ASSERT_TRUE(TrySend(pair.first(), datagram));
  }

  CloseFuture close = pair.first().Close();
  RunToCompletion(close);

  async2::FutureTask<CloseFuture> second_closed(pair.second().WhenClosed());
  dispatcher_.Post(second_closed);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(second_closed.is_complete());

  // Writes from the peer fail once the first socket has closed.
  EXPECT_FALSE(TrySend(pair.second(), "reply"));

  for (std::string_view datagram : kDatagrams) {
    ConstBuf received = Receive(pair.second());
    ASSERT_TRUE(received != nullptr);
    EXPECT_EQ(AsString(received), datagram);
  }

  dispatcher_.RunUntilStalled();
  EXPECT_TRUE(second_closed.is_complete());
  EXPECT_TRUE(Receive(pair.second()) == nullptr);
}

TEST_F(ForwardingDatagramSocketTest,
       Close_WakesPeerWriterWhilePeerHasUnreadDatagrams) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  // Enqueue a datagram for `second()` so its read queue is non-empty.
  ASSERT_TRUE(TrySend(pair.first(), "unread"));

  // Fill `second()`'s write capacity and pend a `ReserveWriteFuture` on it.
  ASSERT_TRUE(TrySend(pair.second(), "in-flight"));
  async2::FutureTask<ReserveWriteFuture> pending_write(
      pair.second().ReserveWrite(4));
  dispatcher_.Post(pending_write);
  dispatcher_.RunUntilStalled();
  EXPECT_FALSE(pending_write.has_value());

  // Closing `first()` discards "in-flight" and wakes `pending_write` even
  // though `second()` still has "unread" in its read queue.
  CloseFuture close = pair.first().Close();
  RunToCompletion(close);
  dispatcher_.RunUntilStalled();

  ASSERT_TRUE(pending_write.has_value());
  ASSERT_TRUE(pending_write.value().has_value());
  EXPECT_FALSE(pending_write.value()->Commit(4));

  ConstBuf unread = Receive(pair.second());
  ASSERT_TRUE(unread != nullptr);
  EXPECT_EQ(AsString(unread), "unread");
  EXPECT_TRUE(Receive(pair.second()) == nullptr);
}

TEST_F(ForwardingDatagramSocketTest,
       Close_PeerReservationsStillLimitedByQueueDepth) {
  auto pair =
      ForwardingDatagramSocketPair::Create(allocator_, {.queue_depth = 2});

  // Keep `second()` open after `first()` closes by leaving a datagram unread.
  ASSERT_TRUE(TrySend(pair.first(), "unread"));
  CloseFuture close = pair.first().Close();
  RunToCompletion(close);

  std::optional<WriteReservation> first = pair.second().TryReserveWrite(4);
  std::optional<WriteReservation> second = pair.second().TryReserveWrite(4);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(pair.second().TryReserveWrite(4).has_value());

  // A failed commit releases its slot.
  EXPECT_FALSE(first->Commit(4));
  EXPECT_TRUE(pair.second().TryReserveWrite(4).has_value());
}

TEST_F(ForwardingDatagramSocketTest, Close_CommitAfterCloseFails) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  std::optional<WriteReservation> reservation = pair.first().TryReserveWrite(4);
  ASSERT_TRUE(reservation.has_value());

  CloseFuture close = pair.first().Close();
  EXPECT_FALSE(reservation->Commit(4));
  RunToCompletion(close);

  EXPECT_TRUE(Receive(pair.second()) == nullptr);
}

TEST_F(ForwardingDatagramSocketTest, Close_DiscardsUnreadDatagrams) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  ASSERT_TRUE(TrySend(pair.first(), "unread"));
  ReadFuture read = pair.second().Read();

  CloseFuture close = pair.second().Close();
  RunToCompletion(close);
  EXPECT_TRUE(RunToCompletion(read) == nullptr);
}

TEST_F(ForwardingDatagramSocketTest, PairClose_ClosesBothImmediately) {
  auto pair = ForwardingDatagramSocketPair::Create(allocator_);

  ASSERT_TRUE(TrySend(pair.first(), "to-second"));
  ASSERT_TRUE(TrySend(pair.second(), "to-first"));

  pair.Close();

  EXPECT_TRUE(Receive(pair.first()) == nullptr);
  EXPECT_TRUE(Receive(pair.second()) == nullptr);
}

TEST_F(ForwardingDatagramSocketTest, Sockets_OutlivePair) {
  ReliableSocket first;
  ReliableSocket second;
  {
    auto pair = ForwardingDatagramSocketPair::Create(allocator_);
    first = pair.first();
    second = pair.second();
  }

  ASSERT_TRUE(TrySend(first, "still open"));
  ConstBuf received = Receive(second);
  ASSERT_TRUE(received != nullptr);
  EXPECT_EQ(AsString(received), "still open");
}

TEST_F(ForwardingDatagramSocketTest,
       DropLastHandle_ClosesPeerAfterDrainingCommittedDatagrams) {
  ReliableSocket first;
  ReliableSocket second;
  {
    auto pair = ForwardingDatagramSocketPair::Create(allocator_);
    first = pair.first();
    second = pair.second();
  }

  ASSERT_TRUE(TrySend(first, "final message"));
  first = nullptr;

  ConstBuf received = Receive(second);
  ASSERT_TRUE(received != nullptr);
  EXPECT_EQ(AsString(received), "final message");

  EXPECT_TRUE(Receive(second) == nullptr);
  CloseFuture closed = second.WhenClosed();
  RunToCompletion(closed);
}

}  // namespace
}  // namespace pw::transport
