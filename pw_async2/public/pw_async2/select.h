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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_containers/optional_tuple.h"
#include "pw_preprocessor/compiler.h"

namespace pw::async2 {
namespace internal {

// Pends `future` if it is pendable, storing its output at `kIndex` in `results`
// if it completes. Futures that are complete or were never armed are skipped,
// which disables their branch of the select.
template <size_t kIndex, typename Results, typename SubFuture>
void PendSelectBranch(Context& cx,
                      Results& results,
                      bool& any_pendable,
                      SubFuture& future) {
  if (!future.is_pendable()) {
    return;
  }
  any_pendable = true;
  auto poll = future.Pend(cx);
  if (poll.IsReady()) {
    results.template emplace<kIndex>(std::move(*poll));
  }
}

template <typename... Futures, size_t... kIndices>
Poll<OptionalTuple<FutureValue<Futures>...>> PendSelectImpl(
    Context& cx, std::index_sequence<kIndices...>, Futures&... futures) {
  OptionalTuple<FutureValue<Futures>...> results;
  bool any_pendable = false;
  (PendSelectBranch<kIndices>(cx, results, any_pendable, futures), ...);

  // None of the futures are pendable, which is a violation of the call
  // contract. Returning pending would stall the task forever.
  PW_ASSERT(any_pendable);

  if (results.empty()) {
    return Pending();
  }
  return results;
}

// Pends each pendable future in place and returns the outputs of all that
// completed. The futures are only referenced for the duration of the call, so
// their owner may move them between calls.
template <typename... Futures>
Poll<OptionalTuple<FutureValue<Futures>...>> PendSelect(Context& cx,
                                                        Futures&... futures) {
  static_assert(sizeof...(Futures) > 0,
                "Cannot select over an empty set of futures");
  static_assert(
      (!std::is_const_v<Futures> && ...),
      "Futures passed to PW_SELECT or PW_CO_SELECT must not be const");
  static_assert((Future<Futures> && ...),
                "All arguments to PW_SELECT or PW_CO_SELECT must be futures");
  return PendSelectImpl(cx, std::index_sequence_for<Futures...>(), futures...);
}

// Future that pends futures owned by a coroutine in place. Holds pointers to
// the futures across suspension, which is only safe because coroutine frames
// never move. For use by `PW_CO_SELECT` only.
template <typename... Futures>
class InPlaceSelectFuture {
 public:
  using value_type = OptionalTuple<FutureValue<Futures>...>;

  constexpr InPlaceSelectFuture() = default;

  explicit constexpr InPlaceSelectFuture(
      Futures&... futures PW_ATTRIBUTE_LIFETIME_BOUND)
      : futures_(&futures...), state_(FutureState::kPending) {}

  InPlaceSelectFuture(InPlaceSelectFuture&&) = default;
  InPlaceSelectFuture& operator=(InPlaceSelectFuture&&) = default;

  Poll<value_type> Pend(Context& cx) {
    PW_ASSERT(is_pendable());
    Poll<value_type> poll =
        PendFutures(cx, std::index_sequence_for<Futures...>());
    if (poll.IsReady()) {
      state_.MarkComplete();
    }
    return poll;
  }

  [[nodiscard]] constexpr bool is_pendable() const {
    return state_.is_pendable();
  }
  [[nodiscard]] constexpr bool is_complete() const {
    return state_.is_complete();
  }

 private:
  template <size_t... kIndices>
  Poll<value_type> PendFutures(Context& cx, std::index_sequence<kIndices...>) {
    return PendSelect(cx, *std::get<kIndices>(futures_)...);
  }

  std::tuple<Futures*...> futures_;
  FutureState state_;
};

template <typename... Futures>
InPlaceSelectFuture(Futures&...) -> InPlaceSelectFuture<Futures...>;

}  // namespace internal

/// @submodule{pw_async2,combinators}

template <typename... Futures>
class SelectFuture {
 public:
  static_assert(sizeof...(Futures) > 0,
                "Cannot select over an empty set of futures");

  using value_type = OptionalTuple<FutureValue<Futures>...>;

  constexpr SelectFuture() = default;

  explicit SelectFuture(Futures&&... futures)
      : futures_(std::move(futures)...), state_(FutureState::kPending) {}

  Poll<value_type> Pend(Context& cx) {
    Poll<value_type> poll = PendAll(cx, kTupleIndexSequence);
    if (poll.IsReady()) {
      state_.MarkComplete();
      futures_ = std::tuple<Futures...>();
    }
    return poll;
  }

  [[nodiscard]] constexpr bool is_pendable() const {
    return state_.is_pendable();
  }
  [[nodiscard]] constexpr bool is_complete() const {
    return state_.is_complete();
  }

 private:
  static constexpr auto kTupleIndexSequence =
      std::make_index_sequence<sizeof...(Futures)>();

  enum State : uint8_t {
    kDefaultConstructed,
    kPendable,
    kComplete,
  };

  /// Pends every sub-future, returning the results of each that is ready.
  template <size_t... Is>
  Poll<value_type> PendAll(Context& cx, std::index_sequence<Is...>) {
    return internal::PendSelect(cx, std::get<Is>(futures_)...);
  }

  std::tuple<Futures...> futures_;
  FutureState state_;
};

template <typename... Futures>
SelectFuture(Futures&&...) -> SelectFuture<Futures...>;

/// Runs each of the provided futures, resolving once the first has completed.
///
/// As additional futures may have completed during the time between the first
/// completion and the task running, `Select` returns a `Future<OptionalTuple>`
/// which stores the results of all the sub-futures which managed to complete.
///
/// `Select` takes ownership of its futures and destroys all of them once it
/// completes, including those that did not complete. To wait on the same
/// futures repeatedly, such as in a loop, use `PW_SELECT` or `PW_CO_SELECT`.
template <typename... Futures>
SelectFuture<Futures...> Select(Futures&&... futures) {
  static_assert((Future<Futures> && ...),
                "All arguments to Select must be Future types");
  return SelectFuture(std::forward<Futures>(futures)...);
}

/// @endsubmodule

}  // namespace pw::async2

/// @submodule{pw_async2,combinators}

/// Waits for the first of several futures to complete, without taking
/// ownership of them.
///
/// `PW_SELECT` pends each of the futures in place. If none of them have
/// completed, it returns `Pending()` from the current function. Otherwise, it
/// assigns an `OptionalTuple` containing the outputs of every future that
/// completed to `lhs`.
///
/// Unlike `Select`, `PW_SELECT` leaves the futures with their owner. Futures
/// that did not complete stay pending and can be selected again, so timers keep
/// their deadlines and in-progress operations are not cancelled. Only re-arm
/// the futures that completed.
///
/// - The futures must be non-const lvalues, such as members of the task or
///   composite future using `PW_SELECT`. They are only referenced while
///   `PW_SELECT` runs, so their owner may be moved between polls.
/// - Futures that are complete or were never armed are skipped, which disables
///   their branch. At least one future must be pendable.
/// - Check each element of the result with `has_value<I>()`. More than one
///   future may complete at once.
/// - `PW_SELECT` is a statement, not a future, so it can't be stored, returned,
///   or passed to other combinators. Use `Select` when a future is needed, or
///   call `PW_SELECT` from the `Pend()` function of a composite future.
///
/// For coroutines, use `PW_CO_SELECT`.
///
/// @code{.cpp}
///   Poll<> DoPend(Context& cx) override {
///     while (true) {
///       if (!coin_future_.is_pendable()) {
///         coin_future_ = coin_slot_.GetCoins();
///       }
///       if (!key_future_.is_pendable()) {
///         key_future_ = keypad_.WaitForKeyPress();
///       }
///       PW_SELECT(auto result, cx, coin_future_, key_future_);
///       if (result.has_value<0>()) {
///         coins_ += result.value<0>();
///       }
///       if (result.has_value<1>()) {
///         HandleKeyPress(result.value<1>());
///       }
///     }
///   }
/// @endcode
#define PW_SELECT(lhs, cx, ...) \
  PW_TRY_READY_ASSIGN(lhs, ::pw::async2::internal::PendSelect(cx, __VA_ARGS__))

/// Like `PW_SELECT`, but for use in coroutines.
///
/// `PW_CO_SELECT` suspends the coroutine until at least one of the futures
/// completes, then assigns an `OptionalTuple` containing the outputs of every
/// future that completed to `lhs`. Futures that did not complete stay pending
/// and can be selected again.
///
/// The futures are referenced while the coroutine is suspended. This is safe
/// for local variables of the coroutine, since coroutine frames never move.
/// Like `co_await` on an lvalue future, other futures must outlive the
/// `PW_CO_SELECT` and must not move while it is suspended.
///
/// @code{.cpp}
///   Coro<Status> Relay(CoroContext,
///                      Receiver<Msg>& rx,
///                      TimeProvider<SystemClock>& time) {
///     auto deadline = time.WaitFor(5s);  // One deadline for the whole loop.
///     auto next_msg = rx.Receive();
///     while (true) {
///       PW_CO_SELECT(auto result, next_msg, deadline);
///       if (result.has_value<1>()) {
///         co_return Status::DeadlineExceeded();
///       }
///       if (!result.value<0>().has_value()) {
///         co_return OkStatus();  // The channel closed.
///       }
///       Handle(*result.value<0>());
///       next_msg = rx.Receive();  // Re-arm only the future that completed.
///     }
///   }
/// @endcode
#define PW_CO_SELECT(lhs, ...) \
  lhs = co_await ::pw::async2::internal::InPlaceSelectFuture(__VA_ARGS__)

/// @endsubmodule
