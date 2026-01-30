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

#include <optional>
#include <type_traits>

#include "pw_assert/assert.h"
#include "pw_async2/future.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_sync/interrupt_spin_lock.h"

namespace pw::async2 {
namespace internal {

inline sync::InterruptSpinLock& ValueProviderLock() {
  PW_CONSTINIT static sync::InterruptSpinLock lock;
  return lock;
}

}  // namespace internal

template <typename T>
class ValueProvider;
template <typename T>
class BroadcastValueProvider;

/// @submodule{pw_async2,futures}

/// A future that holds a single value.
///
/// A `ValueFuture` is a concrete `Future` implementation that is vended by a
/// `ValueProvider` or a `BroadcastValueProvider`. It waits until the provider
/// resolves it with a value.
template <typename T>
class ValueFuture {
 public:
  using value_type = T;

  constexpr ValueFuture() = default;

  ValueFuture(ValueFuture&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    *this = std::move(other);
  }

  ValueFuture& operator=(ValueFuture&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    if (this != &other) {
      std::lock_guard lock(internal::ValueProviderLock());
      core_ = std::move(other.core_);
      value_ = std::move(other.value_);
    }
    return *this;
  }

  ~ValueFuture() PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    std::lock_guard lock(internal::ValueProviderLock());
    core_.Unlist();
  }

  /// Creates a `ValueFuture` that is already resolved by constructing its
  /// value in-place.
  template <typename... Args>
  static ValueFuture Resolved(Args&&... args) {
    return ValueFuture(std::in_place, std::forward<Args>(args)...);
  }

  Poll<T> Pend(Context& cx) {
    // ValueFuture uses a global lock so that futures don't have to access their
    // provider to get a lock after they're completed. This ensures the
    // ValueFuture never needs to access the provider.
    //
    // With some care (and complexity), the lock could be moved to the provider.
    // A global lock is simpler and more efficient in practice.
    std::lock_guard lock(internal::ValueProviderLock());
    return core_.DoPend<ValueFuture<T>>(*this, cx);
  }

  [[nodiscard]] bool is_pendable() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return core_.is_pendable();
  }

  [[nodiscard]] bool is_complete() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return core_.is_complete();
  }

 private:
  friend class FutureCore;
  friend class ValueProvider<T>;
  friend class BroadcastValueProvider<T>;

  static constexpr char kWaitReason[] = "ValueFuture";

  template <typename... Args>
  explicit ValueFuture(std::in_place_t, Args&&... args)
      : core_(FutureState::kReadyForCompletion),
        value_(std::in_place, std::forward<Args>(args)...) {}

  ValueFuture(FutureState::Pending) : core_(FutureState::kPending) {}

  Poll<T> DoPend(Context&)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::ValueProviderLock()) {
    if (!core_.is_ready()) {
      return Pending();
    }

    return Ready(std::move(*value_));
  }

  template <typename... Args>
  void ResolveLocked(Args&&... args)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::ValueProviderLock()) {
    // SAFETY: This is only called from FutureList with the lock held.
    PW_DASSERT(!value_.has_value());
    value_.emplace(std::forward<Args>(args)...);
    core_.WakeAndMarkReady();
  }

  FutureCore core_ PW_GUARDED_BY(internal::ValueProviderLock());
  std::optional<T> value_ PW_GUARDED_BY(internal::ValueProviderLock());
};

/// Specialization for a future that does not return any value, just a
/// completion signal.
template <>
class ValueFuture<void> {
 public:
  using value_type = ReadyType;

  constexpr ValueFuture() = default;

  ValueFuture(ValueFuture&& other) = default;

  ValueFuture& operator=(ValueFuture&& other) = default;

  ~ValueFuture() PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    std::lock_guard lock(internal::ValueProviderLock());
    core_.Unlist();
  }

  Poll<> Pend(Context& cx) {
    std::lock_guard lock(internal::ValueProviderLock());
    return core_.DoPend<ValueFuture<void>>(*this, cx);
  }

  [[nodiscard]] bool is_pendable() const { return core_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return core_.is_complete(); }

  static ValueFuture Resolved() {
    return ValueFuture(FutureState::kReadyForCompletion);
  }

  static constexpr char kWaitReason[] = "ValueFuture<void>";

 private:
  friend class FutureCore;
  friend class ValueProvider<void>;
  friend class BroadcastValueProvider<void>;

  explicit ValueFuture(FutureState::ReadyForCompletion)
      : core_(FutureState::kReadyForCompletion) {}

  explicit ValueFuture(FutureState::Pending) : core_(FutureState::kPending) {}

  Poll<> DoPend(Context&) {
    if (!core_.is_ready()) {
      return Pending();
    }
    return Ready();
  }

  FutureCore core_;
};

/// A `ValueFuture` that does not return any value, just a completion signal.
using VoidFuture = ValueFuture<void>;

/// A `ValueFuture` that wraps a `std::optional`.
template <typename T>
using OptionalValueFuture = ValueFuture<std::optional<T>>;

/// A one-to-many provider for a single value.
///
/// A `BroadcastValueProvider` can vend multiple `ValueFuture` objects. When the
/// provider is resolved, all futures vended by it are completed with the same
/// value.
///
/// This provider is multi-shot: after `Resolve` is called, new futures can
/// be retrieved with `Get` to wait for the next `Resolve` event.
///
/// `BroadcastValueProvider` must resolve all futures it has vended before it is
/// destroyed. `OptionalBroadcastValueProvider`, in contrast, supports
/// cancelling its futures.
///
/// @tparam T The type of value to provide.
template <typename T>
class BroadcastValueProvider {
 public:
  constexpr BroadcastValueProvider() = default;

  BroadcastValueProvider(BroadcastValueProvider&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    std::lock_guard lock(internal::ValueProviderLock());
    list_ = std::move(other.list_);
  }

  BroadcastValueProvider& operator=(BroadcastValueProvider&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    if (this != &other) {
      std::lock_guard lock(internal::ValueProviderLock());
      PW_ASSERT(list_.empty());  // ensure any futures were resolved
      list_ = std::move(other.list_);
    }
    return *this;
  }

  BroadcastValueProvider(const BroadcastValueProvider&) = delete;
  BroadcastValueProvider& operator=(const BroadcastValueProvider&) = delete;

  ~BroadcastValueProvider() { PW_ASSERT(list_.empty()); }

  /// Returns a `ValueFuture` that will be completed when `Resolve` is called.
  ///
  /// Multiple futures can be retrieved and will pend concurrently.
  ValueFuture<T> Get() {
    ValueFuture<T> future(FutureState::kPending);
    {
      std::lock_guard lock(internal::ValueProviderLock());
      list_.Push(future.core_);
    }
    return future;
  }

  /// Resolves every pending `ValueFuture` with a copy of the provided value.
  template <typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
  void Resolve(const U& value) {
    std::lock_guard lock(internal::ValueProviderLock());
    list_.ResolveAllWith(
        [&](ValueFuture<T>& future)
            PW_NO_LOCK_SAFETY_ANALYSIS { future.ResolveLocked(value); });
  }

  /// Resolves every pending `ValueFuture`.
  template <typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
  void Resolve() {
    std::lock_guard lock(internal::ValueProviderLock());
    list_.ResolveAll();
  }

 private:
  FutureList<&ValueFuture<T>::core_> list_
      PW_GUARDED_BY(internal::ValueProviderLock());
};

/// A one-to-one provider for a single value.
///
/// An `ValueProvider` can only vend one `ValueFuture` at a time.
///
/// This provider is multi-shot: after `Resolve` is called, a new future can
/// be retrieved with `Get` to wait for the next `Resolve` event.
///
/// `ValueProvider` must resolve its future, if any, before it is destroyed.
/// `OptionalValueProvider`, in contrast, supports cancelling its future.
///
/// @tparam T The type of value to provide.
template <typename T>
class ValueProvider {
 public:
  constexpr ValueProvider() = default;

  ValueProvider(ValueProvider&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    std::lock_guard lock(internal::ValueProviderLock());
    list_ = std::move(other.list_);
  }

  ValueProvider& operator=(ValueProvider&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    if (this != &other) {
      std::lock_guard lock(internal::ValueProviderLock());
      PW_ASSERT(list_.empty());  // ensure any futures were resolved
      list_ = std::move(other.list_);
    }
    return *this;
  }

  ValueProvider(const ValueProvider&) = delete;
  ValueProvider& operator=(const ValueProvider&) = delete;

  ~ValueProvider() { PW_ASSERT(list_.empty()); }

  /// Returns a `ValueFuture` that will be completed when `Resolve` is called.
  ///
  /// If a future has already been vended and is still pending, this crashes.
  ValueFuture<T> Get() {
    ValueFuture<T> future(FutureState::kPending);
    {
      std::lock_guard lock(internal::ValueProviderLock());
      list_.PushRequireEmpty(future);
    }
    return future;
  }

  /// Returns a `ValueFuture` that will be completed when `Resolve` is called.
  ///
  /// If a future has already been vended and is still pending, this will
  /// return `std::nullopt`.
  std::optional<ValueFuture<T>> TryGet() {
    ValueFuture<T> future(FutureState::kPending);
    {
      std::lock_guard lock(internal::ValueProviderLock());
      if (!list_.PushIfEmpty(future.core_)) {
        return std::nullopt;
      }
    }
    return future;
  }

  /// Returns `true` if the provider stores a pending future.
  bool has_future() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return !list_.empty();
  }

  /// Resolves the pending `ValueFuture`, if any, by constructing its value
  /// in-place.
  template <typename... Args,
            typename U = T,
            std::enable_if_t<!std::is_void_v<U>, int> = 0>
  void Resolve(Args&&... args) {
    std::lock_guard lock(internal::ValueProviderLock());
    if (ValueFuture<T>* future = list_.PopIfAvailable(); future != nullptr) {
      future->ResolveLocked(std::forward<Args>(args)...);
    };
  }

  /// Resolves the pending `ValueFuture`.
  template <typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
  void Resolve() {
    std::lock_guard lock(internal::ValueProviderLock());
    list_.ResolveOneIfAvailable();
  }

 private:
  FutureList<&ValueFuture<T>::core_> list_
      PW_GUARDED_BY(internal::ValueProviderLock());
};

/// A `ValueProvider` that may or may not produce a value.
///
/// Adds a `Cancel()` function that resolves the pending future with
/// `std::nullopt`.
template <typename T>
class OptionalValueProvider {
 public:
  OptionalValueProvider() = default;

  OptionalValueProvider(OptionalValueProvider&&) = default;
  OptionalValueProvider& operator=(OptionalValueProvider&& other) {
    Cancel();
    provider_ = std::move(other.provider_);
    return *this;
  }

  OptionalValueProvider(const OptionalValueProvider&) = delete;
  OptionalValueProvider& operator=(const OptionalValueProvider&) = delete;

  ~OptionalValueProvider() { Cancel(); }

  /// Returns a `ValueFuture` that will be completed when `Resolve` or `Cancel`
  /// is called.
  OptionalValueFuture<T> Get() { return provider_.Get(); }

  /// Resolves the pending `ValueFuture` by constructing it in-place.
  template <typename... Args>
  void Resolve(Args&&... args) {
    provider_.Resolve(std::in_place, std::forward<Args>(args)...);
  }

  /// Resolves the pending `ValueFuture` with `std::nullopt`.
  void Cancel() { provider_.Resolve(std::nullopt); }

 private:
  ValueProvider<std::optional<T>> provider_;
};

/// A `BroadcastValueProvider` that may or may not produce a value.
///
/// Adds a `Cancel()` function that resolves all pending futures with
/// `std::nullopt`.
template <typename T>
class OptionalBroadcastValueProvider {
 public:
  OptionalBroadcastValueProvider() = default;

  OptionalBroadcastValueProvider(OptionalBroadcastValueProvider&&) = default;
  OptionalBroadcastValueProvider& operator=(
      OptionalBroadcastValueProvider&& other) {
    Cancel();
    provider_ = std::move(other.provider_);
    return *this;
  }

  OptionalBroadcastValueProvider(const OptionalBroadcastValueProvider&) =
      delete;
  OptionalBroadcastValueProvider& operator=(
      const OptionalBroadcastValueProvider&) = delete;

  ~OptionalBroadcastValueProvider() { Cancel(); }

  /// Returns a `ValueFuture` that will be completed when `Resolve` or `Cancel`
  /// is called.
  OptionalValueFuture<T> Get() { return provider_.Get(); }

  /// Resolves all pending `ValueFuture`s with the provided value.
  void Resolve(const T& value) { provider_.Resolve(value); }

  /// Resolves all pending `ValueFuture`s with `std::nullopt`.
  void Cancel() { provider_.Resolve(std::nullopt); }

 private:
  BroadcastValueProvider<std::optional<T>> provider_;
};

/// @endsubmodule

}  // namespace pw::async2
