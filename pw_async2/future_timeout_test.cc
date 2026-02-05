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

#include "pw_async2/future_timeout.h"

#include <chrono>  // IWYU pragma: keep
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/simulated_time_provider.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_chrono/system_clock.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_function/function.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using namespace std::chrono_literals;

using pw::Function;
using pw::Result;
using pw::Status;
using pw::async2::BroadcastValueProvider;
using pw::async2::ChannelStorage;
using pw::async2::Context;
using pw::async2::CreateSpscChannel;
using pw::async2::DispatcherForTest;
using pw::async2::FuncTask;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::ReadyType;
using pw::async2::ReceiveFuture;
using pw::async2::ReceiveFutureWithTimeout;
using pw::async2::ReceiveFutureWithTimeoutOrClosed;
using pw::async2::Receiver;
using pw::async2::ReserveSendFutureWithTimeout;
using pw::async2::ReserveSendFutureWithTimeoutOrClosed;
using pw::async2::Sender;
using pw::async2::SendFutureWithTimeout;
using pw::async2::SendFutureWithTimeoutOrClosed;
using pw::async2::SendReservation;
using pw::async2::SimulatedTimeProvider;
using pw::chrono::SystemClock;

template <typename FutureType, typename ProviderFunction>
typename FutureType::value_type DoDispatchCommon(
    FutureType future, ProviderFunction&& provider_function) {
  DispatcherForTest dispatcher;

  typename FutureType::value_type result{};
  FuncTask task([&result, pend_future = std::move(future)](
                    Context& cx) mutable -> Poll<> {
    PW_TRY_READY_ASSIGN(result, pend_future.Pend(cx));
    return Ready();
  });

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());
  std::forward<ProviderFunction>(provider_function)();
  EXPECT_FALSE(dispatcher.RunUntilStalled());
  return result;
}

TEST(FutureTimeout, IntValueTimeoutOrResolvesWithoutTimeoutRealClock) {
  // Since we are intentionally using the default system clock for this test...
  // One hundred hours shouldn't cause trouble or flakiness, when we don't
  // expect a timeout, and we are resolving the future manually almost
  // immediately.
  constexpr auto large_timeout = 100h;

  BroadcastValueProvider<int> value_provider;
  auto future = TimeoutOr(value_provider.Get(), large_timeout, 9999);
  static_assert(pw::async2::Future<decltype(future)>);

  const int result =
      DoDispatchCommon(std::move(future), [&] { value_provider.Resolve(27); });
  EXPECT_EQ(result, 27);
}

TEST(FutureTimeout, DefaultConstructAndAssign) {
  BroadcastValueProvider<int> value_provider;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto future =
      TimeoutOr(value_provider.Get(), time_provider, 30s, [] { return 9999; });

  decltype(future) other;
  EXPECT_FALSE(other.is_pendable());
  EXPECT_FALSE(other.is_complete());

  other = std::move(future);

  EXPECT_FALSE(future.is_pendable());  // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(future.is_complete());  // NOLINT(bugprone-use-after-move)

  EXPECT_TRUE(other.is_pendable());
  EXPECT_FALSE(other.is_complete());
}

TEST(FutureTimeout, IntValueTimeoutOrFunctionReturnResolvesOnTimeout) {
  BroadcastValueProvider<int> value_provider;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto future =
      TimeoutOr(value_provider.Get(), time_provider, 30s, [] { return 9999; });

  const int result = DoDispatchCommon(std::move(future),
                                      [&] { time_provider.AdvanceTime(31s); });

  EXPECT_EQ(result, 9999);
}

TEST(FutureTimeout, IntValueTimeoutOrRuntimeValueResolvesOnTimeout) {
  BroadcastValueProvider<int> value_provider;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto future = TimeoutOr(value_provider.Get(), time_provider, 30s, 8888);

  const int result = DoDispatchCommon(std::move(future),
                                      [&] { time_provider.AdvanceTime(31s); });

  EXPECT_EQ(result, 8888);
}

TEST(FutureTimeout, IntValueTimeoutOrCompileTimeConstantResolvesOnTimeout) {
  BroadcastValueProvider<int> value_provider;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto future = TimeoutOr(value_provider.Get(),
                          time_provider,
                          30s,
                          std::integral_constant<int, 7777>());

  const int result = DoDispatchCommon(std::move(future),
                                      [&] { time_provider.AdvanceTime(31s); });

  EXPECT_EQ(result, 7777);
}

TEST(FutureTimeout, IntValueTimeoutResolvesToDeadlineExceededOnTimeout) {
  BroadcastValueProvider<int> value_provider;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto future = Timeout(value_provider.Get(), time_provider, 30s);

  const Result<int> result = DoDispatchCommon(
      std::move(future), [&] { time_provider.AdvanceTime(31s); });

  EXPECT_TRUE(result.status().IsDeadlineExceeded());
}

TEST(FutureTimeout, IntValueTimeoutResolvesToProviderValueOnRace) {
  BroadcastValueProvider<int> value_provider;
  SimulatedTimeProvider<SystemClock> time_provider;

  auto future = TimeoutOr(value_provider.Get(), time_provider, 30s, 9999);

  const int result = DoDispatchCommon(std::move(future), [&] {
    value_provider.Resolve(27);
    time_provider.AdvanceTime(31s);
  });

  EXPECT_EQ(result, 27);
}

[[maybe_unused]] void CannotConstructTimeoutOrWithVoidValueFuture() {
#if PW_NC_TEST(TestCannotConstructTimeoutOrWithVoidValueFuture)
  PW_NC_EXPECT("ValueFuture<void> cannot be used with TimeoutOr");
  BroadcastValueProvider<void> void_provider;
  SimulatedTimeProvider<SystemClock> time_provider;
  [[maybe_unused]] TimeoutOr(void_provider.Get(), time_provider, 30s);
#endif  // PW_NC_TEST
}

TEST(FutureTimeout, VoidValueTimeoutResolvesWithoutTimeout) {
  // If a `void` future is resolved before timing out, it resolves as completed.
  BroadcastValueProvider<void> void_provider;
  SimulatedTimeProvider<SystemClock> time_provider;

  auto future = Timeout(void_provider.Get(), time_provider, 30s);

  [[maybe_unused]] const Result<ReadyType> result =
      DoDispatchCommon(std::move(future), [&] { void_provider.Resolve(); });

  EXPECT_TRUE(result.status().ok());
}

TEST(FutureTimeout, VoidValueTimeoutResolvesToDeadlineExceededOnTimeout) {
  // If a `void` future is set to time out with an error, and it times out, the
  // error is DEADLINE_EXCEEDED.
  BroadcastValueProvider<void> void_provider;
  SimulatedTimeProvider<SystemClock> time_provider;

  auto future = Timeout(void_provider.Get(), time_provider, 30s);

  const Result<ReadyType> result = DoDispatchCommon(
      std::move(future), [&] { time_provider.AdvanceTime(31s); });

  EXPECT_TRUE(result.status().IsDeadlineExceeded());
}

template <typename T>
void DrainChannelToAvoidAssertOnDestruction(Receiver<T>&& channel_receiver) {
  // Needed as the channel asserts if there is any data in it on destruction!
  DispatcherForTest dispatcher;
  std::optional<ReceiveFuture<T>> receive_future;
  auto drain_task =
      FuncTask([&receive_future, receiver = std::move(channel_receiver)](
                   Context& cx) mutable -> Poll<> {
        while (true) {
          if (!receive_future.has_value()) {
            receive_future = receiver.Receive();
          }

          PW_TRY_READY_ASSIGN([[maybe_unused]] const auto result,
                              receive_future->Pend(cx));
          if (!result) {
            return Ready();
          }
          receive_future.reset();
        }
      });

  dispatcher.Post(drain_task);
  dispatcher.RunUntilStalled();
}

TEST(FutureTimeout, SendFutureTimeoutOrClosedResolvesToClosedOnTimeout) {
  ChannelStorage<int, 2> storage;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto channel_result = CreateSpscChannel(storage);
  auto handle = std::move(std::get<0>(channel_result));
  auto sender = std::move(std::get<1>(channel_result));
  auto receiver = std::move(std::get<2>(channel_result));
  handle.Release();

  std::optional<SendFutureWithTimeoutOrClosed<int>> send_future;
  int send_count = 0;
  FuncTask send_task([&](Context& cx) -> Poll<> {
    while (true) {
      if (!send_future.has_value()) {
        send_future =
            TimeoutOrClosed(sender.Send(send_count), time_provider, 30s);
      }

      PW_TRY_READY_ASSIGN(const bool sent, send_future->Pend(cx));
      if (!sent) {
        return Ready();
      }

      send_count += 1;
      send_future.reset();
    }
  });

  DispatcherForTest dispatcher;
  dispatcher.Post(send_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);
  time_provider.AdvanceTime(31s);

  // Ends due to timeout.
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);

  DrainChannelToAvoidAssertOnDestruction(std::move(receiver));
}

TEST(FutureTimeout, SendFutureTimeoutResolvesToDeadlineExceededOnTimeout) {
  ChannelStorage<int, 2> storage;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto channel_result = CreateSpscChannel(storage);
  auto handle = std::move(std::get<0>(channel_result));
  auto sender = std::move(std::get<1>(channel_result));
  auto receiver = std::move(std::get<2>(channel_result));
  handle.Release();

  std::optional<SendFutureWithTimeout<int>> send_future;
  int send_count = 0;
  Status send_status = {};
  FuncTask send_task([&](Context& cx) -> Poll<> {
    while (true) {
      if (!send_future.has_value()) {
        send_future = Timeout(sender.Send(send_count), time_provider, 30s);
      }

      PW_TRY_READY_ASSIGN(const Result<bool> result, send_future->Pend(cx));
      if (!result.ok()) {
        send_status = result.status();
        return Ready();
      }

      if (!*result) {
        send_status = Status{};
        return Ready();
      }

      send_count += 1;
      send_future.reset();
    }
  });

  DispatcherForTest dispatcher;
  dispatcher.Post(send_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);
  EXPECT_TRUE(send_status.ok());

  time_provider.AdvanceTime(31s);
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);
  EXPECT_TRUE(send_status.IsDeadlineExceeded());

  DrainChannelToAvoidAssertOnDestruction(std::move(receiver));
}

TEST(FutureTimeout, ReserveSendFutureTimeoutOrClosedResolvesToClosedOnTimeout) {
  ChannelStorage<int, 2> storage;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto channel_result = CreateSpscChannel(storage);
  auto handle = std::move(std::get<0>(channel_result));
  auto sender = std::move(std::get<1>(channel_result));
  auto receiver = std::move(std::get<2>(channel_result));
  handle.Release();

  std::optional<ReserveSendFutureWithTimeoutOrClosed<int>> send_reservation;
  int send_count = 0;
  FuncTask send_task([&](Context& cx) -> Poll<> {
    while (true) {
      if (!send_reservation.has_value()) {
        send_reservation =
            TimeoutOrClosed(sender.ReserveSend(), time_provider, 30s);
      }

      PW_TRY_READY_ASSIGN(std::optional<SendReservation<int>> result,
                          send_reservation->Pend(cx));
      if (!result.has_value()) {
        return Ready();
      }

      result->Commit(send_count);
      send_count += 1;
      send_reservation.reset();
    }
  });

  DispatcherForTest dispatcher;
  dispatcher.Post(send_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);
  time_provider.AdvanceTime(31s);

  // Ends due to timeout.
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);

  DrainChannelToAvoidAssertOnDestruction(std::move(receiver));
}

TEST(FutureTimeout,
     ReserveSendFutureTimeoutOrClosedResolvesToDeadlineExceededOnTimeout) {
  ChannelStorage<int, 2> storage;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto channel_result = CreateSpscChannel(storage);
  auto handle = std::move(std::get<0>(channel_result));
  auto sender = std::move(std::get<1>(channel_result));
  auto receiver = std::move(std::get<2>(channel_result));
  handle.Release();

  std::optional<ReserveSendFutureWithTimeout<int>> send_reservation;
  int send_count = 0;
  Status send_status = {};
  FuncTask send_task([&](Context& cx) -> Poll<> {
    while (true) {
      if (!send_reservation.has_value()) {
        send_reservation = Timeout(sender.ReserveSend(), time_provider, 30s);
      }

      PW_TRY_READY_ASSIGN(Result<std::optional<SendReservation<int>>> result,
                          send_reservation->Pend(cx));

      if (!result.ok()) {
        send_status = result.status();
        return Ready();
      }

      if (!*result) {
        send_status = Status{};
        return Ready();
      }

      (*result)->Commit(send_count);
      send_count += 1;
      send_reservation.reset();
    }
  });

  DispatcherForTest dispatcher;
  dispatcher.Post(send_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);
  EXPECT_TRUE(send_status.ok());

  time_provider.AdvanceTime(31s);
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_EQ(send_count, 2);
  EXPECT_TRUE(send_status.IsDeadlineExceeded());

  DrainChannelToAvoidAssertOnDestruction(std::move(receiver));
}

TEST(FutureTimeout, ReceiveFutureTimeoutOrClosedResolvesToClosedOnTimeout) {
  ChannelStorage<int, 2> storage;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto channel_result = CreateSpscChannel(storage);
  auto handle = std::move(std::get<0>(channel_result));
  auto sender = std::move(std::get<1>(channel_result));
  auto receiver = std::move(std::get<2>(channel_result));
  handle.Release();

  // Load up the queue
  ASSERT_TRUE(sender.TrySend(1).ok());
  ASSERT_TRUE(sender.TrySend(2).ok());

  std::optional<ReceiveFutureWithTimeoutOrClosed<int>> receive_future;
  int receive_count = 0;
  FuncTask receive_task([&](Context& cx) -> Poll<> {
    while (true) {
      if (!receive_future.has_value()) {
        receive_future =
            TimeoutOrClosed(receiver.Receive(), time_provider, 30s);
      }

      PW_TRY_READY_ASSIGN(const std::optional<int> result,
                          receive_future->Pend(cx));
      if (!result) {
        return Ready();
      }

      receive_count += 1;
      receive_future.reset();
    }
  });

  DispatcherForTest dispatcher;
  dispatcher.Post(receive_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_EQ(receive_count, 2);

  time_provider.AdvanceTime(31s);

  // Ends due to timeout.
  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_EQ(receive_count, 2);

  DrainChannelToAvoidAssertOnDestruction(std::move(receiver));
}

TEST(FutureTimeout, ReceiveFutureTimeoutResolvesToDeadlineExceededOnTimeout) {
  ChannelStorage<int, 2> storage;
  SimulatedTimeProvider<SystemClock> time_provider;
  auto channel_result = CreateSpscChannel(storage);
  auto handle = std::move(std::get<0>(channel_result));
  auto sender = std::move(std::get<1>(channel_result));
  auto receiver = std::move(std::get<2>(channel_result));
  handle.Release();

  // Load up the queue
  ASSERT_TRUE(sender.TrySend(1).ok());
  ASSERT_TRUE(sender.TrySend(2).ok());

  std::optional<ReceiveFutureWithTimeout<int>> receive_future;
  int receive_count = 0;
  Status receive_status = {};
  FuncTask receive_task([&](Context& cx) -> Poll<> {
    while (true) {
      if (!receive_future.has_value()) {
        receive_future = Timeout(receiver.Receive(), time_provider, 30s);
      }

      PW_TRY_READY_ASSIGN(const Result<std::optional<int>> result,
                          receive_future->Pend(cx));

      if (!result.ok()) {
        receive_status = result.status();
        return Ready();
      }

      if (!*result) {
        receive_status = Status{};
        return Ready();
      }

      receive_count += 1;
      receive_future.reset();
    }
  });

  DispatcherForTest dispatcher;
  dispatcher.Post(receive_task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());

  EXPECT_EQ(receive_count, 2);
  EXPECT_TRUE(receive_status.ok());

  time_provider.AdvanceTime(31s);

  EXPECT_FALSE(dispatcher.RunUntilStalled());

  EXPECT_EQ(receive_count, 2);
  EXPECT_TRUE(receive_status.IsDeadlineExceeded());

  DrainChannelToAvoidAssertOnDestruction(std::move(receiver));
}

}  // namespace
