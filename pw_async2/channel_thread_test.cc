// Copyright 2025 The Pigweed Authors
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

#include <atomic>

#include "pw_async2/channel.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/try.h"
#include "pw_containers/vector.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::ChannelStorage;
using pw::async2::Context;
using pw::async2::CreateSpscChannel;
using pw::async2::Dispatcher;
using pw::async2::DispatcherForTest;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::ReceiveFuture;
using pw::async2::Receiver;
using pw::async2::Sender;
using pw::async2::SendFuture;
using pw::async2::Task;

using namespace std::chrono_literals;

template <typename T>
class BasicSenderTask : public Task {
 public:
  BasicSenderTask(Sender<T> sender, int start, int end)
      : Task(PW_ASYNC_TASK_NAME("SenderTask")),
        sender_(std::move(sender)),
        next_(start),
        end_(end) {}

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ <= end_) {
      if (!future_.is_pendable()) {
        future_ = sender_.Send(T(next_));
      }

      PW_TRY_READY_ASSIGN(bool sent, future_.Pend(cx));
      if (!sent) {
        return Ready();
      }

      next_++;
    }

    sender_.Disconnect();
    return Ready();
  }

  Sender<T> sender_;
  SendFuture<T> future_;
  int next_;
  int end_;
};

using SenderTask = BasicSenderTask<int>;

class ReceiverTask : public Task {
 public:
  ReceiverTask(Receiver<int> receiver,
               size_t disconnect_after = std::numeric_limits<size_t>::max())
      : Task(PW_ASYNC_TASK_NAME("ReceiverTask")),
        receiver_(std::move(receiver)),
        disconnect_after_(disconnect_after) {}

  const pw::Vector<int>& received() const { return received_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (disconnect_after_ > 0) {
      if (!future_.is_pendable()) {
        future_ = receiver_.Receive();
      }

      PW_TRY_READY_ASSIGN(std::optional<int> value, future_.Pend(cx));
      if (!value.has_value()) {
        break;
      }
      received_.push_back(*value);
      disconnect_after_--;
    }

    receiver_.Disconnect();
    return Ready();
  }

  Receiver<int> receiver_;
  ReceiveFuture<int> future_;
  pw::Vector<int, 10> received_;
  size_t disconnect_after_;
};

struct MoveOnly {
  int n = 0;
  constexpr MoveOnly() = default;
  constexpr explicit MoveOnly(int n_) : n(n_) {}
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
};

TEST(Channel, BlockingSend) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  struct {
    DispatcherForTest& dispatcher;
    Sender<int>& sender;
    size_t send_count;
  } sender_context{dispatcher, sender, 0};

  pw::thread::test::TestThreadContext context;
  pw::Thread sender_thread(context.options(), [&sender_context]() {
    pw::Status last_status;
    for (int i = 0; i < 10; ++i) {
      last_status =
          sender_context.sender.BlockingSend(sender_context.dispatcher, i);
      if (last_status.ok()) {
        ++sender_context.send_count;
      } else {
        break;
      }
    }

    sender_context.sender.Disconnect();
    sender_context.dispatcher.Release();
    EXPECT_EQ(last_status, pw::OkStatus());
  });

  ReceiverTask receiver_task(std::move(receiver));
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletionUntilReleased();
  sender_thread.join();

  EXPECT_EQ(sender_context.send_count, 10u);
  ASSERT_EQ(receiver_task.received().size(), 10u);
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(receiver_task.received()[i], static_cast<int>(i));
  }
}

TEST(Channel, BlockingSend_ChannelCloses) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  struct {
    DispatcherForTest& dispatcher;
    Sender<int>& sender;
    size_t send_count;
  } sender_context{dispatcher, sender, 0};

  // The sender will write values up until the receiver disconnects and the
  // channel closes, causing `BlockingSend` to return false.
  pw::thread::test::TestThreadContext context;
  pw::Thread sender_thread(context.options(), [&sender_context]() {
    pw::Status last_status;
    for (int i = 0; i < 10; ++i) {
      last_status =
          sender_context.sender.BlockingSend(sender_context.dispatcher, i);
      if (last_status.ok()) {
        ++sender_context.send_count;
      } else {
        break;
      }
    }

    EXPECT_EQ(last_status, pw::Status::FailedPrecondition());
    EXPECT_FALSE(sender_context.sender.is_open());
    sender_context.dispatcher.Release();
  });

  constexpr size_t kDisconnectAfter = 3;

  ReceiverTask receiver_task(std::move(receiver), kDisconnectAfter);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletionUntilReleased();
  sender_thread.join();

  // Depending on timing, sender_thread could fill back up the channel before
  // the receiver_task has a chance to disconnect.
  EXPECT_GE(sender_context.send_count, kDisconnectAfter);
  EXPECT_LE(sender_context.send_count, kDisconnectAfter + storage.capacity());

  ASSERT_EQ(receiver_task.received().size(), kDisconnectAfter);
  for (size_t i = 0; i < kDisconnectAfter; ++i) {
    EXPECT_EQ(receiver_task.received()[i], static_cast<int>(i));
  }
}

TEST(Channel, BlockingSend_Timeout) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  struct {
    DispatcherForTest& dispatcher;
    Sender<int>& sender;
    size_t send_count;
  } sender_context{dispatcher, sender, 0};

  // No task is receiving, so the sender should write values up to the channel
  // capacity and then block, timing out and failing. The channel should remain
  // open the entire time.
  pw::thread::test::TestThreadContext context;
  pw::Thread sender_thread(context.options(), [&sender_context]() {
    pw::Status last_status;
    for (int i = 0; i < 10; ++i) {
      last_status = sender_context.sender.BlockingSend(
          sender_context.dispatcher, i, 200ms);
      if (last_status.ok()) {
        ++sender_context.send_count;
      } else {
        break;
      }
    }

    EXPECT_EQ(last_status, pw::Status::DeadlineExceeded());
    EXPECT_TRUE(sender_context.sender.is_open());
    sender_context.dispatcher.Release();
  });

  dispatcher.RunToCompletionUntilReleased();
  sender_thread.join();

  EXPECT_EQ(sender_context.send_count, 2u);
}

TEST(Channel, BlockingSend_ReturnsImmediatelyIfSpaceAvailable) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  // We never actually run the dispatcher, so the only way the sender
  // can write a value is if there is space in the channel.

  pw::Status sent =
      sender.BlockingSend(dispatcher, 0, pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(sent, pw::OkStatus());

  sent =
      sender.BlockingSend(dispatcher, 1, pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(sent, pw::OkStatus());

  sent =
      sender.BlockingSend(dispatcher, 2, pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(sent, pw::Status::DeadlineExceeded());
}

TEST(Channel, BlockingSend_MoveOnly_ReturnsImmediatelyIfSpaceAvailable) {
  DispatcherForTest dispatcher;

  ChannelStorage<MoveOnly, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  // We never actually run the dispatcher, so the only way the sender
  // can write a value is if there is space in the channel.

  pw::Status sent = sender.BlockingSend(
      dispatcher, MoveOnly(), pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(sent, pw::OkStatus());

  sent = sender.BlockingSend(
      dispatcher, MoveOnly(), pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(sent, pw::OkStatus());

  sent = sender.BlockingSend(
      dispatcher, MoveOnly(), pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(sent, pw::Status::DeadlineExceeded());
}

TEST(Channel, BlockingReceive) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  struct {
    DispatcherForTest& dispatcher;
    Receiver<int>& receiver;
    size_t receive_count;
  } receiver_context{dispatcher, receiver, 0};

  // The receive thread should read values until the sender disconnects and the
  // channel closes.
  pw::thread::test::TestThreadContext context;
  pw::Thread receiver_thread(context.options(), [&receiver_context]() {
    pw::Status last_status;
    int expected = 0;

    while (true) {
      pw::Result<int> received = receiver_context.receiver.BlockingReceive(
          receiver_context.dispatcher);
      last_status = received.status();
      if (received.ok()) {
        EXPECT_EQ(*received, expected);
        ++receiver_context.receive_count;
        ++expected;
      } else {
        break;
      }
    }

    EXPECT_EQ(last_status, pw::Status::FailedPrecondition());
    EXPECT_FALSE(receiver_context.receiver.is_open());
    receiver_context.dispatcher.Release();
  });

  SenderTask sender_task(std::move(sender), 0, 9);
  dispatcher.Post(sender_task);

  dispatcher.RunToCompletionUntilReleased();
  receiver_thread.join();

  EXPECT_EQ(receiver_context.receive_count, 10u);
}

TEST(Channel, BlockingReceive_MoveOnly) {
  DispatcherForTest dispatcher;

  ChannelStorage<MoveOnly, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  struct {
    DispatcherForTest& dispatcher;
    Receiver<MoveOnly>& receiver;
  } context{dispatcher, receiver};

  pw::thread::test::TestThreadContext thread_context;
  pw::Thread receiver_thread(thread_context.options(), [&context]() {
    for (int i = 1; i <= 4; ++i) {
      auto received = context.receiver.BlockingReceive(context.dispatcher);
      PW_TEST_ASSERT_OK(received.status());
      EXPECT_EQ(received->n, i);
    }
    context.dispatcher.Release();
  });

  BasicSenderTask<MoveOnly> sender_task(std::move(sender), 1, 4);
  dispatcher.Post(sender_task);

  dispatcher.RunToCompletionUntilReleased();
  receiver_thread.join();
}

TEST(Channel, BlockingReceive_Timeout) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  struct {
    DispatcherForTest& dispatcher;
    Receiver<int>& receiver;
    size_t receive_count;
  } receiver_context{dispatcher, receiver, 0};

  // Push some values into the channel upfront.
  PW_TEST_EXPECT_OK(sender.TrySend(0));
  PW_TEST_EXPECT_OK(sender.TrySend(1));

  // There is no sender actively writing values, so the receive thread should
  // first drain the two existing values, then timeout trying to read more.
  // The channel should remain open the entire time.
  pw::thread::test::TestThreadContext context;
  pw::Thread receiver_thread(context.options(), [&receiver_context]() {
    pw::Status last_status;
    for (int i = 0; i < 10; ++i) {
      pw::Result<int> received = receiver_context.receiver.BlockingReceive(
          receiver_context.dispatcher, 200ms);
      last_status = received.status();
      if (received.ok()) {
        EXPECT_EQ(*received, i);
        ++receiver_context.receive_count;
      } else {
        break;
      }
    }

    EXPECT_EQ(last_status, pw::Status::DeadlineExceeded());
    EXPECT_TRUE(receiver_context.receiver.is_open());
    receiver_context.dispatcher.Release();
  });

  dispatcher.RunToCompletionUntilReleased();
  receiver_thread.join();

  EXPECT_EQ(receiver_context.receive_count, 2u);
}

TEST(Channel, BlockingReceive_ReturnsExistingValueImmediately) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  PW_TEST_ASSERT_OK(sender.TrySend(0));
  PW_TEST_ASSERT_OK(sender.TrySend(1));

  // We never actually run the dispatcher, so the only way the receiver
  // can read a value is if it's already in the channel. This approach is NOT
  // recommended in general; use TryReceive() for non-blocking operations.

  pw::Result<int> received = receiver.BlockingReceive(
      dispatcher, pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(received.status(), pw::OkStatus());
  EXPECT_EQ(*received, 0);

  received = receiver.BlockingReceive(dispatcher,
                                      pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(received.status(), pw::OkStatus());
  EXPECT_EQ(*received, 1);

  received = receiver.BlockingReceive(dispatcher,
                                      pw::chrono::SystemClock::duration(0));
  ASSERT_EQ(received.status(), pw::Status::DeadlineExceeded());
}

TEST(Channel, BlockingSend_AlreadyClosed) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  sender.Disconnect();

  struct {
    Sender<int>& sender;
    Dispatcher& dispatcher;
  } ctx{sender, dispatcher};

  pw::thread::test::TestThreadContext context;
  pw::Thread sender_thread(context.options(), [&ctx]() {
    EXPECT_EQ(ctx.sender.BlockingSend(ctx.dispatcher, 1),
              pw::Status::FailedPrecondition());
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  sender_thread.join();
}

TEST(Channel, BlockingReceive_AlreadyClosed) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  receiver.Disconnect();

  struct {
    Receiver<int>& receiver;
    Dispatcher& dispatcher;
  } ctx{receiver, dispatcher};

  pw::thread::test::TestThreadContext context;
  pw::Thread receiver_thread(context.options(), [&ctx]() {
    EXPECT_EQ(ctx.receiver.BlockingReceive(ctx.dispatcher).status(),
              pw::Status::FailedPrecondition());
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  receiver_thread.join();
}

TEST(Channel, BlockingReceive_ClosedWithData) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  PW_TEST_ASSERT_OK(sender.TrySend(1));
  sender.Disconnect();

  struct {
    Receiver<int>& receiver;
    Dispatcher& dispatcher;
  } ctx{receiver, dispatcher};

  pw::thread::test::TestThreadContext context;
  pw::Thread receiver_thread(context.options(), [&ctx]() {
    // Channel is closed, but receiver should still be able to read the data
    EXPECT_FALSE(ctx.receiver.is_open());
    auto result = ctx.receiver.BlockingReceive(ctx.dispatcher);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, 1);

    // Now the channel is empty and closed.
    EXPECT_FALSE(ctx.receiver.is_open());
    EXPECT_EQ(ctx.receiver.BlockingReceive(ctx.dispatcher).status(),
              pw::Status::FailedPrecondition());
  });

  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  receiver_thread.join();
}

}  // namespace
