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

#ifdef __cpp_concepts
#include <concepts>
#endif  // __cpp_concepts

#include <mutex>
#include <optional>
#include <type_traits>

#include "lib/stdcompat/utility.h"
#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_containers/intrusive_list.h"
#include "pw_memory/container_of.h"
#include "pw_sync/interrupt_spin_lock.h"

namespace pw::async2 {

/// @submodule{pw_async2,futures}

#ifdef __cpp_concepts

/// @concept pw::async2::Future
///
/// A `Future` is an abstract handle to an asynchronous operation that is polled
/// to completion. On completion, futures may return a value representing the
/// result of the operation.
///
/// The `Future` concept describes the future API. All future implementations
/// must satisfy this concept.
template <typename T>
concept Future = requires {
  /// value_type refers to the type produced by the future.
  typename T::value_type;

  /// `bool is_pendable() const` returns whether `Pend` can be called.
  static_cast<bool (T::*)() const>(&T::is_pendable);

  /// `bool is_complete() const` returns whether the future completed.
  static_cast<bool (T::*)() const>(&T::is_complete);

  /// `Poll<value_type> Pend(Context&)` advances the future. Returns `Ready`
  /// when the operation completes. Uses the `Context` to store a waker and
  /// returns `Pending` if there is more work to do. Must not be called again
  /// after completing.
  static_cast<Poll<typename T::value_type> (T::*)(Context&)>(&T::Pend);

  /// Futures must have a default constructor that initializes them to an empty
  /// state, and a destructor that cancels them.
} && std::default_initializable<T> && std::destructible<T> && std::movable<T>;

#else  // C++17 version

namespace internal {

template <typename T, typename = void>
struct is_future : std::false_type {};

template <typename T>
struct is_future<
    T,
    std::void_t<typename T::value_type,
                decltype(std::declval<T&>().Pend(std::declval<Context&>())),
                decltype(std::declval<const T&>().is_pendable()),
                decltype(std::declval<const T&>().is_complete())>>
    : std::conjunction<
          std::is_convertible<decltype(&T::is_pendable), bool (T::*)() const>,
          std::is_convertible<decltype(&T::is_complete), bool (T::*)() const>,
          std::is_convertible<decltype(&T::Pend),
                              Poll<typename T::value_type> (T::*)(Context&)>,
          std::is_default_constructible<T>,
          std::is_destructible<T>> {};

}  // namespace internal

// This variable is named as a type to match the C++20 concept. This makes it
// possible to use `Future` in static asserts in either C++17 or C++20.
template <typename T>
constexpr bool Future =
    internal::is_future<std::remove_cv_t<std::remove_reference_t<T>>>::value;

#endif  // __cpp_concepts

/// Standard pw_async2 future states. Used by `FutureCore` and may be used by
/// custom future implementations.
class FutureState {
 public:
  /// Tag for constructing an active future, for which `Pend` may be called.
  enum Pending { kPending };

  /// Tag for constructing a future that is guaranteed to return `Ready` from
  /// the next `Pend` call.
  enum ReadyForCompletion { kReadyForCompletion };

  /// Represents an empty future. The future does not yet represent an
  /// asynchronous operation and `Pend` may not be called.
  constexpr FutureState() : state_(State::kEmpty) {}

  /// Represents an active future, for which `Pend` may be called.
  constexpr FutureState(Pending) : state_(State::kPending) {}

  /// Represents a future that is guaranteed to return `Ready` from the next
  /// `Pend` call. Use of the `ReadyForCompletion" state is optional---future
  /// implementations may skip from `Pending` to `Complete` if desired.
  constexpr FutureState(ReadyForCompletion) : state_(State::kReady) {}

  constexpr FutureState(const FutureState&) = delete;
  constexpr FutureState& operator=(const FutureState&) = delete;

  /// Move constructs a `FutureState`, leaving the other in its default
  /// constructed / empty state.
  constexpr FutureState(FutureState&& other)
      : state_(cpp20::exchange(other.state_, State::kEmpty)) {}

  constexpr FutureState& operator=(FutureState&& other) {
    state_ = cpp20::exchange(other.state_, State::kEmpty);
    return *this;
  }

  friend constexpr bool operator==(const FutureState& lhs,
                                   const FutureState& rhs) {
    return lhs.state_ == rhs.state_;
  }

  friend constexpr bool operator!=(const FutureState& lhs,
                                   const FutureState& rhs) {
    return lhs.state_ != rhs.state_;
  }

  /// @returns Whether the future's `Pend()` function can be called.
  [[nodiscard]] constexpr bool is_pendable() const {
    return state_ > State::kComplete;
  }

  /// @returns Whether the future has completed: the future's `Pend()` returned
  /// `Ready`.
  [[nodiscard]] constexpr bool is_complete() const {
    return state_ == State::kComplete;
  }

  /// @returns `true` if the next `Pend()` call is guaranteed to return `Ready`.
  /// Not all future implementations use the ready state; `Pend()` may return
  /// `Ready` even though `is_ready` is `false`.
  [[nodiscard]] constexpr bool is_ready() const {
    return state_ == State::kReady;
  }

  /// @returns `true` if the future was NOT default constructed. The future
  /// either represents an active or completed asynchronous operation.
  [[nodiscard]] constexpr bool is_initialized() const {
    return state_ != State::kEmpty;
  }

  void MarkReady() { state_ = State::kReady; }

  void MarkComplete() { state_ = State::kComplete; }

 private:
  enum class State : unsigned char {
    // The future is in a default constructed, empty state. It does not
    // represent an async operation and `Pend()` cannot be called.
    kEmpty,

    // A previous call to `Pend()` returned `Ready()`.
    kComplete,

    // The next call to `Pend()` will return `Ready()`.
    kReady,

    // `Pend()` may be called to advance the operation represented by this
    // future. `Pend()` may return `Ready()` or `Pending()`.
    kPending,
  } state_;
};

/// `FutureCore` provides common functionality for futures that need to be
/// wakeable and stored in a list.
///
/// This class provides the core mechanism for:
/// - Storing a `Waker` to be woken up.
/// - Tracking future state.
/// - Being part of an `IntrusiveList`.
///
/// It is designed to be used as a member of a concrete future class
/// (composition) rather than a base class, to simplify move semantics and
/// object lifetime.
class FutureCore : public IntrusiveForwardList<FutureCore>::Item {
 public:
  constexpr FutureCore() = default;

  FutureCore(FutureCore&& other) noexcept;

  FutureCore& operator=(FutureCore&& other) noexcept;

  /// Constructs a pending `FutureCore` that represents an async operation.
  /// `Pend` must be called until it returns `Ready`.
  ///
  /// A pending `FutureCore` must be tracked by its provider (e.g. in a
  /// `FutureList`).
  explicit constexpr FutureCore(FutureState::Pending)
      : state_(FutureState::kPending) {}

  /// Creates a wakeable future that is ready for completion. The next call to
  /// `Pend` must return `Ready`.
  explicit constexpr FutureCore(FutureState::ReadyForCompletion)
      : state_(FutureState::kReadyForCompletion) {}

  FutureCore(const FutureCore&) = delete;
  FutureCore& operator=(const FutureCore&) = delete;

  ~FutureCore() = default;

  /// @copydoc FutureState::is_pendable
  [[nodiscard]] constexpr bool is_pendable() const {
    return state_.is_pendable();
  }

  /// @copydoc FutureState::is_complete
  [[nodiscard]] constexpr bool is_complete() const {
    return state_.is_complete();
  }

  /// @returns `true` if the next `Pend()` call is guaranteed to return `Ready`.
  /// Depending on the implementation, `Pend()` may return `Ready` while
  /// `is_ready` is `false`.
  ///
  /// Future implementations call `WakeAndMarkReady` to set `is_ready` to
  /// `true`.
  [[nodiscard]] constexpr bool is_ready() const { return state_.is_ready(); }

  /// @copydoc FutureState::is_initialized
  [[nodiscard]] constexpr bool is_initialized() const {
    return state_.is_initialized();
  }

  /// Wakes the task waiting on the future, if any. The future must be pended in
  /// order to make progress.
  void Wake() { waker_.Wake(); }

  /// Wakes the task pending the future and sets `is_ready` to `true`. Only call
  /// this if the next call to to the future's `Pend()` will return `Ready`.
  void WakeAndMarkReady() {
    Wake();
    state_.MarkReady();
  }

  /// Provides direct access to the waker for future implementations that
  /// manually store a waker.
  ///
  /// @warning Do not use this function when `FutureCore::DoPend` is used.
  /// `FutureCore::DoPend` stores the waker when `Pend()` returns `Pending`.
  Waker& waker() { return waker_; }

  /// Mark the future as complete, which indicates that a future has returned
  /// `Ready` from its `Pend` function.
  ///
  /// @warning Do not use this function when `FutureCore::DoPend` is used.
  /// `FutureCore::DoPend` calls `MarkComplete` when `Pend()` returns `Ready`.
  void MarkComplete() { state_.MarkComplete(); }

  /// Removes this future from its list, if it is in one.
  void Unlist() { unlist(); }

  /// Unlists the `FutureCore` and resets it to the empty state.
  void Reset() {
    Unlist();
    state_ = FutureState();
  }

  /// Optional `Pend()` function that defers to the future implementation's
  /// `DoPend(Context&)` function. `FutureCore::DoPend` does the following:
  ///
  /// - Asserts that the future is pendable.
  /// - If the future's `DoPend` returns `Pending`, stores a waker.
  /// - If the future's `DoPend` returns `Ready`, marks the future as complete.
  ///
  /// It is recommended for `Pend` to use `FutureCore::DoPend`, but not
  /// required. Custom `Pend` implementations must enforce the same semantics.
  template <typename FutureType>
  auto DoPend(FutureType& future, Context& cx) PW_NO_LOCK_SAFETY_ANALYSIS {
    PW_ASSERT(is_pendable());

    auto poll = future.DoPend(cx);
    if (poll.IsPending()) {
      PW_ASYNC_STORE_WAKER(cx, waker_, FutureType::kWaitReason);
    } else {
      MarkComplete();
    }

    return poll;
  }

  /// @returns whether the future is currently in a list.
  [[nodiscard]] bool in_list() const { return !unlisted(); }

 private:
  friend class BaseFutureList;  // for IntrusiveForwardList::Item functions

  Waker waker_;
  FutureState state_;
};

/// A list of `FutureCore`s with common future-related operations. Future
/// providers may use `BaseFutureList` or `FutureList` in place of a plain
/// `IntrusiveForwardList`.
///
/// This class does not provide any locking. It is the responsibility of the
/// user to ensure safety, typically by holding a lock in the containing
/// provider.
class BaseFutureList {
 public:
  constexpr BaseFutureList() = default;

  bool empty() const { return list_.empty(); }

  void Push(FutureCore& future) { containers::PushBackSlow(list_, future); }

  void PushRequireEmpty(FutureCore& future);

  bool PushIfEmpty(FutureCore& future);

  FutureCore* PopIfAvailable() { return list_.empty() ? nullptr : &Pop(); }

  FutureCore& Pop() {
    FutureCore& future = list_.front();
    list_.pop_front();
    return future;
  }

  /// Pops the next future fromt the list and calls `WakeAndMarkReady()`.
  /// Crashes if there are no futures in the list.
  void ResolveOne() {
    list_.front().WakeAndMarkReady();
    list_.pop_front();
  }

  void ResolveOneIfAvailable() {
    if (!list_.empty()) {
      ResolveOne();
    }
  }

  /// Pops all futures and calls `WakeAndMarkReady()` on them.
  void ResolveAll();

 protected:
  IntrusiveForwardList<FutureCore>& list() { return list_; }

 private:
  IntrusiveForwardList<FutureCore> list_;
};

/// List of futures of a custom future type. This is a minimal extension to
/// `BaseFutureList`.
///
/// @tparam kGetFutureImpl a function that converts a `FutureCore&` to its
///     corresponding future type
/// @tparam kGetFutureCore a function that converts a future reference to its
///     corresponding `FutureCore`.
template <auto kGetFutureImpl, auto kGetFutureCore>
class CustomFutureList : public BaseFutureList {
 public:
  using value_type = std::remove_reference_t<decltype(*kGetFutureImpl(
      std::declval<FutureCore*>()))>;
  using pointer = value_type*;
  using reference = value_type&;

  constexpr CustomFutureList() = default;

  void Push(FutureCore& future) { BaseFutureList::Push(future); }
  void Push(reference future) { Push(kGetFutureCore(future)); }

  void PushRequireEmpty(FutureCore& future) {
    BaseFutureList::PushRequireEmpty(future);
  }
  void PushRequireEmpty(reference future) {
    PushRequireEmpty(kGetFutureCore(future));
  }

  bool PushIfEmpty(FutureCore& future) {
    return BaseFutureList::PushIfEmpty(future);
  }
  bool PushIfEmpty(reference future) {
    return PushIfEmpty(kGetFutureCore(future));
  }

  pointer PopIfAvailable() {
    return kGetFutureImpl(BaseFutureList::PopIfAvailable());
  }

  reference Pop() { return *kGetFutureImpl(BaseFutureList::PopIfAvailable()); }

  template <typename Resolver>
  void ResolveAllWith(Resolver&& resolver) {
    while (!list().empty()) {
      resolver(Pop());
    }
  }

  template <typename Resolver>
  void ResolveOneWith(Resolver&& resolver) {
    if (!list().empty()) {
      resolver(Pop());
    }
  }
};

/// A `CustomFutureList` that uses a pointer to a `FutureCore` member.
///
/// @tparam kMemberPtr pointer to a `FutureCore` member of a custom future
///     class
template <auto kMemberPtr>
using FutureList =
    CustomFutureList<ContainerOf<kMemberPtr, FutureCore>, MemberOf<kMemberPtr>>;

/// @endsubmodule

}  // namespace pw::async2
