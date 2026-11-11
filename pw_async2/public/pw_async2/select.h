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

#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_containers/optional_tuple.h"
#include "pw_memory/construct_at.h"
#include "pw_polyfill/language_feature_macros.h"
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
auto PendSelect(Context& cx, Futures&... futures) {
  static_assert(sizeof...(Futures) > 0,
                "Cannot select over an empty set of futures");
  static_assert(
      (!std::is_const_v<Futures> && ...),
      "Futures passed to PW_SELECT or PW_CO_SELECT must not be const");
  static_assert(
      (!std::is_volatile_v<Futures> && ...),
      "Futures passed to PW_SELECT or PW_CO_SELECT must not be volatile");
  static_assert(((std::is_const_v<Futures> || std::is_volatile_v<Futures> ||
                  Future<Futures>) &&
                 ...),
                "All arguments to PW_SELECT or PW_CO_SELECT must be futures");
  if constexpr (sizeof...(Futures) > 0 && (!std::is_const_v<Futures> && ...) &&
                (!std::is_volatile_v<Futures> && ...) &&
                (Future<Futures> && ...)) {
    return PendSelectImpl(
        cx, std::index_sequence_for<Futures...>(), futures...);
  } else {
    return Poll<>(Pending());
  }
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

// Stand-in for `pw::containers::internal::Maybe<Tuple>` until
// https://pwrev.dev/522188 lands.
template <typename Tuple, bool = std::is_trivially_destructible_v<Tuple>>
union SelectFutureUnion {
  constexpr SelectFutureUnion() : no_futures{} {}

  template <typename... Futures>
  explicit constexpr SelectFutureUnion(std::in_place_t,
                                       Futures&&... sub_futures)
      : futures(std::forward<Futures>(sub_futures)...) {}

  ~SelectFutureUnion() = default;

  char no_futures;
  Tuple futures;
};

template <typename Tuple>
union SelectFutureUnion<Tuple, false> {
  constexpr SelectFutureUnion() : no_futures{} {}

  template <typename... Futures>
  explicit constexpr SelectFutureUnion(std::in_place_t,
                                       Futures&&... sub_futures)
      : futures(std::forward<Futures>(sub_futures)...) {}

  PW_CONSTEXPR_CPP20 ~SelectFutureUnion() {}

  char no_futures;
  Tuple futures;
};

template <typename Tuple, bool = std::is_trivially_destructible_v<Tuple>>
struct SelectFutureStorage;

// Storage and lifecycle management for a `SelectFuture`'s sub-futures and
// state. The sub-futures are alive if and only if `state.is_pendable()`, so no
// separate flag is needed to track their lifetime.
template <typename Tuple>
struct SelectFutureMembers {
  constexpr SelectFutureMembers() = default;

  template <typename... Futures>
  explicit constexpr SelectFutureMembers(std::in_place_t,
                                         Futures&&... sub_futures)
      : storage(std::in_place, std::forward<Futures>(sub_futures)...),
        state(FutureState::kPending) {}

  SelectFutureMembers(const SelectFutureMembers&) = delete;
  SelectFutureMembers& operator=(const SelectFutureMembers&) = delete;
  SelectFutureMembers(SelectFutureMembers&&) = delete;
  SelectFutureMembers& operator=(SelectFutureMembers&&) = delete;
  ~SelectFutureMembers() = default;

  constexpr Tuple& futures() { return storage.futures; }

  void Complete() {
    state.MarkComplete();
    // Destroy (and thereby cancel) all sub-futures.
    std::destroy_at(&storage.futures);
  }

  PW_CONSTEXPR_CPP20 void Reset() {
    if (state.is_pendable()) {
      state = FutureState();
      std::destroy_at(&storage.futures);
    }
  }

  // Moves the sub-futures, if any, from `other`, then destroys `other`'s
  // sub-futures. The sub-futures in `this` must not be alive.
  PW_CONSTEXPR_CPP20 void MoveFrom(SelectFutureMembers& other) {
    if (other.state.is_pendable()) {
      pw::construct_at(&storage.futures, std::move(other.storage.futures));
      state = std::move(other.state);
      std::destroy_at(&other.storage.futures);
    } else {
      state = std::move(other.state);
    }
  }

  PW_CONSTEXPR_CPP20 void MoveAssignFrom(SelectFutureMembers& other) {
    if (this == &other) {
      return;
    }
    if (state.is_pendable()) {
      // Move `other` before destroying `this`'s sub-futures in case a
      // sub-future of `this` transitively owns `other`.
      SelectFutureStorage<Tuple> temp;
      temp.MoveFrom(other);
      Reset();
      MoveFrom(temp);
    } else {
      MoveFrom(other);
    }
  }

  SelectFutureUnion<Tuple> storage;
  FutureState state;
};

// Specialized so that `SelectFuture` is trivially destructible if its
// sub-futures are.
template <typename Tuple, bool>
struct SelectFutureStorage : SelectFutureMembers<Tuple> {
  using SelectFutureMembers<Tuple>::SelectFutureMembers;
  constexpr SelectFutureStorage() = default;
};

template <typename Tuple>
struct SelectFutureStorage<Tuple, false> : SelectFutureMembers<Tuple> {
  using SelectFutureMembers<Tuple>::SelectFutureMembers;
  constexpr SelectFutureStorage() = default;
  PW_CONSTEXPR_CPP20 ~SelectFutureStorage() { this->Reset(); }
};

// Guards `SelectFuture` member types so invalid `Futures` fail cleanly at
// `SelectFuture`'s `static_assert`s without secondary instantiation errors.
template <bool kValid, typename... Futures>
struct SelectFutureTraits {
  using value_type = OptionalTuple<FutureValue<Futures>...>;
  using Tuple = std::tuple<Futures...>;
};

template <typename... Futures>
struct SelectFutureTraits<false, Futures...> {
  using value_type = void;
  using Tuple = std::tuple<>;
};

}  // namespace internal

/// @submodule{pw_async2,combinators}

/// Future returned by `Select()` that polls multiple sub-futures until at least
/// one completes.
///
/// `SelectFuture` owns its sub-futures by value and destroys all of them in
/// place once `Pend()` returns `Ready`, cancelling any that did not complete.
///
/// Prefer constructing `SelectFuture` via `Select()`. To wait on the same
/// futures repeatedly without destroying incomplete ones, use `PW_SELECT` or
/// `PW_CO_SELECT`.
template <typename... Futures>
class [[nodiscard]] SelectFuture {
 private:
  static constexpr bool kValidFutures =
      sizeof...(Futures) > 0 && (!std::is_reference_v<Futures> && ...) &&
      (!std::is_const_v<Futures> && ...) &&
      (!std::is_volatile_v<Futures> && ...) && (Future<Futures> && ...);

  using Traits = internal::SelectFutureTraits<kValidFutures, Futures...>;
  using Tuple = typename Traits::Tuple;

  static constexpr bool kNothrowMove =
      std::is_nothrow_move_constructible_v<Tuple>;

 public:
  static_assert(sizeof...(Futures) > 0,
                "Cannot select over an empty set of futures");
  static_assert((!std::is_reference_v<Futures> && ...),
                "SelectFuture stores futures by value; reference types are not "
                "supported");
  static_assert((!std::is_const_v<std::remove_reference_t<Futures>> && ...),
                "Futures in SelectFuture must not be const");
  static_assert((!std::is_volatile_v<std::remove_reference_t<Futures>> && ...),
                "Futures in SelectFuture must not be volatile");
  static_assert(((std::is_reference_v<Futures> || std::is_const_v<Futures> ||
                  std::is_volatile_v<Futures> || Future<Futures>) &&
                 ...),
                "All types in SelectFuture must be Future types");

  /// Output type (`OptionalTuple<FutureValue<Futures>...>`) produced on
  /// completion, holding the output of each sub-future that completed.
  using value_type = typename Traits::value_type;

  /// Constructs an empty, non-pendable `SelectFuture`.
  constexpr SelectFuture() = default;

  /// Constructs a pendable `SelectFuture` that takes ownership of `futures`.
  ///
  /// @param[in] futures The sub-futures to poll concurrently.
  template <bool kEnable = kValidFutures, std::enable_if_t<kEnable, int> = 0>
  explicit constexpr SelectFuture(Futures&&... futures)
      : storage_(std::in_place, std::move(futures)...) {}

  SelectFuture(const SelectFuture&) = delete;
  SelectFuture& operator=(const SelectFuture&) = delete;

  /// Move-constructs a `SelectFuture`, leaving `other` empty.
  ///
  /// @param[in,out] other The `SelectFuture` to move from.
  PW_CONSTEXPR_CPP20 SelectFuture(SelectFuture&& other) noexcept(kNothrowMove) {
    storage_.MoveFrom(other.storage_);
  }

  /// Move-assigns from `other`, cancelling and destroying any active
  /// sub-futures in `this` and leaving `other` empty.
  ///
  /// @param[in,out] other The `SelectFuture` to move from.
  /// @returns A reference to `this`.
  PW_CONSTEXPR_CPP20 SelectFuture& operator=(SelectFuture&& other) noexcept(
      kNothrowMove) {
    storage_.MoveAssignFrom(other.storage_);
    return *this;
  }

  /// Destroys the `SelectFuture`, cancelling any active sub-futures.
  ~SelectFuture() = default;

  /// Polls each pendable sub-future in order, completing once at least one
  /// returns `Ready`.
  ///
  /// When `Pend()` returns `Ready`, all sub-futures (including any that did not
  /// complete) are immediately destroyed in place.
  ///
  /// @pre `is_pendable()` is `true` and at least one sub-future is pendable.
  /// @param[in,out] cx The asynchronous `Context` used to register wakers.
  /// @returns `Ready` with an `OptionalTuple` of all completed outputs if one
  /// or more sub-futures completed, or `Pending` otherwise.
  Poll<value_type> Pend(Context& cx) {
    PW_ASSERT(is_pendable());
    Poll<value_type> poll = PendAll(cx, kTupleIndexSequence);
    if (poll.IsReady()) {
      storage_.Complete();
    }
    return poll;
  }

  /// Checks whether `Pend()` may be called.
  ///
  /// @returns `true` if the `SelectFuture` holds active sub-futures and has not
  /// yet completed.
  [[nodiscard]] constexpr bool is_pendable() const {
    return storage_.state.is_pendable();
  }

  /// Checks whether the `SelectFuture` has completed.
  ///
  /// @returns `true` if a previous call to `Pend()` returned `Ready`.
  [[nodiscard]] constexpr bool is_complete() const {
    return storage_.state.is_complete();
  }

 private:
  static constexpr auto kTupleIndexSequence =
      std::make_index_sequence<sizeof...(Futures)>();

  /// Pends every sub-future, returning the results of each that is ready.
  template <size_t... kIndices>
  Poll<value_type> PendAll(Context& cx, std::index_sequence<kIndices...>) {
    return internal::PendSelectImpl(cx,
                                    std::index_sequence<kIndices...>(),
                                    std::get<kIndices>(storage_.futures())...);
  }

  internal::SelectFutureStorage<Tuple> storage_;
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
///
/// @param[in] futures The futures to take ownership of and poll concurrently.
/// @returns A `SelectFuture` owning `futures`.
template <typename... Futures>
constexpr auto Select(Futures&&... futures) {
  static_assert(sizeof...(Futures) > 0,
                "Cannot select over an empty set of futures");
  static_assert((!std::is_lvalue_reference_v<Futures> && ...),
                "Select takes ownership of its futures; pass rvalues "
                "(std::move) or use PW_SELECT to select in place");
  static_assert((!std::is_rvalue_reference_v<Futures> && ...),
                "Do not specify explicit reference template arguments to "
                "Select");
  static_assert((!std::is_const_v<std::remove_reference_t<Futures>> && ...),
                "Futures passed to Select must not be const");
  static_assert((!std::is_volatile_v<std::remove_reference_t<Futures>> && ...),
                "Futures passed to Select must not be volatile");
  static_assert(((std::is_reference_v<Futures> || std::is_const_v<Futures> ||
                  std::is_volatile_v<Futures> || Future<Futures>) &&
                 ...),
                "All arguments to Select must be Future types");
  if constexpr (sizeof...(Futures) > 0 &&
                (!std::is_reference_v<Futures> && ...) &&
                (!std::is_const_v<Futures> && ...) &&
                (!std::is_volatile_v<Futures> && ...) &&
                (Future<Futures> && ...)) {
    return SelectFuture<Futures...>(std::forward<Futures>(futures)...);
  } else {
    return 0;
  }
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
