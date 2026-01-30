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
#pragma once

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/type_traits.h"
#include "pw_assert/assert.h"
#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/system_time_provider.h"
#include "pw_async2/time_provider.h"
#include "pw_async2/value_future.h"
#include "pw_chrono/system_clock.h"
#include "pw_function/function.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"

namespace pw::async2 {

/// @submodule{pw_async2,time}

/// When used as the `TimeoutResolution` type parameter, causes the
/// `FutureWithTimeout` wrapper to return `Ready()` when there is a timeout.
struct EmptyReadyResolution {
  using value_type = ReadyType;
  constexpr Poll<> operator()() const { return Ready(); }
};

/// When used as the `TimeoutResolution` type parameter, causes the
/// `FutureWithTimeout` wrapper to return `Ready(ready_value_fn())` when there
/// is a timeout.
///
/// This is useful when the type is large and shouldn't be stored, or if it
/// should be constructed on demand.
template <typename T>
struct ReadyFunctionResultResolution {
  using value_type = T;

  /// Cannot be invoked when default constructed; will crash.
  constexpr ReadyFunctionResultResolution() = default;

  constexpr explicit ReadyFunctionResultResolution(
      pw::Function<T()> ready_value_fn)
      : ready_(std::move(ready_value_fn)) {}

  constexpr Poll<T> operator()() const {
    return Ready<T>(std::in_place_t{}, ready_());
  }

 private:
  pw::Function<T()> ready_;
};

/// When used as the `TimeoutResolution` type parameter, causes the
/// `FutureWithTimeout` wrapper to return `Ready(constant)` when there is a
/// timeout.
///
/// The constant value is stored when in this type when constructed, and
/// adds to the total size of the `FutureWithTimeout`, so prefer to use
/// this when the type is reasonably small. If it is larger, or you want
/// on-demand construction, consider using `ReadyFunctionResultResolution`
/// instead.
template <typename T, typename ConstantType, typename CastType = T>
struct ReadyConstantValueResolution {
  using value_type = T;
  constexpr ReadyConstantValueResolution() = default;
  constexpr explicit ReadyConstantValueResolution(
      const cpp20::remove_cvref_t<ConstantType>& value)
      : constant_(value) {}
  constexpr explicit ReadyConstantValueResolution(
      cpp20::remove_cvref_t<ConstantType>&& value)
      : constant_(std::move(value)) {}

  constexpr Poll<T> operator()() const {
    return Ready<T>(std::in_place_t{}, CastType(constant_));
  }

 private:
  cpp20::remove_cvref_t<ConstantType> constant_;
};

/// When used as the `TimeoutResolution` type parameter, causes the
/// `FutureWithTimeout` wrapper to return a `Status.DeadlineExceeded()` value
/// when there is a timeout. Usually this is is used in a way that modifies
/// the `Poll<U>` type to instead use `Poll<Result<T>>`, so that the error state
/// is distinct from a non-timeout value.
template <typename T>
using DeadlineExceededResolution = ReadyConstantValueResolution<
    T,
    std::integral_constant<pw::Status::Code, PW_STATUS_DEADLINE_EXCEEDED>,
    pw::Status>;

/// While this class is public, it should typically be constructed using the
/// factory functions defined later, as those handle appropriate selection of
/// the `TimeoutFuture` and `TimeoutResolution` type parameters based on the
/// `PrimaryFuture` type and timeout value you use in constructing a
/// `FutureWithTimeout`.
///
/// Combines a primary (value) future with a secondary (timeout) future. This
/// type is itself a future.
///
/// Invoking the `Pend()` member function invokes `Pend()` on both the primary
/// and secondary futures.
///
/// When pended, the primary future is checked first. If that result is
/// `Ready(T)`, then that `Ready` value is returned from the
/// `FutureWithTimeout` wrapper's `Pend()`.
///
/// If the primary future is `Pending`, then the secondary timeout future is
/// checked. If that is `Ready`, then the `TimeoutResolution` policy class type
/// argument is used to decide what value should be returned.
template <
    typename T,
    typename PrimaryFuture,
    typename TimeoutFuture,
    typename TimeoutResolution,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<PrimaryFuture>>,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<TimeoutFuture>>,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<TimeoutResolution>>>
class [[nodiscard]] FutureWithTimeout {
  static_assert(
      Future<PrimaryFuture>,
      "FutureWithTimeout can only be used when PrimaryFuture is a Future type");
  static_assert(
      Future<TimeoutFuture>,
      "FutureWithTimeout can only be used when TimeoutFuture is a Future type");

 public:
  using value_type = T;

  constexpr FutureWithTimeout() = default;

  FutureWithTimeout(PrimaryFuture&& primary_future,
                    TimeoutFuture&& timeout_future,
                    TimeoutResolution&& timeout_resolution)
      : primary_future_(std::move(primary_future)),
        timeout_future_(std::move(timeout_future)),
        timeout_resolution_(std::move(timeout_resolution)),
        state_(FutureState::kPending) {}

  [[nodiscard]] constexpr bool is_pendable() const {
    return state_.is_pendable();
  }

  [[nodiscard]] constexpr bool is_complete() const {
    return state_.is_complete();
  }

  Poll<value_type> Pend(Context& cx) {
    PW_ASSERT(is_pendable());
    auto result = primary_future_.Pend(cx);
    if (result.IsReady()) {
      state_.MarkComplete();
      return Ready<value_type>(std::in_place_t{}, std::move(result).value());
    }

    if (timeout_future_.Pend(cx).IsReady()) {
      state_.MarkComplete();
      return timeout_resolution_();
    }

    return Pending();
  }

 private:
  PrimaryFuture primary_future_;
  TimeoutFuture timeout_future_;
  PW_NO_UNIQUE_ADDRESS TimeoutResolution timeout_resolution_;
  FutureState state_;
};

/// Helper class to construct a `FutureWithTimeout` instance given the value
/// type `T` along with the types and values which are the components for that
/// future.
///
/// It effectively acts as a class template deduction guide, where all the type
/// parameters are deduced except for the first `T` type parameter. This sort
/// of partial deduction is not possible with CTAD.
template <typename T,
          typename PrimaryFuture,
          typename TimeoutFuture,
          typename TimeoutResolution>
auto CreateFutureWithTimeout(PrimaryFuture&& primary_future,
                             TimeoutFuture&& timeout_future,
                             TimeoutResolution&& timeout_resolution)
    -> FutureWithTimeout<T,
                         std::decay_t<PrimaryFuture>,
                         std::decay_t<TimeoutFuture>,
                         std::decay_t<TimeoutResolution>> {
  return FutureWithTimeout<T,
                           std::decay_t<PrimaryFuture>,
                           std::decay_t<TimeoutFuture>,
                           std::decay_t<TimeoutResolution>>(
      std::forward<PrimaryFuture>(primary_future),
      std::forward<TimeoutFuture>(timeout_future),
      std::forward<TimeoutResolution>(timeout_resolution));
}

/// Creates a future, resolving to a `pw::Result<T>` wrapping an existing
/// future that resolves to a `T`.
///
/// If the given future completes before the timeout, the Result-wrapped value
/// is returned. If however the timeout occurs, the returned status matches
/// `status.IsDeadlineExceed()`.
///
/// The timeout is based on the given time provider, using whatever clock
/// source backs it. The timeout period begins when this call is made, and ends
/// when the time provider decides that the given delay has passed.
template <
    typename PrimaryFuture,
    typename TimeProvider,
    typename Duration,
    int&... kExplicitGuard,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<PrimaryFuture>>>
auto Timeout(PrimaryFuture&& primary_future,
             TimeProvider& time_provider,
             Duration delay) {
  using ResultType = Result<typename std::decay_t<PrimaryFuture>::value_type>;
  return CreateFutureWithTimeout<ResultType>(
      std::forward<PrimaryFuture>(primary_future),
      time_provider.WaitFor(delay),
      DeadlineExceededResolution<ResultType>());
}

/// Creates a future, resolving to a `pw::Result<T>` wrapping an existing
/// future that resolves to a `T`.
///
/// If the given future completes before the timeout, the Result-wrapped value
/// is returned. If however the timeout occurs, the returned status matches
/// `status.IsDeadlineExceed()`.
///
/// This overload uses `GetSystemTimeProvider()` to get the system clock time
/// provider for the timeouts. The timeout period begins when this call is
/// made, and ends when the given amount of time has passed according to the
/// system clock.
template <
    typename PrimaryFuture,
    int&... kExplicitGuard,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<PrimaryFuture>>>
auto Timeout(PrimaryFuture&& primary_future,
             typename chrono::SystemClock::duration delay) {
  using ResultType = Result<typename std::decay_t<PrimaryFuture>::value_type>;
  return CreateFutureWithTimeout<ResultType>(
      std::forward<PrimaryFuture>(primary_future),
      GetSystemTimeProvider().WaitFor(delay),
      DeadlineExceededResolution<ResultType>());
}

/// Constructs a `FutureWithTimeout` for a `ValueFuture<void>` as the primary
/// future, a timeout future obtained from a `TimeProvider<Clock>` instance,
/// and the timeout delay.
///
/// On timeout, `Ready()` is returned from the composed future's `Pend()`.
///
/// Note however that this is indistinguishable from the `Ready()` returned
/// from the `ValueFuture<void>` when it resolves without there being a timeout.
///
/// Strongly prefer instead to use `TimeoutFutureWithDeadlineExceededAfter`, as
/// that gives you a `Status.DeadlineExceeded()` result instead.
template <typename Clock>
void TimeoutOr([[maybe_unused]] ValueFuture<void>&& future,
               [[maybe_unused]] TimeProvider<Clock>& time_provider,
               [[maybe_unused]] typename Clock::duration delay) {
  static_assert(false, "ValueFuture<void> cannot be used with TimeoutOr");
}

/// Constructs a `FutureWithTimeout` for a `ValueFuture<T>` as the primary
/// future, a timeout future obtained from a `TimeProvider<Clock>` instance,
/// and the timeout delay.
///
/// On timeout, the `value_or_fn_on_timeout` function is used to obtain a
/// sentinel value to return as the `Ready(T)` from the composed future's
/// `Pend()`, either by invoking it with no arguments if it is a function,
/// or by converting it to the `T` type.
///
/// Note that sentinel values can be problematic. You should generally prefer
/// to use the non-sentinel call `Timeout(...)` which results in a value
/// with a status code you can check.
template <typename PrimaryFuture, typename Clock, typename U>
auto TimeoutOr(PrimaryFuture&& primary_future,
               TimeProvider<Clock>& time_provider,
               typename Clock::duration delay,
               U&& value_or_fn_on_timeout) {
  using value_type = typename PrimaryFuture::value_type;

  static_assert(
      std::is_invocable_v<U> || std::is_convertible_v<U, value_type>,
      "value_or_fn_on_timeout (U) must be callable, or convert to the "
      "value_type of the primary_future");

  if constexpr (std::is_invocable_v<U>) {
    return CreateFutureWithTimeout<value_type>(
        std::forward<PrimaryFuture>(primary_future),
        time_provider.WaitFor(delay),
        ReadyFunctionResultResolution<value_type>(
            std::forward<U>(value_or_fn_on_timeout)));
  } else if constexpr (std::is_convertible_v<U, value_type>) {
    return CreateFutureWithTimeout<value_type>(
        std::forward<PrimaryFuture>(primary_future),
        time_provider.WaitFor(delay),
        ReadyConstantValueResolution<value_type, U>(
            std::forward<U>(value_or_fn_on_timeout)));
  }
}

/// Helper function for using one of the other `TimeoutOr` overloads which take
/// a TimeProvider.
///
/// This overload uses the system time provider returned by the call to
/// `GetSystemTimeProvider()`, and forwards it as the time provider along with
/// the other arguments to the other overloads. See those other overloads to
/// understand what type of future you will construct.
template <
    typename PrimaryFuture,
    typename... Args,
    int&... kExplicitGuard,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<PrimaryFuture>>>
auto TimeoutOr(PrimaryFuture&& primary_future,
               typename chrono::SystemClock::duration delay,
               Args&&... args) {
  return TimeoutOr<typename std::decay_t<PrimaryFuture>>(
      std::forward<PrimaryFuture>(primary_future),
      GetSystemTimeProvider(),
      delay,
      std::forward<Args>(args)...);
}

/// Constructs a `FutureWithTimeout` for a `ReceiveFuture<T>` as the primary
/// future, a timeout future obtained from a `TimeProvider<Clock>` instance,
/// and the timeout delay.
///
/// On timeout, the an empty `std::optional<T>` value is returned as the
/// `Ready(T)` from the composed future's `Pend()`.
///
/// Note that this return value means that the timeout return value is not
/// distinguishable from the result when the channel is closed. If you prefer
/// a distinct status value, use `Timeout()`.
template <typename T, typename Clock>
auto TimeoutOrClosed(ReceiveFuture<T>&& future,
                     TimeProvider<Clock>& time_provider,
                     typename Clock::duration delay) {
  return CreateFutureWithTimeout<std::optional<T>>(
      std::move(future),
      time_provider.WaitFor(delay),
      ReadyConstantValueResolution<std::optional<T>, std::nullopt_t>(
          std::nullopt));
}

/// Constructs a `FutureWithTimeout` for a `SendFuture<T>` as the primary
/// future, a timeout future obtained from a `TimeProvider<Clock>` instance,
/// and the timeout delay.
///
/// On timeout, a boolean `false` value is returned as the `Ready(T)` from the
/// composed future's `Pend()`.
///
/// Note that this return value means that the timeout return value is not
/// distinguishable from the result when the channel is closed. If you prefer
/// a distinct status value, use `Timeout()`.
template <typename T, typename Clock>
auto TimeoutOrClosed(SendFuture<T>&& future,
                     TimeProvider<Clock>& time_provider,
                     typename Clock::duration delay) {
  return CreateFutureWithTimeout<T>(
      std::move(future),
      time_provider.WaitFor(delay),
      ReadyConstantValueResolution<bool, std::false_type>());
}

/// Constructs a `FutureWithTimeout` for a `SendFuture<T>` as the primary
/// future, a timeout future obtained from a `TimeProvider<Clock>` instance,
/// and the timeout delay.
///
/// On timeout, the an empty `optional<SendReservation<T>>` value is returned
/// as the `Ready(T)` from the composed future's `Pend()`.
///
/// Note that this return value means that the timeout return value is not
/// distinguishable from the result when the channel is closed. If you prefer
/// a distinct status value, use `Timeout()`.
template <typename T, typename Clock>
auto TimeoutOrClosed(ReserveSendFuture<T>&& future,
                     TimeProvider<Clock>& time_provider,
                     typename Clock::duration delay) {
  using ResultType = typename ReserveSendFuture<T>::value_type;
  return CreateFutureWithTimeout<ResultType>(
      std::move(future),
      time_provider.WaitFor(delay),
      ReadyConstantValueResolution<std::optional<SendReservation<T>>,
                                   std::nullopt_t>(std::nullopt));
}

/// Helper function for using one of the other `TimeoutOrClosed` overloads which
/// take a TimeProvider.
///
/// This overload uses the system time provider returned by the call to
/// `GetSystemTimeProvider()`, and forwards it as the time provider along with
/// the other arguments to the other overloads. See those other overloads to
/// understand what type of future you will construct.
template <
    typename PrimaryFuture,
    int&... kExplicitGuard,
    typename = std::enable_if_t<!std::is_lvalue_reference_v<PrimaryFuture>>>
auto TimeoutOrClosed(PrimaryFuture&& primary_future,
                     typename chrono::SystemClock::duration delay) {
  return TimeoutOrClosed(std::forward<PrimaryFuture>(primary_future),
                         GetSystemTimeProvider(),
                         delay);
}

/// `ValueFutureWithTimeout<T>` is an alias for the type you get if you invoke
/// `Timeout()` with `ValueFuture<T>` as the future to add a timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using ValueFutureWithTimeout =
    decltype(Timeout(std::declval<ValueFuture<T>&&>(),
                     std::declval<TimeProvider<Clock>&>(),
                     std::declval<typename Clock::duration>()));

/// `ValueFutureWithTimeoutOr<T, U>` is an alias for the type you get if you
/// invoke `TimeoutOr(..., U&&)` with `ValueFuture<T>` as the future to add a
/// timeout to.
///
/// `U` is either the sentinel value type if you specify a constant value, or
/// `Function<T()>` if you use a function to generate the sentinel value.
template <typename T, typename U, typename Clock = chrono::SystemClock>
using ValueFutureWithTimeoutOr =
    decltype(TimeoutOr(std::declval<ValueFuture<T>&&>(),
                       std::declval<TimeProvider<Clock>&>(),
                       std::declval<typename Clock::duration>(),
                       std::declval<U&&>()));

/// `OptionalValueFutureWithTimeout<T>` is an alias for the type you get if you
/// invoke `Timeout()` with `OptionalValueFuture<T>` as the future to add a
/// timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using OptionalValueFutureWithTimeout =
    decltype(Timeout(std::declval<OptionalValueFuture<T>&&>(),
                     std::declval<TimeProvider<Clock>&>(),
                     std::declval<typename Clock::duration>()));

/// `OptionalValueFutureWithTimeoutOr<T, U>` is an alias for the type you get if
/// you invoke `TimeoutOr(..., U&&)` with `OptionalValueFuture<T>` as the future
/// to add a timeout to.
///
/// `U` is either the sentinel value type if you specify a constant value, or
/// `Function<T()>` if you use a function to generate the sentinel value.
template <typename T, typename U, typename Clock = chrono::SystemClock>
using OptionalValueFutureWithTimeoutOr =
    decltype(TimeoutOr(std::declval<OptionalValueFuture<T>&&>(),
                       std::declval<TimeProvider<Clock>&>(),
                       std::declval<typename Clock::duration>(),
                       std::declval<U&&>()));

/// `SendFutureWithTimeout<T>` is an alias for the type you get if you invoke
/// `Timeout()` with `SendFuture<T>` as the future to add a timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using SendFutureWithTimeout =
    decltype(Timeout(std::declval<SendFuture<T>&&>(),
                     std::declval<TimeProvider<Clock>&>(),
                     std::declval<typename Clock::duration>()));

/// `ReceiveFutureWithTimeout<T>` is an alias for the type you get if you
/// invoke `Timeout()` with `ReceiveFuture<T>` as the future to add a timeout
/// to.
template <typename T, typename Clock = chrono::SystemClock>
using ReceiveFutureWithTimeout =
    decltype(Timeout(std::declval<ReceiveFuture<T>&&>(),
                     std::declval<TimeProvider<Clock>&>(),
                     std::declval<typename Clock::duration>()));

/// `ReserveSendFutureWithTimeout<T>` is an alias for the type you get if you
/// invoke `Timeout()` with `ReserveSendFuture<T>` as the future to add a
/// timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using ReserveSendFutureWithTimeout =
    decltype(Timeout(std::declval<ReserveSendFuture<T>&&>(),
                     std::declval<TimeProvider<Clock>&>(),
                     std::declval<typename Clock::duration>()));

/// `SendFutureWithTimeoutOrClosed<T>` is an alias for the type you get if you
/// invoke `TimeoutOrClosed()` with `SendFuture<T>` as the future to add a
/// timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using SendFutureWithTimeoutOrClosed =
    decltype(TimeoutOrClosed(std::declval<SendFuture<T>&&>(),
                             std::declval<TimeProvider<Clock>&>(),
                             std::declval<typename Clock::duration>()));

/// `ReceiveFutureWithTimeoutOrClosed<T>` is an alias for the type you get if
/// you invoke `TimeoutOrClosed()` with `ReceiveFuture<T>` as the future to add
/// a timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using ReceiveFutureWithTimeoutOrClosed =
    decltype(TimeoutOrClosed(std::declval<ReceiveFuture<T>&&>(),
                             std::declval<TimeProvider<Clock>&>(),
                             std::declval<typename Clock::duration>()));

/// `ReserveSendFutureWithTimeoutOrClosed<T>` is an alias for the type you get
/// if you invoke `TimeoutOrClosed()` with `ReserveSendFuture<T>` as the future
/// to add a timeout to.
template <typename T, typename Clock = chrono::SystemClock>
using ReserveSendFutureWithTimeoutOrClosed =
    decltype(TimeoutOrClosed(std::declval<ReserveSendFuture<T>&&>(),
                             std::declval<TimeProvider<Clock>&>(),
                             std::declval<typename Clock::duration>()));

/// `CustomFutureWithTimeout<FutureType>` is an alias for the type you get if
/// you invoke `Timeout()` with a `FutureType`, to generate a composite future
/// with a timeout.
template <typename FutureType, typename Clock = chrono::SystemClock>
using CustomFutureWithTimeout =
    decltype(Timeout(std::declval<FutureType&&>(),
                     std::declval<TimeProvider<Clock>&>(),
                     std::declval<typename Clock::duration>()));

/// `CustomFutureWithTimeoutOr<FutureType, U>` is an alias for the type you get
/// if you invoke `TimeoutOr(..., U&&)` with `FutureType`, to generate a
/// composite future with a timeout.
///
/// `U` is either the sentinel value type if you specify a constant value, or
/// `Function<T()>` if you use a function to generate the sentinel value.
template <typename FutureType, typename U, typename Clock = chrono::SystemClock>
using CustomFutureWithTimeoutOr =
    decltype(TimeoutOr(std::declval<FutureType&&>(),
                       std::declval<TimeProvider<Clock>&>(),
                       std::declval<typename Clock::duration>(),
                       std::declval<U&&>()));

/// @endsubmodule

}  // namespace pw::async2
