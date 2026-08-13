// Copyright 2026 The Pigweed Authors
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

#include <type_traits>
#include <utility>
#include <variant>

#include "pw_assert/assert.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"

namespace pw::async2 {

/// @submodule{pw_async2,futures}

/// @brief A wrapper that holds either a pending future or its resolved value.
///
/// `FutureOrValue` is designed for manual polling state machines (in `DoPend`)
/// where a task must wait for multiple independent asynchronous operations to
/// complete. As operations may complete at different times, the results of each
/// may need to be stored across later suspension points while waiting for the
/// remaining operations. `FutureOrValue` simplifies writing such state machines
/// by holding either a pending future or its resolved result.
///
/// @warning `FutureOrValue` should _only_ be used in manual polling contexts,
/// stored directly as a member within the final consumer of the future. It
/// should never be used as a general purpose container, as it breaks async
/// composability. `FutureOrValue` should never be used if the result of a
/// future is consumed immediately. It only provides value when the result needs
/// to persist across subsequent suspension points before it is used.
template <typename FutureType, typename Enable = void>
class FutureOrValue;

template <typename FutureType>
class FutureOrValue<
    FutureType,
    std::enable_if_t<!std::is_same_v<FutureValue<FutureType>, ReadyType>>> {
 public:
  using value_type = typename FutureType::value_type;

  constexpr FutureOrValue() = default;

  explicit FutureOrValue(FutureType&& future)
      : state_(std::in_place_type<FutureType>, std::move(future)) {}

  FutureOrValue(const FutureOrValue&) = delete;
  FutureOrValue& operator=(const FutureOrValue&) = delete;

  FutureOrValue(FutureOrValue&&) = default;
  FutureOrValue& operator=(FutureOrValue&&) = default;

  ~FutureOrValue() = default;

  /// Assigns a new future.
  ///
  /// This destroys any existing future (cancelling it) and clears any stored
  /// value.
  FutureOrValue& operator=(FutureType&& future) {
    state_.template emplace<FutureType>(std::move(future));
    return *this;
  }

  /// Advances the stored future.
  ///
  /// If the value is already available, returns `true` immediately.
  /// Otherwise, polls the future. If the future resolves, stores the value and
  /// returns `true`. If the future is pending, returns `false`.
  bool Advance(Context& cx) {
    if (std::holds_alternative<value_type>(state_)) {
      return true;
    }
    PW_ASSERT(std::holds_alternative<FutureType>(state_));
    FutureType& future = std::get<FutureType>(state_);
    PW_ASSERT(future.is_pendable());
    auto result = future.Pend(cx);
    if (result.IsPending()) {
      return false;
    }
    state_.template emplace<value_type>(std::move(*result));
    return true;
  }

  /// Returns true if neither a future nor its result is held.
  bool empty() const { return std::holds_alternative<std::monostate>(state_); }

  /// Returns whether the value is available.
  bool has_value() const { return std::holds_alternative<value_type>(state_); }

  /// Returns whether an active future is stored and pending.
  bool has_future() const {
    return std::holds_alternative<FutureType>(state_) &&
           std::get<FutureType>(state_).is_pendable();
  }

  /// Accesses the stored value. Must only be called if `has_value()` is true.
  value_type& value() & {
    PW_ASSERT(has_value());
    return std::get<value_type>(state_);
  }

  const value_type& value() const& {
    PW_ASSERT(has_value());
    return std::get<value_type>(state_);
  }

  /// Accesses the stored value. Must only be called if `has_value()` is true.
  value_type& operator*() & {
    PW_ASSERT(has_value());
    return std::get<value_type>(state_);
  }

  const value_type& operator*() const& {
    PW_ASSERT(has_value());
    return std::get<value_type>(state_);
  }

  value_type* operator->() {
    PW_ASSERT(has_value());
    return &std::get<value_type>(state_);
  }

  const value_type* operator->() const {
    PW_ASSERT(has_value());
    return &std::get<value_type>(state_);
  }

  /// Moves the value out and resets state to empty.
  /// Must only be called if `has_value()` is true.
  value_type Take() {
    PW_ASSERT(has_value());
    value_type val = std::move(std::get<value_type>(state_));
    state_.template emplace<std::monostate>();
    return val;
  }

  /// Cancels the operation by destroying the future and clearing any value.
  void Reset() { state_.template emplace<std::monostate>(); }

 private:
  std::variant<std::monostate, FutureType, value_type> state_;
};

// Specialization for futures that don't produce a value.
template <typename FutureType>
class FutureOrValue<
    FutureType,
    std::enable_if_t<std::is_same_v<FutureValue<FutureType>, ReadyType>>> {
 public:
  using value_type = void;

  constexpr FutureOrValue() = default;

  explicit FutureOrValue(FutureType&& future)
      : state_(std::in_place_type<FutureType>, std::move(future)) {}

  FutureOrValue(const FutureOrValue&) = delete;
  FutureOrValue& operator=(const FutureOrValue&) = delete;

  FutureOrValue(FutureOrValue&&) = default;
  FutureOrValue& operator=(FutureOrValue&&) = default;

  ~FutureOrValue() = default;

  FutureOrValue& operator=(FutureType&& future) {
    state_.template emplace<FutureType>(std::move(future));
    return *this;
  }

  bool Advance(Context& cx) {
    if (std::holds_alternative<ReadyType>(state_)) {
      return true;
    }
    PW_ASSERT(std::holds_alternative<FutureType>(state_));
    FutureType& future = std::get<FutureType>(state_);
    PW_ASSERT(future.is_pendable());
    auto result = future.Pend(cx);
    if (result.IsPending()) {
      return false;
    }
    state_.template emplace<ReadyType>();
    return true;
  }

  /// Returns true if neither a future nor its result is held.
  bool empty() const { return std::holds_alternative<std::monostate>(state_); }

  /// Returns whether the value is available.
  bool has_value() const { return std::holds_alternative<ReadyType>(state_); }

  /// Returns whether an active future is stored and pending.
  bool has_future() const {
    return std::holds_alternative<FutureType>(state_) &&
           std::get<FutureType>(state_).is_pendable();
  }

  void Take() {
    PW_ASSERT(has_value());
    state_.template emplace<std::monostate>();
  }

  void Reset() { state_.template emplace<std::monostate>(); }

 private:
  std::variant<std::monostate, FutureType, ReadyType> state_;
};

namespace internal {

template <typename... Ts>
bool AdvanceAll(Context& cx, Ts&... ts) {
  return (... & ts.Advance(cx));
}

}  // namespace internal

/// @brief Macro to poll multiple `FutureOrValue` objects.
///
/// Returns `Pending()` if any of the provided slots are not ready.
/// Ensures that all provided futures are polled.
#define PW_FOV_TRY_ADVANCE(cx, ...)                               \
  do {                                                            \
    if (!::pw::async2::internal::AdvanceAll((cx), __VA_ARGS__)) { \
      return ::pw::async2::Pending();                             \
    }                                                             \
  } while (0)

}  // namespace pw::async2
