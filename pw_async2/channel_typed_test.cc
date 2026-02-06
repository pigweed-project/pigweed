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

#include <list>

#include "pw_allocator/testing.h"
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/try.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::async2::ChannelStorage;
using pw::async2::Context;
using pw::async2::CreateMpmcChannel;
using pw::async2::CreateMpscChannel;
using pw::async2::CreateSpmcChannel;
using pw::async2::CreateSpscChannel;
using pw::async2::DispatcherForTest;
using pw::async2::FuncTask;
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

// Test Description =========================
// ## Summary
// This test suite is intended to test the Async2 Channel API with a variety of
// types to ensure that the API correctly supports passing around different
// types.
//
// ## Types Tested:
// ### Fundamental Types:
// bool (Boolean)
// int8_t, int16_t, int32_t, int64_t (Integers)
// float, double (Floating Point)
// int* (Pointer)
//
// ### Composite Types:
// POD (Plain Old Data)
// Default Constructible
// Non-Default Constructible
// Non-Trivially Copyable (Shared Pointer)
// Non-Trivially Moveable (Unique Pointer)

// Composite Type Definitions =========================

// Plain Old Data (POD)
struct Pod {
  bool operator==(const Pod& other) const { return value_ == other.value_; }
  int value_;
};
static_assert(std::is_standard_layout_v<Pod>);
static_assert(std::is_trivially_default_constructible_v<Pod>);
static_assert(std::is_trivially_destructible_v<Pod>);
static_assert(std::is_trivially_copy_constructible_v<Pod>);
static_assert(std::is_trivially_copy_assignable_v<Pod>);
static_assert(std::is_trivially_move_constructible_v<Pod>);
static_assert(std::is_trivially_move_assignable_v<Pod>);

// Default Constructible
struct DefaultConstructible {
  DefaultConstructible() = default;
  ~DefaultConstructible() = default;
  DefaultConstructible(const DefaultConstructible&) = default;
  DefaultConstructible(DefaultConstructible&&) = default;
  DefaultConstructible& operator=(const DefaultConstructible&) = default;
  DefaultConstructible& operator=(DefaultConstructible&&) = default;

  bool operator==(const DefaultConstructible& other) const {
    return value_ == other.value_;
  }
  int value_;
};
static_assert(std::is_default_constructible_v<DefaultConstructible>);
static_assert(std::is_trivially_constructible_v<DefaultConstructible>);

// Non-Default Constructible
struct NonDefaultConstructible {
  NonDefaultConstructible() = delete;
  NonDefaultConstructible(int value) : value_(value) {}
  ~NonDefaultConstructible() = default;
  NonDefaultConstructible(const NonDefaultConstructible&) = default;
  NonDefaultConstructible(NonDefaultConstructible&&) = default;
  NonDefaultConstructible& operator=(const NonDefaultConstructible&) = default;
  NonDefaultConstructible& operator=(NonDefaultConstructible&&) = default;

  bool operator==(const NonDefaultConstructible& other) const {
    return value_ == other.value_;
  }
  int value_;
};
static_assert(!std::is_default_constructible_v<NonDefaultConstructible>);

// Helper Functions =========================

// Provides a single interface for generating test data of different types.
template <typename T>
T MakeT(int value) {
  if constexpr (std::is_fundamental_v<T>) {
    return static_cast<T>(value);
  } else if constexpr (std::is_pointer_v<T>) {
    return reinterpret_cast<T>(value);
  } else if constexpr (std::is_same_v<T, std::shared_ptr<int>>) {
    return std::make_shared<int>(value);
  } else if constexpr (std::is_same_v<T, std::unique_ptr<int>>) {
    return std::make_unique<int>(value);
  } else if constexpr (std::is_default_constructible_v<T>) {
    T t;
    t.value_ = value;
    return t;
  } else {
    T t(value);
    return t;
  }
}

// Provides a single interface for retrieving the value of different types.
template <typename T>
int GetValue(const T& t) {
  if constexpr (std::is_fundamental_v<T>) {
    return static_cast<int>(t);
  } else if constexpr (std::is_pointer_v<T>) {
    return static_cast<int>(reinterpret_cast<intptr_t>(t));
  } else if constexpr (std::is_same_v<T, std::shared_ptr<int>>) {
    return *t;
  } else if constexpr (std::is_same_v<T, std::unique_ptr<int>>) {
    return *t;
  } else {
    return t.value_;
  }
}

// Channel Task Definitions =========================
// Sender Task sends a sequence of values to a channel.
// Values sent the channel start at 'start' and increment by 1 each iteration up
// to 'count' times. If the channel is full, the task will pend until there is
// space available. Once the data is sent the task will disconnect from the
// channel and complete.
template <typename T>
class SenderTask : public Task {
 public:
  SenderTask(Sender<T> sender, int start, int count)
      : Task(PW_ASYNC_TASK_NAME("SenderTask")),
        sender_(std::move(sender)),
        current_value_(start),
        count_(count) {}

  bool succeeded() const { return success_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (count_ > 0) {
      if (!future_.is_pendable()) {
        future_ = sender_.Send(std::move(MakeT<T>(current_value_)));
      }

      PW_TRY_READY_ASSIGN(bool sent, future_.Pend(cx));
      if (!sent) {
        success_ = false;
        return Ready();
      }

      current_value_++;
      count_--;
    }

    success_ = true;
    sender_.Disconnect();
    return Ready();
  }

  Sender<T> sender_;
  SendFuture<T> future_;
  bool success_ = false;
  int current_value_;
  int count_;
};

// Receiver Task receives a set 'count' of values from a channel.
// Values received from the channel are stored in a vector.
// If the channel is empty, the task will pend until there is data available.
// Once the data is received the task will disconnect from the channel and
// complete.
template <typename T>
class ReceiverTask : public Task {
 public:
  explicit ReceiverTask(Receiver<T> receiver, size_t count)
      : Task(PW_ASYNC_TASK_NAME("ReceiverTask")),
        receiver_(std::move(receiver)),
        count_(count) {}

  std::list<T>& received() { return received_; }
  const std::list<T>& received() const { return received_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (count_ > 0) {
      if (!future_.is_pendable()) {
        future_ = receiver_.Receive();
      }

      PW_TRY_READY_ASSIGN(std::optional<T> value, future_.Pend(cx));
      if (!value.has_value()) {
        break;
      }

      received_.emplace_back(std::move(*value));
      count_--;
    }

    receiver_.Disconnect();
    return Ready();
  }

  Receiver<T> receiver_;
  ReceiveFuture<T> future_;
  std::list<T> received_;
  size_t count_;
};

// Test Setup =========================

// Template to test different channel types.
template <typename T>
class ChannelTypedTest : public ::testing::Test {};

// Define the types to be tested.
using ChannelTypes = ::testing::Types<bool,
                                      int8_t,
                                      int16_t,
                                      int32_t,
                                      int64_t,
                                      float,
                                      double,
                                      int*,
                                      Pod,
                                      DefaultConstructible,
                                      NonDefaultConstructible,
                                      std::shared_ptr<int>,
                                      std::unique_ptr<int>>;

// Generates a name for each type-parameterized test case, based on the type
// being tested.
class NameGenerator {
 public:
  template <typename T>
  static std::string GetName(int) {
    if constexpr (std::is_same_v<T, bool>) {
      return "bool";
    }
    if constexpr (std::is_same_v<T, int8_t>) {
      return "int8_t";
    }
    if constexpr (std::is_same_v<T, int16_t>) {
      return "int16_t";
    }
    if constexpr (std::is_same_v<T, int32_t>) {
      return "int32_t";
    }
    if constexpr (std::is_same_v<T, int64_t>) {
      return "int64_t";
    }
    if constexpr (std::is_same_v<T, float>) {
      return "float";
    }
    if constexpr (std::is_same_v<T, double>) {
      return "double";
    }
    if constexpr (std::is_same_v<T, int*>) {
      return "int*";
    }
    if constexpr (std::is_same_v<T, Pod>) {
      return "Pod";
    }
    if constexpr (std::is_same_v<T, DefaultConstructible>) {
      return "DefaultConstructible";
    }
    if constexpr (std::is_same_v<T, NonDefaultConstructible>) {
      return "NonDefaultConstructible";
    }
    if constexpr (std::is_same_v<T, std::shared_ptr<int>>) {
      return "std::shared_ptr<int>";
    }
    if constexpr (std::is_same_v<T, std::unique_ptr<int>>) {
      return "std::unique_ptr<int>";
    }
    return "Unknown";
  }
};

// Test Suite =========================

TYPED_TEST_SUITE(ChannelTypedTest, ChannelTypes, NameGenerator);

TYPED_TEST(ChannelTypedTest, SingleProducerSingleConsumer) {
  DispatcherForTest dispatcher;

  // Channel storage is not large enough to hold all values at once.
  ChannelStorage<TypeParam, 2> storage;
  auto [channel, sender, receiver] = CreateSpscChannel(storage);
  channel.Release();

  // Sender will send 5 values (0 - 4).
  SenderTask<TypeParam> sender_task(std::move(sender), 0, 5);
  // Receiver will receive 5 values (0 - 4).
  ReceiverTask<TypeParam> receiver_task(std::move(receiver), 5);

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  // Ensure sender was able to send all values.
  EXPECT_TRUE(sender_task.succeeded());

  // Ensure receiver received all values.
  std::list<TypeParam> expected;
  expected.emplace_back(std::move(MakeT<TypeParam>(0)));
  expected.emplace_back(std::move(MakeT<TypeParam>(1)));
  expected.emplace_back(std::move(MakeT<TypeParam>(2)));
  expected.emplace_back(std::move(MakeT<TypeParam>(3)));
  expected.emplace_back(std::move(MakeT<TypeParam>(4)));

  std::list<TypeParam> received{std::move(receiver_task.received())};
  ASSERT_EQ(received.size(), expected.size());

  // Check that all expected values were received (regardless of order).
  for (auto& exp : expected) {
    auto it = std::find_if(received.begin(), received.end(), [&exp](auto& val) {
      return GetValue(val) == GetValue(exp);
    });
    ASSERT_NE(it, received.end()) << "Expected: " << GetValue(exp);
    received.erase(it);
  }
}

TYPED_TEST(ChannelTypedTest, MultiProducerSingleConsumer) {
  DispatcherForTest dispatcher;

  // Channel storage is not large enough to hold all values at once.
  ChannelStorage<TypeParam, 2> storage;
  auto [channel, receiver] = CreateMpscChannel(storage);

  // Sender 1 will send 5 values (0-4).
  SenderTask<TypeParam> sender_task_1(channel.CreateSender(), 0, 5);
  // Sender 2 will send 5 values (5-9).
  SenderTask<TypeParam> sender_task_2(channel.CreateSender(), 5, 5);
  // Receiver will receive 10 values (no defined order).
  ReceiverTask<TypeParam> receiver_task(std::move(receiver), 10);
  channel.Release();

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task);

  dispatcher.RunToCompletion();

  // Ensure both senders were able to send all values.
  EXPECT_TRUE(sender_task_1.succeeded());
  EXPECT_TRUE(sender_task_2.succeeded());

  // Ensure receiver received all values.
  std::list<TypeParam> expected;
  expected.emplace_back(std::move(MakeT<TypeParam>(0)));
  expected.emplace_back(std::move(MakeT<TypeParam>(1)));
  expected.emplace_back(std::move(MakeT<TypeParam>(2)));
  expected.emplace_back(std::move(MakeT<TypeParam>(3)));
  expected.emplace_back(std::move(MakeT<TypeParam>(4)));
  expected.emplace_back(std::move(MakeT<TypeParam>(5)));
  expected.emplace_back(std::move(MakeT<TypeParam>(6)));
  expected.emplace_back(std::move(MakeT<TypeParam>(7)));
  expected.emplace_back(std::move(MakeT<TypeParam>(8)));
  expected.emplace_back(std::move(MakeT<TypeParam>(9)));

  std::list<TypeParam> received{std::move(receiver_task.received())};
  ASSERT_EQ(received.size(), expected.size());

  // Check that all expected values were received (regardless of order).
  for (auto& exp : expected) {
    auto it = std::find_if(received.begin(), received.end(), [&exp](auto& val) {
      return GetValue(val) == GetValue(exp);
    });
    ASSERT_NE(it, received.end()) << "Expected: " << GetValue(exp);
    received.erase(it);
  }
}

TYPED_TEST(ChannelTypedTest, SingleProducerMultiConsumer) {
  DispatcherForTest dispatcher;

  // Channel storage is not large enough to hold all values at once.
  ChannelStorage<TypeParam, 2> storage;
  auto [channel, sender] = CreateSpmcChannel(storage);

  // Sender will send 10 values (0 - 9).
  SenderTask<TypeParam> sender_task(std::move(sender), 0, 10);
  // Receiver 1 will receive 5 values (no defined order).
  ReceiverTask<TypeParam> receiver_task_1(channel.CreateReceiver(), 5);
  // Receiver 2 will receive 5 values (no defined order).
  ReceiverTask<TypeParam> receiver_task_2(channel.CreateReceiver(), 5);
  channel.Release();

  dispatcher.Post(sender_task);
  dispatcher.Post(receiver_task_1);
  dispatcher.Post(receiver_task_2);

  dispatcher.RunToCompletion();

  // Ensure sender was able to send all values.
  EXPECT_TRUE(sender_task.succeeded());

  // Ensure all expected values were received.
  std::list<TypeParam> expected;
  expected.emplace_back(std::move(MakeT<TypeParam>(0)));
  expected.emplace_back(std::move(MakeT<TypeParam>(1)));
  expected.emplace_back(std::move(MakeT<TypeParam>(2)));
  expected.emplace_back(std::move(MakeT<TypeParam>(3)));
  expected.emplace_back(std::move(MakeT<TypeParam>(4)));
  expected.emplace_back(std::move(MakeT<TypeParam>(5)));
  expected.emplace_back(std::move(MakeT<TypeParam>(6)));
  expected.emplace_back(std::move(MakeT<TypeParam>(7)));
  expected.emplace_back(std::move(MakeT<TypeParam>(8)));
  expected.emplace_back(std::move(MakeT<TypeParam>(9)));

  // Merge received values from both receivers.
  std::list<TypeParam> received{std::move(receiver_task_1.received())};
  for (auto& val : receiver_task_2.received()) {
    received.push_back(std::move(val));
  }

  // Check that all expected values were received (regardless of order).
  ASSERT_EQ(received.size(), expected.size());
  for (auto& exp : expected) {
    auto it = std::find_if(received.begin(), received.end(), [&exp](auto& val) {
      return GetValue(val) == GetValue(exp);
    });
    ASSERT_NE(it, received.end()) << "Expected: " << GetValue(exp);
    received.erase(it);
  }
}

TYPED_TEST(ChannelTypedTest, MultiProducerMultiConsumer) {
  DispatcherForTest dispatcher;
  ChannelStorage<TypeParam, 2> storage;
  auto channel = CreateMpmcChannel(storage);

  // Sender 1 will send 5 values (0 - 4).
  SenderTask<TypeParam> sender_task_1(channel.CreateSender(), 0, 5);
  // Sender 2 will send 5 values (5 - 9).
  SenderTask<TypeParam> sender_task_2(channel.CreateSender(), 5, 5);
  // Receiver 1 will receive 5 values (no defined order).
  ReceiverTask<TypeParam> receiver_task_1(channel.CreateReceiver(), 5);
  // Receiver 2 will receive 5 values (no defined order).
  ReceiverTask<TypeParam> receiver_task_2(channel.CreateReceiver(), 5);
  channel.Release();

  dispatcher.Post(sender_task_1);
  dispatcher.Post(sender_task_2);
  dispatcher.Post(receiver_task_1);
  dispatcher.Post(receiver_task_2);

  dispatcher.RunToCompletion();

  // Ensure both senders were able to send all values.
  EXPECT_TRUE(sender_task_1.succeeded());
  EXPECT_TRUE(sender_task_2.succeeded());

  // Ensure all expected values were received.
  std::list<TypeParam> expected;
  expected.emplace_back(std::move(MakeT<TypeParam>(0)));
  expected.emplace_back(std::move(MakeT<TypeParam>(1)));
  expected.emplace_back(std::move(MakeT<TypeParam>(2)));
  expected.emplace_back(std::move(MakeT<TypeParam>(3)));
  expected.emplace_back(std::move(MakeT<TypeParam>(4)));
  expected.emplace_back(std::move(MakeT<TypeParam>(5)));
  expected.emplace_back(std::move(MakeT<TypeParam>(6)));
  expected.emplace_back(std::move(MakeT<TypeParam>(7)));
  expected.emplace_back(std::move(MakeT<TypeParam>(8)));
  expected.emplace_back(std::move(MakeT<TypeParam>(9)));

  // Merge received values from both receivers.
  std::list<TypeParam> received{std::move(receiver_task_1.received())};
  for (auto& val : receiver_task_2.received()) {
    received.push_back(std::move(val));
  }

  // Check that all expected values were received (regardless of order).
  ASSERT_EQ(received.size(), expected.size());
  for (auto& exp : expected) {
    auto it = std::find_if(received.begin(), received.end(), [&exp](auto& val) {
      return GetValue(val) == GetValue(exp);
    });
    ASSERT_NE(it, received.end()) << "Expected: " << GetValue(exp);
    received.erase(it);
  }
}

}  // namespace
