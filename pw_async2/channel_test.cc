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
#include "pw_async2/await.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/internal/channel_test_util.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::Status;
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
using pw::async2::Sender;
using pw::async2::SendFuture;
using pw::async2::SendReservation;
using pw::async2::SpmcChannelHandle;
using pw::async2::SpscChannelHandle;

using IntFunction = pw::InlineFunction<int()>;
using MoveOnlyInt = pw::async2::test::MoveOnlyInt;

constexpr pw::Status AsStatus(pw::Status status) { return status; }
template <typename T>
constexpr pw::Status AsStatus(const pw::Result<T>& result) {
  return result.status();
}

constexpr bool IsValidMpMcOrder(pw::span<pw::span<const int>> produced,
                                pw::span<pw::span<const int>> consumed) {
  // Checks if the sequences in `consumed` are some interleaving of the
  // sequences in `produced`.

  // This implementation makes the simplifying assumption that any value
  // produced is only produced once by any producer.

  size_t produced_size = 0;
  for (const auto& seq : produced) {
    produced_size += seq.size();
  }

  size_t consumed_size = 0;
  for (const auto& seq : consumed) {
    consumed_size += seq.size();
  }

  if (produced_size != consumed_size) {
    return false;
  }

  // We expect at every iteration that a value from the front of a produced
  // sequence is at the front of one of the consumed sequences.
  //
  // If there is a match, the value is removed from both the producer and
  // consumer.
  //
  // This process repeats until we run out of produced values (success), or
  // don't find a consumer for a produced value (failure).

  while (produced_size > 0) {
    bool found = false;
    for (auto& produced_seq : produced) {
      if (produced_seq.empty()) {
        continue;
      }

      int value = produced_seq.front();

      for (auto& consumed_seq : consumed) {
        if (consumed_seq.empty()) {
          continue;
        }

        if (consumed_seq.front() == value) {
          consumed_seq = consumed_seq.subspan(1);
          consumed_size--;
          found = true;
          break;
        }
      }

      if (found) {
        produced_seq = produced_seq.subspan(1);
        produced_size--;
        break;
      }
    }

    if (!found) {
      return false;
    }
  }

  return produced_size == 0 && consumed_size == 0;
}

// The above implementation is complex enough that it is worth some quick
// correctness tests.

// This adapter allows more concise compile-time tests, as the inputs to the
// implementation must be modifiable, which requires making a copy.
constexpr bool IsValidMpMcOrder(
    std::initializer_list<std::initializer_list<int>> orig_produced,
    std::initializer_list<std::initializer_list<int>> orig_consumed) {
  std::array<pw::span<const int>, 10> produced_copy;
  std::array<pw::span<const int>, 10> consumed_copy;

  size_t produced_count = 0;
  for (const auto& seq : orig_produced) {
    produced_copy[produced_count++] = seq;
  }

  size_t consumed_count = 0;
  for (const auto& seq : orig_consumed) {
    consumed_copy[consumed_count++] = seq;
  }

  return IsValidMpMcOrder(pw::span(produced_copy.data(), produced_count),
                          pw::span(consumed_copy.data(), consumed_count));
}

static_assert(IsValidMpMcOrder({}, {}));

// There is one way for a single producer to distribute (1, 2, 3) to a single
// consumer.
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{1, 2, 3}}));

// Any other order is invalid.
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2, 3, 1}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2, 1, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{3, 1, 2}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{3, 2, 1}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1, 3, 2}}));

// Missing one or more values is invalid
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{3}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}}));

// There are eight valid ways a single producer can distribute (1, 2, 3) to two
// consumers.
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{1, 2, 3}, {}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{2, 3}, {1}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{1, 3}, {2}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{1, 2}, {3}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{1}, {2, 3}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{2}, {1, 3}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{3}, {1, 2}}));
static_assert(IsValidMpMcOrder({{1, 2, 3}}, {{}, {1, 2, 3}}));

// Missing one or more values is invalid
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2}, {3}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{3}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {2, 3}}));

static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1, 2}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2}, {1}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {1, 2}}));

static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1, 2}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2}, {1}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {1, 2}}));

static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{1}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {1}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{2}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {3}}));

static_assert(!IsValidMpMcOrder({{1, 2, 3}}, {{}, {}}));

// There are 144 ways one producer can distribute (1, 2) and another
// producer can distribute (3, 4) to two consumers. These are half of them.
//
// The other half are the same but with the two consumers swapped.
//
// You can generate an additional 144 cases by swapping the two producers as
// well.
//
// Not included are tests where one or more values are missing from the
// consumers.

// 24 ways for all values to end up with one consumer, but only 6 are valid.
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 2, 4, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 4, 2, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 4, 3, 2}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 1, 3, 4}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 1, 4, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 3, 1, 4}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 3, 4, 1}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 4, 1, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 4, 3, 1}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 2, 1, 4}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 2, 4, 1}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 4, 2, 1}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 1, 2, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 1, 3, 2}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 2, 1, 3}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 2, 3, 1}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 3, 1, 2}, {}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 3, 2, 1}, {}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 2, 3, 4}, {}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 3, 2, 4}, {}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 3, 4, 2}, {}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 1, 2, 4}, {}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 1, 4, 2}, {}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 4, 1, 2}, {}}));

// 24 ways for three values to end up with one consumer, but only 12 are valid.
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 4, 3}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 1, 3}, {4}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 1, 4}, {3}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 3, 1}, {4}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 4, 1}, {3}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 4, 3}, {1}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 2, 1}, {4}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 1, 3}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 2, 1}, {3}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 2, 3}, {1}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 3, 1}, {2}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 3, 2}, {1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 2, 3}, {4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 2, 4}, {3}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 3, 2}, {4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 3, 4}, {2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 4, 2}, {3}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 3, 4}, {1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 1, 2}, {4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 1, 4}, {2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 2, 4}, {1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 4, 1}, {2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 4, 2}, {1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 1, 2}, {3}}));

// 24 ways for two values to end up with one consumer, but only 16 are valid
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 2}, {4, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 1}, {3, 4}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 1}, {4, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 3}, {4, 1}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 4}, {2, 1}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 1}, {2, 3}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 3}, {1, 2}}));
static_assert(!IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 3}, {2, 1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 2}, {3, 4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 3}, {2, 4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 3}, {4, 2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 4}, {2, 3}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{1, 4}, {3, 2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 3}, {1, 4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 4}, {1, 3}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{2, 4}, {3, 1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 1}, {2, 4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 1}, {4, 2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 2}, {1, 4}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 2}, {4, 1}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{3, 4}, {1, 2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 1}, {3, 2}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 2}, {1, 3}}));
static_assert(IsValidMpMcOrder({{1, 2}, {3, 4}}, {{4, 2}, {3, 1}}));

template <typename T>
struct ChannelAdapter {
  using ReceiverTask = pw::async2::test::ReceiverTask<T>;
  using ReservedSenderTask = pw::async2::test::ReservedSenderTask<T, int>;
  using SenderTask = pw::async2::test::SenderTask<T, int>;

  bool IsDynamicChannelDequeAllocated() const { return true; }

  SendFuture<T> Send(Sender<T>& sender, int value) {
    return sender.Send(T(value));
  }

  Status TrySend(Sender<T>& sender, int value) {
    return sender.TrySend(T(value));
  }

  template <typename U>
  Status TrySend(Sender<T>& sender, U&& value) {
    return sender.TrySend(std::forward<U>(value));
  }

  void Commit(SendReservation<T>& reservation, int value) {
    reservation.Commit(value);
  }

  void ExpectReceiveFutureResolution(const Poll<std::optional<T>>& result,
                                     int value) {
    ASSERT_TRUE(result.IsReady());
    EXPECT_EQ(*result, value);
  }

  void ExpectTryReceiveResult(const pw::Result<T>& result, int value) {
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(*result, value);
  }

  void ExpectProducedWasConsumed(
      std::initializer_list<std::initializer_list<int>> produced,
      std::initializer_list<const ReceiverTask*> consumers) {
    pw::Vector<pw::Vector<int, 10>, 10> consumed;
    for (const auto* consumer : consumers) {
      consumed.push_back(GetReceivedBy(consumer));
    }

    pw::Vector<pw::span<const int>, 10> consumed_spans;
    for (const auto& consumer : consumed) {
      consumed_spans.push_back(pw::span(consumer));
    }

    pw::Vector<pw::span<const int>, 10> produced_spans;
    for (const auto& producer : produced) {
      produced_spans.push_back(pw::span<const int>(producer));
    }

    EXPECT_TRUE(IsValidMpMcOrder(consumed_spans, produced_spans));
  }

  pw::Vector<int, 10> GetReceivedBy(const ReceiverTask* receiver_task) {
    pw::Vector<int, 10> received;
    for (const auto& v : receiver_task->received()) {
      received.push_back(v);
    }
    return received;
  }

  ChannelStorage<T, 2> storage;
};

template <>
struct ChannelAdapter<IntFunction> {
  using ReceiverTask = pw::async2::test::ReceiverTask<IntFunction>;
  using ReservedSenderTask =
      pw::async2::test::ReservedSenderTask<IntFunction, int>;
  using SenderTask = pw::async2::test::SenderTask<IntFunction, int>;

  bool IsDynamicChannelDequeAllocated() const { return true; }

  SendFuture<IntFunction> Send(Sender<IntFunction>& sender, int value) {
    return sender.Send([value]() { return value; });
  }

  Status TrySend(Sender<IntFunction>& sender, int value) {
    return sender.TrySend([value]() { return value; });
  }

  void Commit(SendReservation<IntFunction>& reservation, int value) {
    reservation.Commit([value]() { return value; });
  }

  void ExpectReceiveFutureResolution(Poll<std::optional<IntFunction>>& result,
                                     int value) {
    ASSERT_TRUE(result.IsReady());
    ASSERT_TRUE(*result);
    EXPECT_EQ((**result)(), value);
  }

  void ExpectTryReceiveResult(const pw::Result<IntFunction>& result,
                              int value) {
    EXPECT_TRUE(result.ok());
    EXPECT_EQ((*result)(), value);
  }

  void ExpectProducedWasConsumed(
      std::initializer_list<std::initializer_list<int>> produced,
      std::initializer_list<const ReceiverTask*> consumers) {
    pw::Vector<pw::Vector<int, 10>, 10> consumed;
    for (const auto* consumer : consumers) {
      consumed.push_back(GetReceivedBy(consumer));
    }

    pw::Vector<pw::span<const int>, 10> consumed_spans;
    for (const auto& consumer : consumed) {
      consumed_spans.push_back(pw::span(consumer));
    }

    pw::Vector<pw::span<const int>, 10> produced_spans;
    for (const auto& producer : produced) {
      produced_spans.push_back(pw::span<const int>(producer));
    }

    EXPECT_TRUE(IsValidMpMcOrder(consumed_spans, produced_spans));
  }

  pw::Vector<int, 10> GetReceivedBy(const ReceiverTask* receiver_task) {
    pw::Vector<int, 10> received;
    for (const auto& v : receiver_task->received()) {
      received.push_back(v());
    }
    return received;
  }

  ChannelStorage<IntFunction, 2> storage;
};

template <>
struct ChannelAdapter<void> {
  using ReceiverTask = pw::async2::test::ReceiverTask<void>;
  using ReservedSenderTask = pw::async2::test::ReservedSenderTask<void>;
  using SenderTask = pw::async2::test::SenderTask<void>;

  bool IsDynamicChannelDequeAllocated() const { return false; }

  SendFuture<void> Send(Sender<void>& sender, int value) {
    std::ignore = value;  // Not for notifications
    return sender.Send();
  }

  Status TrySend(Sender<void>& sender, int value) {
    std::ignore = value;  // Not for notifications
    return sender.TrySend();
  }

  void Commit(SendReservation<void>& reservation, int value) {
    std::ignore = value;  // Not for notifications
    reservation.CommitNotification();
  }

  void ExpectReceiveFutureResolution(Poll<bool>& result, int value) {
    ASSERT_TRUE(result.IsReady());
    EXPECT_TRUE(*result);
    std::ignore = value;  // Not for notifications
  }

  void ExpectTryReceiveResult(pw::Status status, int value) {
    EXPECT_TRUE(status.ok());
    std::ignore = value;
  }

  void ExpectProducedWasConsumed(
      std::initializer_list<std::initializer_list<int>> produced,
      std::initializer_list<const ReceiverTask*> consumers) {
    size_t total_produced = 0;
    for (const auto& produced_list : produced) {
      total_produced += produced_list.size();
    }
    size_t total_consumed = 0;
    for (const auto* consumer : consumers) {
      total_consumed += consumer->received();
    }
    EXPECT_EQ(total_produced, total_consumed);
  }

  ChannelStorage<void, 2> storage;
};

template <typename T>
class ChannelTest : public ::testing::Test, ChannelAdapter<T> {
  using ReceiverTask = typename ChannelAdapter<T>::ReceiverTask;
  using ReservedSenderTask = typename ChannelAdapter<T>::ReservedSenderTask;
  using SenderTask = typename ChannelAdapter<T>::SenderTask;

  using ChannelAdapter<T>::storage;
  using ChannelAdapter<T>::IsDynamicChannelDequeAllocated;
  using ChannelAdapter<T>::ExpectProducedWasConsumed;
  using ChannelAdapter<T>::TrySend;
  using ChannelAdapter<T>::Send;
  using ChannelAdapter<T>::Commit;
  using ChannelAdapter<T>::ExpectTryReceiveResult;
  using ChannelAdapter<T>::ExpectReceiveFutureResolution;

 public:
  void StaticSingleProducerSingleConsumer() {
    auto [channel, sender, receiver] = CreateSpscChannel(storage);
    channel.Release();

    auto sender_task = SenderTask(std::move(sender), {1, 2, 3, 4, 5, 6});
    auto receiver_task = ReceiverTask(std::move(receiver));

    dispatcher.Post(sender_task);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletion();

    EXPECT_TRUE(sender_task.succeeded());
    ExpectProducedWasConsumed({{1, 2, 3, 4, 5, 6}}, {&receiver_task});
  }

  void StaticMultiProducerSingleConsumer() {
    auto [channel, receiver] = CreateMpscChannel(storage);

    auto sender_task_1 = SenderTask(channel.CreateSender(), {1, 2, 3});
    auto sender_task_2 = SenderTask(channel.CreateSender(), {4, 5, 6});
    auto receiver_task = ReceiverTask(std::move(receiver));
    channel.Release();

    dispatcher.Post(sender_task_1);
    dispatcher.Post(sender_task_2);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletion();

    EXPECT_TRUE(sender_task_1.succeeded());
    EXPECT_TRUE(sender_task_2.succeeded());

    ExpectProducedWasConsumed({{1, 2, 3}, {4, 5, 6}}, {&receiver_task});
  }

  void StaticSingleProducerMultiConsumer() {
    auto [channel, sender] = CreateSpmcChannel(storage);

    auto sender_task = SenderTask(std::move(sender), {1, 2, 3, 4, 5, 6});
    auto receiver_task_1 = ReceiverTask(channel.CreateReceiver());
    auto receiver_task_2 = ReceiverTask(channel.CreateReceiver());
    channel.Release();

    dispatcher.Post(sender_task);
    dispatcher.Post(receiver_task_1);
    dispatcher.Post(receiver_task_2);

    dispatcher.RunToCompletion();

    EXPECT_TRUE(sender_task.succeeded());
    ExpectProducedWasConsumed({{1, 2, 3, 4, 5, 6}},
                              {&receiver_task_1, &receiver_task_2});
  }

  void StaticMultiProducerMultiConsumer() {
    auto channel = CreateMpmcChannel(storage);

    auto sender_task_1 = SenderTask(channel.CreateSender(), {1, 2, 3});
    auto sender_task_2 = SenderTask(channel.CreateSender(), {4, 5, 6});
    auto receiver_task_1 = ReceiverTask(channel.CreateReceiver());
    auto receiver_task_2 = ReceiverTask(channel.CreateReceiver());
    channel.Release();

    dispatcher.Post(sender_task_1);
    dispatcher.Post(sender_task_2);
    dispatcher.Post(receiver_task_1);
    dispatcher.Post(receiver_task_2);

    dispatcher.RunToCompletion();

    EXPECT_TRUE(sender_task_1.succeeded());
    EXPECT_TRUE(sender_task_2.succeeded());

    ExpectProducedWasConsumed({{1, 2, 3}, {4, 5, 6}},
                              {&receiver_task_1, &receiver_task_2});
  }

  void StaticNonAsyncTrySend() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    auto receiver_task = ReceiverTask(channel.CreateReceiver());
    channel.Release();

    dispatcher.Post(receiver_task);

    PW_TEST_EXPECT_OK(TrySend(sender, 1));
    PW_TEST_EXPECT_OK(TrySend(sender, 2));
    EXPECT_EQ(TrySend(sender, 3), pw::Status::Unavailable());
    EXPECT_TRUE(dispatcher.RunUntilStalled());

    PW_TEST_EXPECT_OK(TrySend(sender, 3));
    PW_TEST_EXPECT_OK(TrySend(sender, 4));
    EXPECT_EQ(TrySend(sender, 5), pw::Status::Unavailable());
    EXPECT_TRUE(dispatcher.RunUntilStalled());

    PW_TEST_EXPECT_OK(TrySend(sender, 5));
    PW_TEST_EXPECT_OK(TrySend(sender, 6));
    sender.Disconnect();
    dispatcher.RunToCompletion();

    ExpectProducedWasConsumed({{1, 2, 3, 4, 5, 6}}, {&receiver_task});
  }

  void StaticTryReserveSend() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    auto receiver = channel.CreateReceiver();
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

    Commit(*r1, 1);
    EXPECT_EQ(sender.remaining_capacity(), 0u);

    // Read then reserve again.
    auto result = receiver.TryReceive();
    ExpectTryReceiveResult(result, 1);

    auto r4 = sender.TryReserveSend();
    PW_TEST_ASSERT_OK(r4);

    // Disconnect receiver to close the channel.
    receiver.Disconnect();

    auto r5 = sender.TryReserveSend();
    ASSERT_EQ(r5.status(), pw::Status::FailedPrecondition());
  }

  void StaticTryReceive() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    auto receiver = channel.CreateReceiver();
    channel.Release();

    auto result = receiver.TryReceive();
    EXPECT_TRUE(AsStatus(result).IsUnavailable());

    PW_TEST_EXPECT_OK(TrySend(sender, 1));
    result = receiver.TryReceive();
    ExpectTryReceiveResult(result, 1);

    result = receiver.TryReceive();
    EXPECT_TRUE(AsStatus(result).IsUnavailable());

    // Close the channel.
    sender.Disconnect();
    result = receiver.TryReceive();
    EXPECT_TRUE(AsStatus(result).IsFailedPrecondition());
  }

  void StaticReceiverDisconnects() {
    auto channel = CreateMpmcChannel(storage);

    auto sender_task_1 =
        SenderTask(channel.CreateSender(), {11, 12, 13, 14, 15, 16, 17, 18});
    auto sender_task_2 =
        SenderTask(channel.CreateSender(), {21, 22, 23, 24, 25, 26, 27, 28});
    auto receiver_task = ReceiverTask(channel.CreateReceiver(), 3);
    channel.Release();

    dispatcher.Post(sender_task_1);
    dispatcher.Post(sender_task_2);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletion();

    EXPECT_FALSE(sender_task_1.succeeded());
    EXPECT_FALSE(sender_task_2.succeeded());
  }

  void StaticReserveSend() {
    auto channel = CreateMpmcChannel(storage);

    auto sender_task =
        ReservedSenderTask(channel.CreateSender(), {1, 2, 3, 4, 5, 6});
    auto receiver_task = ReceiverTask(channel.CreateReceiver());
    channel.Release();

    dispatcher.Post(sender_task);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletion();

    EXPECT_TRUE(sender_task.succeeded());
    ExpectProducedWasConsumed({{1, 2, 3, 4, 5, 6}}, {&receiver_task});
  }

  void StaticRemainingCapacity() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();

    EXPECT_EQ(sender.remaining_capacity(), 2u);
    EXPECT_EQ(sender.capacity(), 2u);

    PW_TEST_EXPECT_OK(TrySend(sender, 1));
    EXPECT_EQ(sender.remaining_capacity(), 1u);
    EXPECT_EQ(sender.capacity(), 2u);

    channel.Release();
  }

  void StaticSendOnClosedReturnsFalse() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    channel.Release();
    sender.Disconnect();

    EXPECT_FALSE(sender.is_open());
    EXPECT_EQ(TrySend(sender, 1), pw::Status::FailedPrecondition());
    EXPECT_EQ(sender.TryReserveSend().status(),
              pw::Status::FailedPrecondition());

    FuncTask task([this, &sender](Context& cx) -> Poll<> {
      auto send_future = Send(sender, 1);

      PW_AWAIT(bool sent, send_future, cx);
      EXPECT_FALSE(sent);

      auto reserve_future = sender.ReserveSend();
      PW_AWAIT(auto reservation, reserve_future, cx);
      EXPECT_FALSE(reservation.has_value());

      return Ready();
    });

    dispatcher.Post(task);
    dispatcher.RunToCompletion();
  }

  void StaticReceiveClosed() {
    auto channel = CreateMpmcChannel(storage);

    auto receiver = channel.CreateReceiver();
    channel.Release();
    receiver.Disconnect();
    ASSERT_FALSE(receiver.is_open());

    EXPECT_EQ(AsStatus(receiver.TryReceive()),
              pw::Status::FailedPrecondition());

    FuncTask task([&receiver](Context& cx) -> Poll<> {
      auto receive_future = receiver.Receive();

      PW_AWAIT(auto result, receive_future, cx);
      EXPECT_FALSE(result);
      return Ready();
    });

    dispatcher.Post(task);
    dispatcher.RunToCompletion();
  }

  void StaticTryReceiveClosedWithData() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    auto receiver = channel.CreateReceiver();
    channel.Release();

    PW_TEST_ASSERT_OK(TrySend(sender, 1));
    sender.Disconnect();
    ASSERT_FALSE(receiver.is_open());

    auto result = receiver.TryReceive();
    ExpectTryReceiveResult(result, 1);

    EXPECT_EQ(AsStatus(receiver.TryReceive()),
              pw::Status::FailedPrecondition());
  }

  void StaticReceiveClosedWithData() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    auto receiver = channel.CreateReceiver();
    channel.Release();

    PW_TEST_ASSERT_OK(TrySend(sender, 1));
    sender.Disconnect();
    EXPECT_FALSE(channel.is_open());

    FuncTask task([this, &receiver](Context& cx) -> Poll<> {
      auto receive_future = receiver.Receive();

      auto result = receive_future.Pend(cx);
      ExpectReceiveFutureResolution(result, 1);

      PW_AWAIT(auto result2, receiver.Receive(), cx);
      EXPECT_FALSE(result2);

      return Ready();
    });

    dispatcher.Post(task);
    dispatcher.RunToCompletion();
  }

  void StaticCreateSenderWhenClosed() {
    auto channel = CreateMpmcChannel(storage);
    channel.Close();

    auto sender = channel.CreateSender();
    EXPECT_FALSE(sender.is_open());
  }

  void StaticCreateReceiverWhenClosed() {
    auto channel = CreateMpmcChannel(storage);
    channel.Close();

    auto receiver = channel.CreateReceiver();
    EXPECT_FALSE(receiver.is_open());
  }

  void StaticCanPollReceiveFuturesTwice() {
    auto [channel, channel_sender, channel_receiver] =
        CreateSpscChannel(storage);
    channel.Release();

    FuncTask poll_task([receiver = std::move(channel_receiver)](
                           Context& cx) mutable -> Poll<> {
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

  void StaticCanPollSendFuturesTwice() {
    auto [channel, channel_sender, channel_receiver] =
        CreateSpscChannel(storage);
    channel.Release();

    ASSERT_TRUE(TrySend(channel_sender, 1).ok());
    ASSERT_TRUE(TrySend(channel_sender, 2).ok());

    FuncTask poll_task([this, sender = std::move(channel_sender)](
                           Context& cx) mutable -> Poll<> {
      auto future = Send(sender, 3);
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

  void StaticCanPollReserveSendFutureTwice() {
    auto channel = CreateMpmcChannel(storage);

    auto channel_sender = channel.CreateSender();
    auto channel_receiver = channel.CreateReceiver();
    channel.Release();

    ASSERT_TRUE(TrySend(channel_sender, 1).ok());
    ASSERT_TRUE(TrySend(channel_sender, 2).ok());

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

  void StaticMoveFuture() {
    auto channel = CreateMpmcChannel(storage);

    auto sender = channel.CreateSender();
    auto receiver = channel.CreateReceiver();
    channel.Release();

    PW_TEST_ASSERT_OK(TrySend(sender, 404));

    FuncTask task([this, &receiver](Context& cx) -> Poll<> {
      auto future = receiver.Receive();

      using FutureType = decltype(future);
      FutureType assigned;
      EXPECT_FALSE(assigned.is_pendable());

      assigned = std::move(future);

      EXPECT_FALSE(future.is_pendable());  // NOLINT(bugprone-use-after-move)
      EXPECT_FALSE(future.is_complete());  // NOLINT(bugprone-use-after-move)

      FutureType constructed(std::move(assigned));

      EXPECT_FALSE(assigned.is_pendable());  // NOLINT(bugprone-use-after-move)
      EXPECT_FALSE(assigned.is_complete());  // NOLINT(bugprone-use-after-move)
      auto result = constructed.Pend(cx);
      ExpectReceiveFutureResolution(result, 404);

      EXPECT_FALSE(constructed.is_pendable());
      EXPECT_TRUE(constructed.is_complete());

      return Ready();
    });

    dispatcher.Post(task);
    dispatcher.RunToCompletion();
  }

  void StaticReserveSendReservesSpace() {
    auto channel = CreateMpmcChannel(storage);
    auto receiver_task = ReceiverTask(channel.CreateReceiver());
    auto channel_sender = channel.CreateSender();
    channel.Release();

    static constexpr int kReservedSendValue = 37;
    static constexpr int kFirstSendValue = 40;
    static constexpr int kSecondSendValue = 43;

    FuncTask reserved_sender_task([this, sender = std::move(channel_sender)](
                                      Context& cx) mutable -> Poll<> {
      // This task runs once without sleeping.
      // The channel has two slots. First, we reserve a slot through
      // `ReserveSend`, but don't commit a value. Then, we attempt to write
      // two values through the regular `Send` API. The first should succeed
      // as there is a second available slot, whereas the second should
      // block. Finally, commit the reserved slot.
      auto reserve_send_future = sender.ReserveSend();
      auto send_future_1 = Send(sender, kFirstSendValue);
      auto send_future_2 = Send(sender, kSecondSendValue);
      PW_AWAIT(auto reservation, reserve_send_future, cx);
      EXPECT_EQ(sender.remaining_capacity(), 1u);

      EXPECT_EQ(send_future_1.Pend(cx), Ready(true));
      EXPECT_EQ(sender.remaining_capacity(), 0u);
      EXPECT_EQ(send_future_2.Pend(cx), Pending());

      Commit(*reservation, kReservedSendValue);
      EXPECT_EQ(sender.remaining_capacity(), 0u);

      sender.Disconnect();
      return Ready();
    });

    dispatcher.Post(reserved_sender_task);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletion();

    ExpectProducedWasConsumed({{kFirstSendValue, kReservedSendValue}},
                              {&receiver_task});
  }

  void StaticReserveSendReleasesSpaceWhenDropped() {
    auto channel = CreateMpmcChannel(storage);

    auto receiver_task = ReceiverTask(channel.CreateReceiver());
    auto channel_sender = channel.CreateSender();
    channel.Release();

    static constexpr int kFirstSendValue = 40;
    static constexpr int kSecondSendValue = 43;

    FuncTask reserved_sender_task([this, sender = std::move(channel_sender)](
                                      Context& cx) mutable -> Poll<> {
      // This task runs once without sleeping.
      // The channel has two slots. First, we reserve a slot through
      // `ReserveSend`, but don't commit a value. Then, we attempt to write
      // two values through the regular `Send` API. The first should succeed
      // and the second should block. Afterwards, we drop the reservation,
      // which should release the slot, allowing the second send to succeed.
      auto reserve_send_future = sender.ReserveSend();
      auto send_future_1 = Send(sender, kFirstSendValue);
      auto send_future_2 = Send(sender, kSecondSendValue);

      {
        PW_AWAIT(auto reservation, reserve_send_future, cx);
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

    ExpectProducedWasConsumed({{kFirstSendValue, kSecondSendValue}},
                              {&receiver_task});
  }

  void StaticReserveSendManualCancel() {
    auto channel = CreateMpmcChannel(storage);

    auto receiver_task = ReceiverTask(channel.CreateReceiver());
    auto channel_sender = channel.CreateSender();
    channel.Release();

    static constexpr int kFirstSendValue = 40;
    static constexpr int kSecondSendValue = 43;

    FuncTask reserved_sender_task([this, sender = std::move(channel_sender)](
                                      Context& cx) mutable -> Poll<> {
      // This task runs once without sleeping.
      // The channel has two slots. First, we reserve a slot through
      // `ReserveSend`, but don't commit a value. Then, we attempt to write
      // two values through the regular `Send` API. The first should succeed
      // and the second should block. Afterwards, we cancel the reservation,
      // which should release the slot, allowing the second send to succeed.
      auto reserve_send_future = sender.ReserveSend();
      auto send_future_1 = Send(sender, kFirstSendValue);
      auto send_future_2 = Send(sender, kSecondSendValue);

      PW_AWAIT(auto reservation, reserve_send_future, cx);
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

    ExpectProducedWasConsumed({{kFirstSendValue, kSecondSendValue}},
                              {&receiver_task});
  }

  void DynamicForwardsDataAndAutomaticallyDeallocates() {
    auto channel = CreateMpmcChannel<T>(alloc, 2);
    ASSERT_TRUE(channel.has_value());

    auto channel_sender = channel->CreateSender();
    auto receiver = channel->CreateReceiver();
    channel->Release();

    EXPECT_EQ(alloc.metrics().num_allocations.value(),
              IsDynamicChannelDequeAllocated() ? 2u : 1u);
    EXPECT_EQ(alloc.metrics().num_deallocations.value(), 0u);

    auto sender_task =
        SenderTask(std::move(channel_sender), {1, 2, 3, 4, 5, 6});
    auto receiver_task = ReceiverTask(std::move(receiver));

    dispatcher.Post(sender_task);
    dispatcher.Post(receiver_task);

    dispatcher.RunToCompletion();

    EXPECT_TRUE(sender_task.succeeded());
    ExpectProducedWasConsumed({{1, 2, 3, 4, 5, 6}}, {&receiver_task});

    EXPECT_EQ(alloc.metrics().allocated_bytes.value(), 0u);
    EXPECT_EQ(alloc.metrics().num_allocations.value(),
              IsDynamicChannelDequeAllocated() ? 2u : 1u);
    EXPECT_EQ(alloc.metrics().num_deallocations.value(),
              IsDynamicChannelDequeAllocated() ? 2u : 1u);
  }

  void DynamicRemainingCapacity() {
    auto channel = CreateMpmcChannel<T>(alloc, 2);
    ASSERT_TRUE(channel.has_value());

    auto sender = channel->CreateSender();

    EXPECT_EQ(sender.remaining_capacity(), 2u);
    EXPECT_EQ(sender.capacity(), 2u);

    PW_TEST_EXPECT_OK(TrySend(sender, 1));
    EXPECT_EQ(sender.remaining_capacity(), 1u);
    EXPECT_EQ(sender.capacity(), 2u);

    channel->Release();
  }

  void DynamicAllocationFailure() {
    pw::allocator::test::AllocatorForTest<64> exhausted_alloc;
    exhausted_alloc.Exhaust();
    auto channel = CreateMpmcChannel<T>(exhausted_alloc, 2);
    ASSERT_FALSE(channel.has_value());
    EXPECT_EQ(exhausted_alloc.metrics().allocated_bytes.value(), 0u);
    EXPECT_EQ(exhausted_alloc.metrics().num_allocations.value(), 0u);
    EXPECT_EQ(exhausted_alloc.metrics().num_deallocations.value(), 0u);

    // Enough space to allocate the deque, but not enough for the channel.
    // Deque should be allocated then deallocated.
    constexpr size_t kBaseDequeSize = sizeof(pw::FixedDeque<int, 0>);
    constexpr size_t kElementSize =
        sizeof(std::conditional_t<std::is_void_v<T>, int8_t, T>);
    constexpr size_t kDequeTotalSize = kBaseDequeSize + kElementSize * 2;
    pw::allocator::test::AllocatorForTest<kDequeTotalSize> deque_only_alloc;
    channel = CreateMpmcChannel<T>(deque_only_alloc, 2);
    ASSERT_FALSE(channel.has_value());
    EXPECT_EQ(deque_only_alloc.metrics().allocated_bytes.value(), 0u);
    EXPECT_EQ(deque_only_alloc.metrics().num_allocations.value(),
              IsDynamicChannelDequeAllocated() ? 1u : 0u);
    EXPECT_EQ(deque_only_alloc.metrics().num_deallocations.value(),
              IsDynamicChannelDequeAllocated() ? 1u : 0u);
  }

 private:
  DispatcherForTest dispatcher;
  pw::allocator::test::AllocatorForTest<1024> alloc;
};

using IntChannelTest = ChannelTest<int>;
using IntFunctionChannelTest = ChannelTest<IntFunction>;
using MoveOnlyIntChannelTest = ChannelTest<MoveOnlyInt>;
using NotificationChannelTest = ChannelTest<void>;

#define TEST_CHANNELS(test_name)                             \
  TEST_F(IntChannelTest, test_name) { test_name(); }         \
  TEST_F(IntFunctionChannelTest, test_name) { test_name(); } \
  TEST_F(MoveOnlyIntChannelTest, test_name) { test_name(); } \
  TEST_F(NotificationChannelTest, test_name) { test_name(); }

TEST_CHANNELS(StaticSingleProducerSingleConsumer)
TEST_CHANNELS(StaticMultiProducerSingleConsumer)
TEST_CHANNELS(StaticSingleProducerMultiConsumer)
TEST_CHANNELS(StaticMultiProducerMultiConsumer)
TEST_CHANNELS(StaticNonAsyncTrySend)
TEST_CHANNELS(StaticTryReserveSend)
TEST_CHANNELS(StaticTryReceive)
TEST_CHANNELS(StaticReceiverDisconnects)
TEST_CHANNELS(StaticReserveSend)
TEST_CHANNELS(StaticRemainingCapacity)
TEST_CHANNELS(StaticSendOnClosedReturnsFalse)
TEST_CHANNELS(StaticReceiveClosed)
TEST_CHANNELS(StaticTryReceiveClosedWithData)
TEST_CHANNELS(StaticReceiveClosedWithData)
TEST_CHANNELS(StaticCreateSenderWhenClosed)
TEST_CHANNELS(StaticCreateReceiverWhenClosed)
TEST_CHANNELS(StaticMoveFuture)
TEST_CHANNELS(StaticCanPollReceiveFuturesTwice)
TEST_CHANNELS(StaticCanPollSendFuturesTwice)
TEST_CHANNELS(StaticCanPollReserveSendFutureTwice)
TEST_CHANNELS(StaticReserveSendReservesSpace)
TEST_CHANNELS(StaticReserveSendReleasesSpaceWhenDropped)
TEST_CHANNELS(StaticReserveSendManualCancel)
TEST_CHANNELS(DynamicForwardsDataAndAutomaticallyDeallocates)
TEST_CHANNELS(DynamicRemainingCapacity)
TEST_CHANNELS(DynamicAllocationFailure)

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
