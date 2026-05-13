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

#include "pw_async2/await.h"
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/internal/channel_test_util.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::ChannelStorage;
using pw::async2::CreateSpscChannel;
using pw::async2::Dispatcher;
using pw::async2::DispatcherForTest;
using pw::async2::Receiver;
using pw::async2::Sender;

using IntFunction = pw::InlineFunction<int()>;
using MoveOnlyInt = pw::async2::test::MoveOnlyInt;

using namespace std::chrono_literals;

constexpr pw::Status AsStatus(pw::Status status) { return status; }
template <typename T>
constexpr pw::Status AsStatus(const pw::Result<T>& result) {
  return result.status();
}

template <typename T>
struct ChannelAdapter {
  using ReceiverTask = pw::async2::test::ReceiverTask<T>;
  using ReservedSenderTask = pw::async2::test::ReservedSenderTask<T, int>;
  using SenderTask = pw::async2::test::SenderTask<T, int>;

  pw::Status BlockingSend(Sender<T>& sender,
                          Dispatcher& dispatcher,
                          int value) {
    return sender.BlockingSend(dispatcher, T(value));
  }

  pw::Status BlockingSend(Sender<T>& sender,
                          Dispatcher& dispatcher,
                          int value,
                          pw::chrono::SystemClock::duration timeout) {
    return sender.BlockingSend(dispatcher, T(value), timeout);
  }

  pw::Result<T> BlockingReceive(Receiver<T>& receiver, Dispatcher& dispatcher) {
    return receiver.BlockingReceive(dispatcher);
  }

  pw::Result<T> BlockingReceive(Receiver<T>& receiver,
                                Dispatcher& dispatcher,
                                pw::chrono::SystemClock::duration timeout) {
    return receiver.BlockingReceive(dispatcher, timeout);
  }

  void TrySend(Sender<T>& sender, int value) {
    PW_TEST_ASSERT_OK(sender.TrySend(T(value)));
  }

  void ExpectBlockingReceiveResult(const pw::Result<T>& result, int expected) {
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, expected);
  }

  pw::Vector<int, 10> GetReceivedBy(
      const pw::async2::test::ReceiverTask<T>& receiver_task) {
    pw::Vector<int, 10> received;
    for (const auto& v : receiver_task.received()) {
      received.push_back(v);
    }
    return received;
  }

  void ValidateReceived(std::initializer_list<int> expected,
                        const ReceiverTask& receiver_task) {
    auto received = GetReceivedBy(receiver_task);
    ASSERT_EQ(received.size(), expected.size());
    size_t i = 0;
    for (auto value : expected) {
      EXPECT_EQ(received[i++], value);
    }
  }

  ChannelStorage<T, 2> storage;
};

template <>
struct ChannelAdapter<IntFunction> {
  using ReceiverTask = pw::async2::test::ReceiverTask<IntFunction>;
  using ReservedSenderTask =
      pw::async2::test::ReservedSenderTask<IntFunction, int>;
  using SenderTask = pw::async2::test::SenderTask<IntFunction, int>;

  pw::Status BlockingSend(Sender<IntFunction>& sender,
                          Dispatcher& dispatcher,
                          int value) {
    return sender.BlockingSend(dispatcher, [value]() { return value; });
  }

  pw::Status BlockingSend(Sender<IntFunction>& sender,
                          Dispatcher& dispatcher,
                          int value,
                          pw::chrono::SystemClock::duration timeout) {
    return sender.BlockingSend(
        dispatcher, [value]() { return value; }, timeout);
  }

  pw::Result<IntFunction> BlockingReceive(Receiver<IntFunction>& receiver,
                                          Dispatcher& dispatcher) {
    return receiver.BlockingReceive(dispatcher);
  }

  pw::Result<IntFunction> BlockingReceive(
      Receiver<IntFunction>& receiver,
      Dispatcher& dispatcher,
      pw::chrono::SystemClock::duration timeout) {
    return receiver.BlockingReceive(dispatcher, timeout);
  }

  void TrySend(Sender<IntFunction>& sender, int value) {
    PW_TEST_ASSERT_OK(sender.TrySend([value]() { return value; }));
  }

  void ExpectBlockingReceiveResult(const pw::Result<IntFunction>& result,
                                   int expected) {
    ASSERT_TRUE(result.ok());
    EXPECT_EQ((*result)(), expected);
  }

  pw::Vector<int, 10> GetReceivedBy(
      const pw::async2::test::ReceiverTask<IntFunction>& receiver_task) {
    pw::Vector<int, 10> received;
    for (const auto& v : receiver_task.received()) {
      received.push_back(v());
    }
    return received;
  }

  void ValidateReceived(std::initializer_list<int> expected,
                        const ReceiverTask& receiver_task) {
    auto received = GetReceivedBy(receiver_task);
    ASSERT_EQ(received.size(), expected.size());
    size_t i = 0;
    for (auto value : expected) {
      EXPECT_EQ(received[i++], value);
    }
  }

  ChannelStorage<IntFunction, 2> storage;
};

template <>
struct ChannelAdapter<void> {
  using ReceiverTask = pw::async2::test::ReceiverTask<void>;
  using ReservedSenderTask = pw::async2::test::ReservedSenderTask<void>;
  using SenderTask = pw::async2::test::SenderTask<void>;

  pw::Status BlockingSend(Sender<void>& sender,
                          Dispatcher& dispatcher,
                          int /*value*/) {
    return sender.BlockingSend(dispatcher);
  }

  pw::Status BlockingSend(Sender<void>& sender,
                          Dispatcher& dispatcher,
                          int /*value*/,
                          pw::chrono::SystemClock::duration timeout) {
    return sender.BlockingSend(dispatcher, timeout);
  }

  pw::Status BlockingReceive(Receiver<void>& receiver, Dispatcher& dispatcher) {
    return receiver.BlockingReceive(dispatcher);
  }

  pw::Status BlockingReceive(Receiver<void>& receiver,
                             Dispatcher& dispatcher,
                             pw::chrono::SystemClock::duration timeout) {
    return receiver.BlockingReceive(dispatcher, timeout);
  }

  void TrySend(Sender<void>& sender, int /*value*/) {
    PW_TEST_ASSERT_OK(sender.TrySend());
  }

  void ExpectBlockingReceiveResult(pw::Status status, int /*expected*/) {
    EXPECT_TRUE(status.ok());
  }

  void ValidateReceived(std::initializer_list<int> expected,
                        const ReceiverTask& receiver_task) {
    EXPECT_EQ(receiver_task.received(), expected.size());
  }

  ChannelStorage<void, 2> storage;
};

template <typename T>
class ChannelThreadTest : public ::testing::Test, ChannelAdapter<T> {
  using ReceiverTask = typename ChannelAdapter<T>::ReceiverTask;
  using ReservedSenderTask = typename ChannelAdapter<T>::ReservedSenderTask;
  using SenderTask = typename ChannelAdapter<T>::SenderTask;

  using ChannelAdapter<T>::storage;
  using ChannelAdapter<T>::BlockingSend;
  using ChannelAdapter<T>::BlockingReceive;
  using ChannelAdapter<T>::TrySend;
  using ChannelAdapter<T>::ExpectBlockingReceiveResult;
  using ChannelAdapter<T>::ValidateReceived;

 public:
  void BlockingSend() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    struct {
      DispatcherForTest& dispatcher;
      Sender<T>& sender;
      ChannelAdapter<T>& adapter;
      size_t send_count = 0;
    } sender_context{dispatcher, sender, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread sender_thread(context.options(), [&sender_context]() {
      pw::Status last_status;
      for (int i = 0; i < 10; ++i) {
        last_status = sender_context.adapter.BlockingSend(
            sender_context.sender, sender_context.dispatcher, i);
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

    auto receiver_task = ReceiverTask(std::move(receiver));
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletionUntilReleased();
    sender_thread.join();

    EXPECT_EQ(sender_context.send_count, 10u);
    ValidateReceived({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, receiver_task);
  }

  void BlockingSendChannelCloses() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    struct {
      DispatcherForTest& dispatcher;
      Sender<T>& sender;
      ChannelAdapter<T>& adapter;
      size_t send_count = 0;
    } sender_context{dispatcher, sender, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread sender_thread(context.options(), [&sender_context]() {
      pw::Status last_status;
      for (int i = 0; i < 10; ++i) {
        last_status = sender_context.adapter.BlockingSend(
            sender_context.sender, sender_context.dispatcher, i);
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
    auto receiver_task = ReceiverTask(std::move(receiver), kDisconnectAfter);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletionUntilReleased();
    sender_thread.join();

    EXPECT_GE(sender_context.send_count, kDisconnectAfter);
    EXPECT_LE(sender_context.send_count, kDisconnectAfter + storage.capacity());

    ValidateReceived({0, 1, 2}, receiver_task);
  }

  void BlockingSendTimeout() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    struct {
      DispatcherForTest& dispatcher;
      Sender<T>& sender;
      ChannelAdapter<T>& adapter;
      size_t send_count = 0;
    } sender_context{dispatcher, sender, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread sender_thread(context.options(), [&sender_context]() {
      pw::Status last_status;
      for (int i = 0; i < 10; ++i) {
        last_status = sender_context.adapter.BlockingSend(
            sender_context.sender, sender_context.dispatcher, i, 200ms);
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

  void BlockingSendReturnsImmediatelyIfSpaceAvailable() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    pw::Status sent = BlockingSend(
        sender, dispatcher, 0, pw::chrono::SystemClock::duration(0));
    ASSERT_EQ(sent, pw::OkStatus());

    sent = BlockingSend(
        sender, dispatcher, 1, pw::chrono::SystemClock::duration(0));
    ASSERT_EQ(sent, pw::OkStatus());

    sent = BlockingSend(
        sender, dispatcher, 2, pw::chrono::SystemClock::duration(0));
    ASSERT_EQ(sent, pw::Status::DeadlineExceeded());
  }

  void BlockingReceive() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    struct {
      DispatcherForTest& dispatcher;
      Receiver<T>& receiver;
      ChannelAdapter<T>& adapter;
      size_t receive_count = 0;
    } receiver_context{dispatcher, receiver, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread receiver_thread(context.options(), [&receiver_context]() {
      pw::Status last_status;
      int expected = 0;

      while (true) {
        auto result = receiver_context.adapter.BlockingReceive(
            receiver_context.receiver, receiver_context.dispatcher);
        last_status = AsStatus(result);
        if (last_status.ok()) {
          receiver_context.adapter.ExpectBlockingReceiveResult(result,
                                                               expected);
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

    auto sender_task =
        SenderTask(std::move(sender), {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    dispatcher.Post(sender_task);

    dispatcher.RunToCompletionUntilReleased();
    receiver_thread.join();

    EXPECT_EQ(receiver_context.receive_count, 10u);
  }

  void BlockingReceiveTimeout() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    // Push some values into the channel upfront.
    TrySend(sender, 0);
    TrySend(sender, 1);

    struct {
      DispatcherForTest& dispatcher;
      Receiver<T>& receiver;
      ChannelAdapter<T>& adapter;
      size_t receive_count = 0;
    } receiver_context{dispatcher, receiver, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread receiver_thread(context.options(), [&receiver_context]() {
      pw::Status last_status;
      for (int i = 0; i < 10; ++i) {
        auto result = receiver_context.adapter.BlockingReceive(
            receiver_context.receiver, receiver_context.dispatcher, 200ms);
        last_status = AsStatus(result);
        if (last_status.ok()) {
          receiver_context.adapter.ExpectBlockingReceiveResult(result, i);
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

  void BlockingReceiveReturnsExistingValueImmediately() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    TrySend(sender, 0);
    TrySend(sender, 1);

    auto result = BlockingReceive(
        receiver, dispatcher, pw::chrono::SystemClock::duration(0));
    EXPECT_EQ(AsStatus(result), pw::OkStatus());
    ExpectBlockingReceiveResult(result, 0);

    result = BlockingReceive(
        receiver, dispatcher, pw::chrono::SystemClock::duration(0));
    EXPECT_EQ(AsStatus(result), pw::OkStatus());
    ExpectBlockingReceiveResult(result, 1);

    result = BlockingReceive(
        receiver, dispatcher, pw::chrono::SystemClock::duration(0));
    EXPECT_EQ(AsStatus(result), pw::Status::DeadlineExceeded());
  }

  void BlockingSendAlreadyClosed() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    sender.Disconnect();

    struct {
      DispatcherForTest& dispatcher;
      Sender<T>& sender;
      ChannelAdapter<T>& adapter;
    } sender_context{dispatcher, sender, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread sender_thread(context.options(), [&sender_context]() {
      EXPECT_EQ(sender_context.adapter.BlockingSend(
                    sender_context.sender, sender_context.dispatcher, 1),
                pw::Status::FailedPrecondition());
    });

    dispatcher.AllowBlocking();
    dispatcher.RunToCompletion();
    sender_thread.join();
  }

  void BlockingReceiveAlreadyClosed() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    receiver.Disconnect();

    struct {
      DispatcherForTest& dispatcher;
      Receiver<T>& receiver;
      ChannelAdapter<T>& adapter;
    } receiver_context{dispatcher, receiver, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread receiver_thread(context.options(), [&receiver_context]() {
      auto result = receiver_context.adapter.BlockingReceive(
          receiver_context.receiver, receiver_context.dispatcher);
      EXPECT_EQ(AsStatus(result), pw::Status::FailedPrecondition());
    });

    dispatcher.AllowBlocking();
    dispatcher.RunToCompletion();
    receiver_thread.join();
  }

  void BlockingReceiveClosedWithData() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    TrySend(sender, 1);
    sender.Disconnect();

    struct {
      DispatcherForTest& dispatcher;
      Receiver<T>& receiver;
      ChannelAdapter<T>& adapter;
    } receiver_context{dispatcher, receiver, *this};

    pw::thread::test::TestThreadContext context;
    pw::Thread receiver_thread(context.options(), [&receiver_context]() {
      // Channel is closed, but receiver should still be able to
      // read the data
      EXPECT_FALSE(receiver_context.receiver.is_open());
      auto result = receiver_context.adapter.BlockingReceive(
          receiver_context.receiver, receiver_context.dispatcher);
      EXPECT_EQ(AsStatus(result), pw::OkStatus());
      receiver_context.adapter.ExpectBlockingReceiveResult(result, 1);

      // Now the channel is empty and closed.
      EXPECT_FALSE(receiver_context.receiver.is_open());
      auto result2 = receiver_context.adapter.BlockingReceive(
          receiver_context.receiver, receiver_context.dispatcher);
      EXPECT_EQ(AsStatus(result2), pw::Status::FailedPrecondition());
    });

    dispatcher.AllowBlocking();
    dispatcher.RunToCompletion();
    receiver_thread.join();
  }

 private:
  DispatcherForTest dispatcher;
};

using IntChannelThreadTest = ChannelThreadTest<int>;
using IntFunctionChannelThreadTest = ChannelThreadTest<IntFunction>;
using MoveOnlyIntChannelThreadTest = ChannelThreadTest<MoveOnlyInt>;
using NotificationChannelThreadTest = ChannelThreadTest<void>;

#define TEST_CHANNEL_THREADS(test_name)                            \
  TEST_F(IntChannelThreadTest, test_name) { test_name(); }         \
  TEST_F(IntFunctionChannelThreadTest, test_name) { test_name(); } \
  TEST_F(MoveOnlyIntChannelThreadTest, test_name) { test_name(); } \
  TEST_F(NotificationChannelThreadTest, test_name) { test_name(); }

TEST_CHANNEL_THREADS(BlockingSend)
TEST_CHANNEL_THREADS(BlockingSendChannelCloses)
TEST_CHANNEL_THREADS(BlockingSendTimeout)
TEST_CHANNEL_THREADS(BlockingSendReturnsImmediatelyIfSpaceAvailable)
TEST_CHANNEL_THREADS(BlockingReceive)
TEST_CHANNEL_THREADS(BlockingReceiveTimeout)
TEST_CHANNEL_THREADS(BlockingReceiveReturnsExistingValueImmediately)
TEST_CHANNEL_THREADS(BlockingSendAlreadyClosed)
TEST_CHANNEL_THREADS(BlockingReceiveAlreadyClosed)
TEST_CHANNEL_THREADS(BlockingReceiveClosedWithData)

}  // namespace
