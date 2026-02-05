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

#include "pw_async2/channel.h"

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/try.h"
#include "pw_containers/vector.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::ChannelHandle;
using pw::async2::ChannelStorage;
using pw::async2::Context;
using pw::async2::CreateMpmcChannel;
using pw::async2::CreateMpscChannel;
using pw::async2::CreateSpmcChannel;
using pw::async2::CreateSpscChannel;
using pw::async2::DispatcherForTest;
using pw::async2::FuncTask;
using pw::async2::McChannelHandle;
using pw::async2::MpChannelHandle;
using pw::async2::MpmcChannelHandle;
using pw::async2::MpscChannelHandle;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::ReceiveFuture;
using pw::async2::Receiver;
using pw::async2::ReserveSendFuture;
using pw::async2::Sender;
using pw::async2::SendFuture;
using pw::async2::SendReservation;
using pw::async2::SpmcChannelHandle;
using pw::async2::SpscChannelHandle;
using pw::async2::Task;

class SenderTask : public Task {
 public:
  SenderTask(Sender<int> sender, int start, int end)
      : Task(PW_ASYNC_TASK_NAME("SenderTask")),
        sender_(std::move(sender)),
        next_(start),
        end_(end) {}

  bool succeeded() const { return success_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ <= end_) {
      if (!future_.has_value()) {
        future_.emplace(sender_.Send(next_));
      }

      PW_TRY_READY_ASSIGN(bool sent, future_->Pend(cx));
      if (!sent) {
        success_ = false;
        return Ready();
      }

      future_.reset();
      next_++;
    }

    success_ = true;
    sender_.Disconnect();
    return Ready();
  }

  Sender<int> sender_;
  std::optional<SendFuture<int>> future_;
  bool success_ = false;
  int next_;
  int end_;
};

class ReceiverTask : public Task {
 public:
  ReceiverTask(Receiver<int> receiver)
      : Task(PW_ASYNC_TASK_NAME("ReceiverTask")),
        receiver_(std::move(receiver)) {}

  pw::Vector<int>& received() { return received_; }
  const pw::Vector<int>& received() const { return received_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (true) {
      if (!future_.has_value()) {
        future_.emplace(receiver_.Receive());
      }

      PW_TRY_READY_ASSIGN(std::optional<int> value, future_->Pend(cx));
      if (!value.has_value()) {
        break;
      }

      received_.push_back(*value);
      future_.reset();
    }

    receiver_.Disconnect();
    return Ready();
  }

  Receiver<int> receiver_;
  std::optional<ReceiveFuture<int>> future_;
  pw::Vector<int, 10> received_;
};

class DisconnectingReceiverTask : public Task {
 public:
  DisconnectingReceiverTask(Receiver<int> receiver, size_t disconnect_after)
      : Task(PW_ASYNC_TASK_NAME("DisconnectingReceiverTask")),
        receiver_(std::move(receiver)),
        disconnect_after_(disconnect_after) {}

 private:
  Poll<> DoPend(Context& cx) override {
    while (disconnect_after_ > 0) {
      if (!future_.has_value()) {
        future_.emplace(receiver_.Receive());
      }

      PW_TRY_READY_ASSIGN(std::optional<int> value, future_->Pend(cx));
      if (!value.has_value()) {
        break;
      }
      future_.reset();
      disconnect_after_--;
    }

    receiver_.Disconnect();
    return Ready();
  }

  Receiver<int> receiver_;
  std::optional<ReceiveFuture<int>> future_;
  size_t disconnect_after_;
};

void ExpectReceived1To6(const pw::Vector<int>& received) {
  ASSERT_EQ(received.size(), 6u);
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(received[i], static_cast<int>(i + 1));
  }
}

TEST(StaticChannel, SingleProducerSingleConsumer) {
  DispatcherForTest dispatcher;

  ChannelStorage<int, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  SenderTask sender_task(std::move(sender), 1, 6);
  ReceiverTask receiver_task(std::move(receiver));

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());
  ExpectReceived1To6(receiver_task.received());
}

TEST(StaticChannel, MultiProducerSingleConsumer) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, receiver] = CreateMpscChannel(storage);

  SenderTask sender_task_1(channel.CreateSender(), 1, 3);
  SenderTask sender_task_2(channel.CreateSender(), 4, 6);
  ReceiverTask receiver_task(std::move(receiver));
  channel.Release();

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task_1.succeeded());
  EXPECT_TRUE(sender_task_2.succeeded());

  pw::Vector<int, 10> received(receiver_task.received());
  std::stable_partition(
      received.begin(), received.end(), [](int x) { return x < 4; });
  ExpectReceived1To6(received);
}

TEST(StaticChannel, SingleProducerMultiConsumer) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, sender] = CreateSpmcChannel(storage);

  SenderTask sender_task(std::move(sender), 1, 6);
  ReceiverTask receiver_task_1(channel.CreateReceiver());
  ReceiverTask receiver_task_2(channel.CreateReceiver());
  channel.Release();

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task_1);
  dispatcher.Post(receiver_task_2);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());

  pw::Vector<int, 10> all_received;
  const auto& received_1 = receiver_task_1.received();
  const auto& received_2 = receiver_task_2.received();
  EXPECT_TRUE(std::is_sorted(received_1.begin(), received_1.end()));
  EXPECT_TRUE(std::is_sorted(received_2.begin(), received_2.end()));
  std::merge(received_1.begin(),
             received_1.end(),
             received_2.begin(),
             received_2.end(),
             std::back_inserter(all_received));
  ExpectReceived1To6(all_received);
}

TEST(StaticChannel, MultiProducerMultiConsumer) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  SenderTask sender_task_1(channel.CreateSender(), 1, 3);
  SenderTask sender_task_2(channel.CreateSender(), 4, 6);
  ReceiverTask receiver_task_1(channel.CreateReceiver());
  ReceiverTask receiver_task_2(channel.CreateReceiver());
  channel.Release();

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task_1);
  dispatcher.Post(receiver_task_2);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task_1.succeeded());
  EXPECT_TRUE(sender_task_2.succeeded());

  pw::Vector<int, 10> all_received;
  auto& received_1 = receiver_task_1.received();
  auto& received_2 = receiver_task_2.received();
  std::stable_partition(
      received_1.begin(), received_1.end(), [](int x) { return x < 4; });
  std::stable_partition(
      received_2.begin(), received_2.end(), [](int x) { return x < 4; });
  EXPECT_TRUE(std::is_sorted(received_1.begin(), received_1.end()));
  EXPECT_TRUE(std::is_sorted(received_2.begin(), received_2.end()));
  std::merge(received_1.begin(),
             received_1.end(),
             received_2.begin(),
             received_2.end(),
             std::back_inserter(all_received));
  ExpectReceived1To6(all_received);
}

TEST(StaticChannel, NonAsyncTrySend) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  ReceiverTask receiver_task(channel.CreateReceiver());
  channel.Release();

  dispatcher.Post(receiver_task);

  PW_TEST_EXPECT_OK(sender.TrySend(1));
  PW_TEST_EXPECT_OK(sender.TrySend(2));
  EXPECT_EQ(sender.TrySend(3), pw::Status::Unavailable());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  PW_TEST_EXPECT_OK(sender.TrySend(3));
  PW_TEST_EXPECT_OK(sender.TrySend(4));
  EXPECT_EQ(sender.TrySend(5), pw::Status::Unavailable());
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  PW_TEST_EXPECT_OK(sender.TrySend(5));
  PW_TEST_EXPECT_OK(sender.TrySend(6));
  sender.Disconnect();
  dispatcher.RunToCompletion();

  ExpectReceived1To6(receiver_task.received());
}

TEST(StaticChannel, TryReserveSend) {
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  Receiver<int> receiver = channel.CreateReceiver();
  channel.Release();

  auto r1 = sender.TryReserveSend();
  PW_TEST_EXPECT_OK(r1);
  EXPECT_EQ(sender.remaining_capacity(), 1u);
  auto r2 = sender.TryReserveSend();
  PW_TEST_EXPECT_OK(r2);
  EXPECT_EQ(sender.remaining_capacity(), 0u);
  auto r3 = sender.TryReserveSend();
  EXPECT_EQ(r3.status(), pw::Status::Unavailable());
  EXPECT_EQ(sender.remaining_capacity(), 0u);

  r1->Commit(1);
  EXPECT_EQ(sender.remaining_capacity(), 0u);

  // Read then reserve again.
  auto result = receiver.TryReceive();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, 1);
  auto r4 = sender.TryReserveSend();
  PW_TEST_ASSERT_OK(r4);

  // Disconnect receiver to close the channel.
  receiver.Disconnect();

  auto r5 = sender.TryReserveSend();
  ASSERT_EQ(r5.status(), pw::Status::FailedPrecondition());
}

TEST(StaticChannel, TryReceive) {
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  Receiver<int> receiver = channel.CreateReceiver();
  channel.Release();

  auto result = receiver.TryReceive();
  EXPECT_TRUE(result.status().IsUnavailable());

  PW_TEST_EXPECT_OK(sender.TrySend(1));
  result = receiver.TryReceive();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, 1);

  result = receiver.TryReceive();
  EXPECT_TRUE(result.status().IsUnavailable());

  // Close the channel.
  sender.Disconnect();
  result = receiver.TryReceive();
  EXPECT_TRUE(result.status().IsFailedPrecondition());
}

TEST(StaticChannel, ReceiverDisconnects) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  SenderTask sender_task_1(channel.CreateSender(), 1, 10);
  SenderTask sender_task_2(channel.CreateSender(), 11, 20);
  DisconnectingReceiverTask receiver_task(channel.CreateReceiver(), 3);
  channel.Release();

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_FALSE(sender_task_1.succeeded());
  EXPECT_FALSE(sender_task_2.succeeded());
}

class ReservedSenderTask : public Task {
 public:
  ReservedSenderTask(Sender<int> sender, int start, int end)
      : Task(PW_ASYNC_TASK_NAME("ReservedSenderTask")),
        sender_(std::move(sender)),
        next_(start),
        end_(end) {}

  bool succeeded() const { return success_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ <= end_) {
      if (!future_.has_value()) {
        future_.emplace(sender_.ReserveSend());
      }

      PW_TRY_READY_ASSIGN(auto reservation, future_->Pend(cx));
      if (!reservation.has_value()) {
        success_ = false;
        return Ready();
      }

      reservation->Commit(next_);
      next_++;
      future_.reset();
    }

    success_ = true;
    sender_.Disconnect();
    return Ready();
  }

  Sender<int> sender_;
  std::optional<ReserveSendFuture<int>> future_;
  bool success_ = false;
  int next_;
  int end_;
};

TEST(StaticChannel, ReserveSend) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  ReservedSenderTask sender_task(channel.CreateSender(), 1, 6);
  ReceiverTask receiver_task(channel.CreateReceiver());
  channel.Release();

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());
  ExpectReceived1To6(receiver_task.received());
}

TEST(StaticChannel, ReserveSendReservesSpace) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  ReceiverTask receiver_task(channel.CreateReceiver());

  Sender<int> channel_sender = channel.CreateSender();
  channel.Release();

  static constexpr int kReservedSendValue = 37;
  static constexpr int kFirstSendValue = 40;
  static constexpr int kSecondSendValue = 43;

  FuncTask reserved_sender_task(
      [sender = std::move(channel_sender)](Context& cx) mutable -> Poll<> {
        // This task runs once without sleeping.
        // The channel has two slots. First, we reserve a slot through
        // `ReserveSend`, but don't commit a value. Then, we attempt to write
        // two values through the regular `Send` API. The first should succeed
        // as there is a second available slot, whereas the second should block.
        // Finally, commit the reserved slot.
        auto reserve_send_future = sender.ReserveSend();
        auto send_future_1 = sender.Send(kFirstSendValue);
        auto send_future_2 = sender.Send(kSecondSendValue);
        PW_TRY_READY_ASSIGN(auto reservation, reserve_send_future.Pend(cx));
        EXPECT_EQ(sender.remaining_capacity(), 1u);

        EXPECT_EQ(send_future_1.Pend(cx), Ready(true));
        EXPECT_EQ(sender.remaining_capacity(), 0u);
        EXPECT_EQ(send_future_2.Pend(cx), Pending());

        reservation->Commit(kReservedSendValue);
        EXPECT_EQ(sender.remaining_capacity(), 0u);

        sender.Disconnect();
        return Ready();
      });

  dispatcher.Post(reserved_sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  ASSERT_EQ(receiver_task.received().size(), 2u);
  EXPECT_EQ(receiver_task.received()[0], kFirstSendValue);
  EXPECT_EQ(receiver_task.received()[1], kReservedSendValue);
}

TEST(StaticChannel, ReserveSendReleasesSpaceWhenDropped) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  ReceiverTask receiver_task(channel.CreateReceiver());
  Sender<int> channel_sender = channel.CreateSender();
  channel.Release();

  static constexpr int kFirstSendValue = 40;
  static constexpr int kSecondSendValue = 43;

  FuncTask reserved_sender_task(
      [sender = std::move(channel_sender)](Context& cx) mutable -> Poll<> {
        // This task runs once without sleeping.
        // The channel has two slots. First, we reserve a slot through
        // `ReserveSend`, but don't commit a value. Then, we attempt to write
        // two values through the regular `Send` API. The first should succeed
        // and the second should block. Afterwards, we drop the reservation,
        // which should release the slot, allowing the second send to succeed.
        auto reserve_send_future = sender.ReserveSend();
        auto send_future_1 = sender.Send(kFirstSendValue);
        auto send_future_2 = sender.Send(kSecondSendValue);

        {
          PW_TRY_READY_ASSIGN(auto reservation, reserve_send_future.Pend(cx));
          EXPECT_EQ(sender.remaining_capacity(), 1u);
          EXPECT_EQ(send_future_1.Pend(cx), Ready(true));
          EXPECT_EQ(sender.remaining_capacity(), 0u);
          EXPECT_EQ(send_future_2.Pend(cx), Pending());
          EXPECT_EQ(sender.remaining_capacity(), 0u);
        }

        EXPECT_EQ(sender.remaining_capacity(), 1u);
        EXPECT_EQ(send_future_2.Pend(cx), Ready(true));
        EXPECT_EQ(sender.remaining_capacity(), 0u);

        sender.Disconnect();
        return Ready();
      });

  dispatcher.Post(reserved_sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  ASSERT_EQ(receiver_task.received().size(), 2u);
  EXPECT_EQ(receiver_task.received()[0], kFirstSendValue);
  EXPECT_EQ(receiver_task.received()[1], kSecondSendValue);
}

TEST(StaticChannel, ReserveSendManualCancel) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  ReceiverTask receiver_task(channel.CreateReceiver());
  Sender<int> channel_sender = channel.CreateSender();
  channel.Release();

  static constexpr int kFirstSendValue = 40;
  static constexpr int kSecondSendValue = 43;

  FuncTask reserved_sender_task(
      [sender = std::move(channel_sender)](Context& cx) mutable -> Poll<> {
        // This task runs once without sleeping.
        // The channel has two slots. First, we reserve a slot through
        // `ReserveSend`, but don't commit a value. Then, we attempt to write
        // two values through the regular `Send` API. The first should succeed
        // and the second should block. Afterwards, we cancel the reservation,
        // which should release the slot, allowing the second send to succeed.
        auto reserve_send_future = sender.ReserveSend();
        auto send_future_1 = sender.Send(kFirstSendValue);
        auto send_future_2 = sender.Send(kSecondSendValue);

        PW_TRY_READY_ASSIGN(auto reservation, reserve_send_future.Pend(cx));
        EXPECT_EQ(sender.remaining_capacity(), 1u);
        EXPECT_EQ(send_future_1.Pend(cx), Ready(true));
        EXPECT_EQ(sender.remaining_capacity(), 0u);
        EXPECT_EQ(send_future_2.Pend(cx), Pending());
        EXPECT_EQ(sender.remaining_capacity(), 0u);

        reservation->Cancel();

        EXPECT_EQ(sender.remaining_capacity(), 1u);
        EXPECT_EQ(send_future_2.Pend(cx), Ready(true));
        EXPECT_EQ(sender.remaining_capacity(), 0u);

        sender.Disconnect();
        return Ready();
      });

  dispatcher.Post(reserved_sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  ASSERT_EQ(receiver_task.received().size(), 2u);
  EXPECT_EQ(receiver_task.received()[0], kFirstSendValue);
  EXPECT_EQ(receiver_task.received()[1], kSecondSendValue);
}

TEST(StaticChannel, RemainingCapacity) {
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();

  EXPECT_EQ(sender.remaining_capacity(), 2u);
  EXPECT_EQ(sender.capacity(), 2u);

  PW_TEST_EXPECT_OK(sender.TrySend(1));
  EXPECT_EQ(sender.remaining_capacity(), 1u);
  EXPECT_EQ(sender.capacity(), 2u);

  channel.Release();
}

class MoveOnly {
 public:
  explicit MoveOnly(int val) : value(val) {}

  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;

  MoveOnly(MoveOnly&& other) : value(other.value), moved(other.moved + 1) {}
  MoveOnly& operator=(MoveOnly&& other) {
    value = other.value;
    moved = other.moved + 1;
    return *this;
  }

  operator int() const { return value; }

  int value;
  int moved = 0;
};

TEST(StaticChannel, MoveOnly) {
  DispatcherForTest dispatcher;
  ChannelStorage<MoveOnly, 3> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<MoveOnly> channel_sender = channel.CreateSender();
  Receiver<MoveOnly> channel_receiver = channel.CreateReceiver();
  channel.Release();

  FuncTask sender_task(
      [sender = std::move(channel_sender)](Context& cx) mutable -> Poll<> {
        MoveOnly move_only_1(1);
        auto send_future = sender.Send(std::move(move_only_1));
        EXPECT_EQ(send_future.Pend(cx), Ready(true));

        MoveOnly move_only_2(2);
        auto reserve_send_future = sender.ReserveSend();
        Poll<std::optional<SendReservation<MoveOnly>>> poll =
            reserve_send_future.Pend(cx);
        auto& reservation_1 = poll.value();
        reservation_1->Commit(std::move(move_only_2));

        reserve_send_future = sender.ReserveSend();
        poll = reserve_send_future.Pend(cx);
        auto& reservation_2 = poll.value();
        reservation_2->Commit(3);

        sender.Disconnect();
        return Ready();
      });

  FuncTask receiver_task(
      [receiver = std::move(channel_receiver)](Context& cx) mutable -> Poll<> {
        auto receive_future = receiver.Receive();
        auto poll1 = receive_future.Pend(cx);
        EXPECT_EQ(poll1.value(), MoveOnly(1));

        receive_future = receiver.Receive();
        auto poll2 = receive_future.Pend(cx);
        EXPECT_EQ(poll2.value(), MoveOnly(2));

        receive_future = receiver.Receive();
        auto poll3 = receive_future.Pend(cx);
        EXPECT_EQ(poll3.value(), MoveOnly(3));

        receiver.Disconnect();
        return Ready();
      });

  dispatcher.Post(sender_task);
  dispatcher.RunToCompletion();

  dispatcher.Post(receiver_task);
  dispatcher.RunToCompletion();
}

TEST(StaticChannel, CanPollReceiveFuturesTwice) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, channel_sender, channel_receiver] = CreateSpscChannel(storage);
  channel.Release();

  FuncTask poll_task(
      [receiver = std::move(channel_receiver)](Context& cx) mutable -> Poll<> {
        auto future = receiver.Receive();
        EXPECT_EQ(future.Pend(cx), Pending());
        // As long as the value is Pending(), Pend() should be safe to call
        // repeatedly.
        EXPECT_EQ(future.Pend(cx), Pending());

        receiver.Disconnect();
        return Ready();
      });

  dispatcher.Post(poll_task);
  dispatcher.RunToCompletion();
}

TEST(StaticChannel, CanPollSendFuturesTwice) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto [channel, channel_sender, channel_receiver] = CreateSpscChannel(storage);
  channel.Release();

  ASSERT_TRUE(channel_sender.TrySend(1).ok());
  ASSERT_TRUE(channel_sender.TrySend(2).ok());

  FuncTask poll_task(
      [sender = std::move(channel_sender)](Context& cx) mutable -> Poll<> {
        auto future = sender.Send(3);
        EXPECT_EQ(future.Pend(cx), Pending());
        // As long as the value is Pending(), Pend() should be safe to call
        // repeatedly.
        EXPECT_EQ(future.Pend(cx), Pending());

        sender.Disconnect();
        return Ready();
      });

  dispatcher.Post(poll_task);
  dispatcher.RunToCompletion();
}

TEST(StaticChannel, CanPollReserveSendFutureTwice) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> channel_sender = channel.CreateSender();
  Receiver<int> channel_receiver = channel.CreateReceiver();
  channel.Release();

  ASSERT_TRUE(channel_sender.TrySend(1).ok());
  ASSERT_TRUE(channel_sender.TrySend(2).ok());

  FuncTask poll_task(
      [sender = std::move(channel_sender)](Context& cx) mutable -> Poll<> {
        auto future = sender.ReserveSend();
        EXPECT_EQ(future.Pend(cx), Pending());
        // As long as the value is Pending(), Pend() should be safe to call
        // repeatedly.
        EXPECT_EQ(future.Pend(cx), Pending());

        sender.Disconnect();
        return Ready();
      });

  dispatcher.Post(poll_task);
  dispatcher.RunToCompletion();
}

TEST(DynamicChannel, ForwardsDataAndAutomaticallyDeallocates) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  DispatcherForTest dispatcher;

  std::optional<MpmcChannelHandle<int>> channel =
      CreateMpmcChannel<int>(alloc, 2);
  ASSERT_TRUE(channel.has_value());

  Sender<int> channel_sender = channel->CreateSender();
  Receiver<int> receiver = channel->CreateReceiver();
  channel->Release();

  EXPECT_EQ(alloc.metrics().num_allocations.value(), 2u);
  EXPECT_EQ(alloc.metrics().num_deallocations.value(), 0u);

  SenderTask sender_task(std::move(channel_sender), 1, 6);
  ReceiverTask receiver_task(std::move(receiver));

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  EXPECT_TRUE(sender_task.succeeded());
  ExpectReceived1To6(receiver_task.received());

  EXPECT_EQ(alloc.metrics().allocated_bytes.value(), 0u);
  EXPECT_EQ(alloc.metrics().num_allocations.value(), 2u);
  EXPECT_EQ(alloc.metrics().num_deallocations.value(), 2u);
}

TEST(DynamicChannel, RemainingCapacity) {
  pw::allocator::test::AllocatorForTest<1024> alloc;
  std::optional<MpmcChannelHandle<int>> channel =
      CreateMpmcChannel<int>(alloc, 2);
  ASSERT_TRUE(channel.has_value());

  Sender<int> sender = channel->CreateSender();

  EXPECT_EQ(sender.remaining_capacity(), 2u);
  EXPECT_EQ(sender.capacity(), 2u);

  PW_TEST_EXPECT_OK(sender.TrySend(1));
  EXPECT_EQ(sender.remaining_capacity(), 1u);
  EXPECT_EQ(sender.capacity(), 2u);

  channel->Release();
}

TEST(DynamicChannel, AllocationFailure) {
  pw::allocator::test::AllocatorForTest<64> exhausted_alloc;
  exhausted_alloc.Exhaust();
  std::optional<MpmcChannelHandle<int>> channel =
      CreateMpmcChannel<int>(exhausted_alloc, 2);
  ASSERT_FALSE(channel.has_value());
  EXPECT_EQ(exhausted_alloc.metrics().allocated_bytes.value(), 0u);
  EXPECT_EQ(exhausted_alloc.metrics().num_allocations.value(), 0u);
  EXPECT_EQ(exhausted_alloc.metrics().num_deallocations.value(), 0u);

  // Enough space to allocate the deque, but not enough for the channel.
  // Deque should be allocated then deallocated.
  pw::allocator::test::AllocatorForTest<32> deque_only_alloc;
  channel = CreateMpmcChannel<int>(deque_only_alloc, 2);
  ASSERT_FALSE(channel.has_value());
  EXPECT_EQ(deque_only_alloc.metrics().allocated_bytes.value(), 0u);
  EXPECT_EQ(deque_only_alloc.metrics().num_allocations.value(), 1u);
  EXPECT_EQ(deque_only_alloc.metrics().num_deallocations.value(), 1u);
}

TEST(StaticChannel, SendOnClosedReturnsFalse) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  channel.Release();
  sender.Disconnect();

  EXPECT_FALSE(sender.is_open());
  EXPECT_EQ(sender.TrySend(1), pw::Status::FailedPrecondition());
  EXPECT_EQ(sender.TryReserveSend().status(), pw::Status::FailedPrecondition());

  FuncTask task([&sender](Context& cx) -> Poll<> {
    auto send_future = sender.Send(1);
    EXPECT_TRUE(send_future.is_pendable());
    EXPECT_FALSE(send_future.is_complete());

    PW_TRY_READY_ASSIGN(bool sent, send_future.Pend(cx));
    EXPECT_FALSE(sent);

    auto reserve_future = sender.ReserveSend();
    PW_TRY_READY_ASSIGN(auto reservation, reserve_future.Pend(cx));
    EXPECT_FALSE(reservation.has_value());

    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(StaticChannel, Receive_Closed) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Receiver<int> receiver = channel.CreateReceiver();
  channel.Release();
  receiver.Disconnect();
  ASSERT_FALSE(receiver.is_open());

  EXPECT_EQ(receiver.TryReceive().status(), pw::Status::FailedPrecondition());

  FuncTask task([&receiver](Context& cx) -> Poll<> {
    auto receive_future = receiver.Receive();
    EXPECT_TRUE(receive_future.is_pendable());
    EXPECT_FALSE(receive_future.is_complete());

    PW_TRY_READY_ASSIGN(auto result, receive_future.Pend(cx));
    EXPECT_FALSE(result.has_value());
    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(StaticChannel, TryReceive_ClosedWithData) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  Receiver<int> receiver = channel.CreateReceiver();
  channel.Release();

  PW_TEST_ASSERT_OK(sender.TrySend(1));
  sender.Disconnect();
  ASSERT_FALSE(receiver.is_open());

  auto result = receiver.TryReceive();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, 1);

  EXPECT_EQ(receiver.TryReceive().status(), pw::Status::FailedPrecondition());
}

TEST(StaticChannel, Receive_ClosedWithData) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  Receiver<int> receiver = channel.CreateReceiver();
  channel.Release();

  PW_TEST_ASSERT_OK(sender.TrySend(1));
  sender.Disconnect();
  EXPECT_FALSE(channel.is_open());

  FuncTask task([&receiver](Context& cx) -> Poll<> {
    ReceiveFuture<int> receive_future = receiver.Receive();
    EXPECT_TRUE(receive_future.is_pendable());
    EXPECT_FALSE(receive_future.is_complete());

    auto result = receive_future.Pend(cx);
    EXPECT_TRUE(result.IsReady());
    EXPECT_TRUE(result->has_value());
    EXPECT_EQ(*result, 1);

    PW_TRY_READY_ASSIGN(result, receiver.Receive().Pend(cx));
    EXPECT_TRUE(result.IsReady());
    EXPECT_FALSE(result->has_value());

    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(StaticChannel, CreateSenderWhenClosed) {
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);
  channel.Close();

  Sender<int> sender = channel.CreateSender();
  EXPECT_FALSE(sender.is_open());
}

TEST(StaticChannel, CreateReceiverWhenClosed) {
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);
  channel.Close();

  Receiver<int> receiver = channel.CreateReceiver();
  EXPECT_FALSE(receiver.is_open());
}

TEST(StaticChannel, MoveFuture) {
  DispatcherForTest dispatcher;
  ChannelStorage<int, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  Sender<int> sender = channel.CreateSender();
  Receiver<int> receiver = channel.CreateReceiver();
  channel.Release();

  PW_TEST_ASSERT_OK(sender.TrySend(404));

  FuncTask task([&receiver](Context& cx) -> Poll<> {
    ReceiveFuture<int> future = receiver.Receive();
    EXPECT_TRUE(future.is_pendable());
    EXPECT_FALSE(future.is_complete());

    ReceiveFuture<int> assigned;
    EXPECT_FALSE(assigned.is_pendable());
    EXPECT_FALSE(assigned.is_complete());

    assigned = std::move(future);
    EXPECT_TRUE(assigned.is_pendable());
    EXPECT_FALSE(assigned.is_complete());

    EXPECT_FALSE(future.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(future.is_complete());  // NOLINT(bugprone-use-after-move)

    ReceiveFuture<int> constructed(std::move(assigned));

    EXPECT_FALSE(assigned.is_pendable());  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(assigned.is_complete());  // NOLINT(bugprone-use-after-move)

    auto result = constructed.Pend(cx);
    EXPECT_EQ(result.value(), 404);

    EXPECT_FALSE(constructed.is_pendable());
    EXPECT_TRUE(constructed.is_complete());

    return Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
}

TEST(ChannelHandles, DefaultConstruct) {
  SpscChannelHandle<int> channel1;
  EXPECT_FALSE(channel1.is_open());
  SpmcChannelHandle<int> channel2;
  EXPECT_FALSE(channel2.is_open());
  MpscChannelHandle<int> channel3;
  EXPECT_FALSE(channel3.is_open());
  MpmcChannelHandle<int> channel4;
  EXPECT_FALSE(channel4.is_open());

  Sender<int> sender;
  EXPECT_FALSE(sender.is_open());

  Receiver<int> receiver;
  EXPECT_FALSE(receiver.is_open());
}

TEST(ChannelHandles, MpChannelHandle_CopyAndMove) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  auto channel_opt = CreateMpmcChannel<int>(alloc, 2);
  ASSERT_TRUE(channel_opt.has_value());
  MpmcChannelHandle<int>& handle = *channel_opt;

  MpChannelHandle<int> mp1 = handle;
  EXPECT_TRUE(mp1.is_open());

  MpChannelHandle<int> mp2;
  mp2 = handle;
  EXPECT_TRUE(mp2.is_open());

  MpChannelHandle<int> mp3 = std::move(handle);
  EXPECT_TRUE(mp3.is_open());
  EXPECT_FALSE(handle.is_open());  // NOLINT(bugprone-use-after-move)
}

TEST(ChannelHandles, McChannelHandle_CopyAndMove) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  auto channel_opt = CreateMpmcChannel<int>(alloc, 2);
  ASSERT_TRUE(channel_opt.has_value());
  MpmcChannelHandle<int>& handle = *channel_opt;

  McChannelHandle<int> mc1 = handle;
  EXPECT_TRUE(mc1.is_open());

  McChannelHandle<int> mc2;
  mc2 = handle;
  EXPECT_TRUE(mc2.is_open());

  McChannelHandle<int> mc3 = std::move(handle);
  EXPECT_FALSE(handle.is_open());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(mc3.is_open());
}

TEST(ChannelHandles, MpscCopyToAsMpHandle) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  auto [handle, receiver] = CreateMpscChannel<int>(alloc, 2).value();

  MpChannelHandle<int> mp_handle = handle;
  EXPECT_TRUE(handle.is_open());
  EXPECT_TRUE(mp_handle.is_open());

  mp_handle.Close();
  EXPECT_FALSE(handle.is_open());
  EXPECT_FALSE(mp_handle.is_open());
}

TEST(ChannelHandles, MpmcMoveToMpHandle) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  MpmcChannelHandle<int> handle = CreateMpmcChannel<int>(alloc, 2).value();

  MpChannelHandle<int> mp_handle = std::move(handle);
  EXPECT_FALSE(handle.is_open());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(mp_handle.is_open());
}

TEST(ChannelHandles, SpmcCopyToMcHandle) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  auto [handle, sender] = CreateSpmcChannel<int>(alloc, 2).value();

  McChannelHandle<int> mc_handle = handle;
  EXPECT_TRUE(handle.is_open());
  EXPECT_TRUE(mc_handle.is_open());

  mc_handle.Close();
  EXPECT_FALSE(handle.is_open());
  EXPECT_FALSE(mc_handle.is_open());
}

TEST(ChannelHandles, MpmcMoveToMcHandle) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  MpmcChannelHandle<int> handle = CreateMpmcChannel<int>(alloc, 2).value();

  McChannelHandle<int> mc_handle = std::move(handle);
  EXPECT_FALSE(handle.is_open());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(mc_handle.is_open());
}

TEST(ChannelHandles, SpccCopyToChannelHandle) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  MpmcChannelHandle<int> mpmc_handle = CreateMpmcChannel<int>(alloc, 2).value();

  ChannelHandle<int> handle;
  EXPECT_FALSE(handle.is_open());

  handle = mpmc_handle;
  EXPECT_TRUE(mpmc_handle.is_open());
  EXPECT_TRUE(handle.is_open());

  handle.Close();
  EXPECT_FALSE(mpmc_handle.is_open());
  EXPECT_FALSE(handle.is_open());
}

TEST(ChannelHandles, MpmcMoveToChannelHandle) {
  pw::allocator::test::AllocatorForTest<256> alloc;
  MpmcChannelHandle<int> mpmc_handle = CreateMpmcChannel<int>(alloc, 2).value();

  ChannelHandle<int> handle = std::move(mpmc_handle);
  EXPECT_FALSE(mpmc_handle.is_open());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(handle.is_open());
}

}  // namespace
