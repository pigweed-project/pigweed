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

#include <mutex>
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

bool PendValueFutureCore(FutureCore& core, Context& cx)
    PW_EXCLUSIVE_LOCKS_REQUIRED(ValueProviderLock());

}  // namespace internal

template <typename T>
class ValueFuture;

template <typename T>
class ValueProvider;
template <typename T>
class BroadcastValueProvider;
template <typename T, typename FutureType = ValueFuture<T>>
class ValueListProvider;

template <typename DerivedFuture>
using DerivedValueListProvider =
    ValueListProvider<typename DerivedFuture::value_type, DerivedFuture>;

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
    if (internal::PendValueFutureCore(core_, cx)) {
      return Pending();
    }
    return Ready(std::move(*value_));
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
  friend class ValueProvider<T>;
  friend class BroadcastValueProvider<T>;
  template <typename DerivedFuture>
  friend class DerivedValueProvider;
  template <typename U, typename FutureType>
  friend class ValueListProvider;

  template <typename... Args>
  explicit ValueFuture(std::in_place_t, Args&&... args)
      : core_(FutureState::kReadyForCompletion),
        value_(std::in_place, std::forward<Args>(args)...) {}

  ValueFuture(FutureState::Pending) : core_(FutureState::kPending) {}

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
  using value_type = void;

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
    }
    return *this;
  }

  ~ValueFuture() PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    std::lock_guard lock(internal::ValueProviderLock());
    core_.Unlist();
  }

  Poll<> Pend(Context& cx) {
    std::lock_guard lock(internal::ValueProviderLock());
    if (internal::PendValueFutureCore(core_, cx)) {
      return Pending();
    }
    return Ready();
  }

  [[nodiscard]] bool is_pendable() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return core_.is_pendable();
  }

  [[nodiscard]] bool is_complete() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return core_.is_complete();
  }

  static ValueFuture Resolved() {
    return ValueFuture(FutureState::kReadyForCompletion);
  }

 private:
  friend class ValueProvider<void>;
  friend class BroadcastValueProvider<void>;
  template <typename DerivedFuture>
  friend class DerivedValueProvider;
  template <typename U, typename FutureType>
  friend class ValueListProvider;

  explicit ValueFuture(FutureState::ReadyForCompletion)
      : core_(FutureState::kReadyForCompletion) {}

  explicit ValueFuture(FutureState::Pending) : core_(FutureState::kPending) {}

  FutureCore core_ PW_GUARDED_BY(internal::ValueProviderLock());
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
      list_.PushRequireEmpty(future.core_);
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

 protected:
  FutureList<&ValueFuture<T>::core_> list_
      PW_GUARDED_BY(internal::ValueProviderLock());
};

/// A generic provider that vends user-defined derived futures.
///
/// `DerivedValueProvider` allows attaching custom parameters to requests by
/// using a class derived from `ValueFuture`. This enables the provider to
/// inspect the parameters (e.g., requested size) and conditionally fulfill
/// requests via `ResolveIf`.
///
/// @tparam DerivedFuture The user-defined future type. It must inherit from
///                       `ValueFuture<T>` and provide a constructor that
///                       accepts `ValueFuture<T>&&` as its first argument.
template <typename DerivedFuture>
class DerivedValueProvider final
    : private ValueProvider<typename DerivedFuture::value_type> {
  using T = typename DerivedFuture::value_type;

  static_assert(std::is_base_of_v<ValueFuture<T>, DerivedFuture>,
                "DerivedFuture must derive from ValueFuture");

 public:
  constexpr DerivedValueProvider() = default;

  using ValueProvider<T>::has_future;
  using ValueProvider<T>::Resolve;

  /// Vends a derived future.
  template <typename... Args>
  DerivedFuture Get(Args&&... args) {
    DerivedFuture future(ValueFuture<T>(FutureState::kPending),
                         std::forward<Args>(args)...);
    {
      std::lock_guard lock(internal::ValueProviderLock());
      this->list_.PushRequireEmpty(future.core_);
    }
    return future;
  }

  /// Vends a derived future if none is currently pending.
  template <typename... Args>
  std::optional<DerivedFuture> TryGet(Args&&... args) {
    DerivedFuture future(ValueFuture<T>(FutureState::kPending),
                         std::forward<Args>(args)...);
    {
      std::lock_guard lock(internal::ValueProviderLock());
      if (!this->list_.PushIfEmpty(future.core_)) {
        return std::nullopt;
      }
    }
    return future;
  }

  /// Atomically inspects the derived future and resolves it if the callback
  /// returns a value indicating fulfillment.
  ///
  /// The callback receives a reference to the `DerivedFuture` and should
  /// return a value indicating whether the request can be fulfilled:
  ///
  /// - For non-void futures (producing `T`), the callback should return
  ///   `std::optional<T>`. Returning `std::nullopt` leaves the future pending.
  /// - For void futures, the callback should return `bool`. Returning `false`
  ///   leaves the future pending.
  ///
  /// @param callback A callable (lambda, function pointer, etc.) invoked with
  ///                 a reference to the `DerivedFuture`.
  /// @returns `true` if the future was resolved, `false` otherwise.
  template <typename F>
  bool ResolveIf(F&& callback) {
    std::lock_guard lock(internal::ValueProviderLock());
    if (this->list_.empty()) {
      return false;
    }

    DerivedFuture& derived_future =
        static_cast<DerivedFuture&>(this->list_.front());

    if constexpr (std::is_void_v<T>) {
      if (callback(derived_future)) {
        this->list_.Pop();
        derived_future.core_.WakeAndMarkReady();
        return true;
      }
    } else {
      auto value_to_resolve = callback(derived_future);
      if (value_to_resolve.has_value()) {
        this->list_.Pop();
        derived_future.ResolveLocked(std::move(*value_to_resolve));
        return true;
      }
    }

    return false;
  }
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

/// A general purpose, multi-consumer provider that manages a list of pending
/// futures.
///
/// `ValueListProvider` allows multiple distinct tasks (potentially running on
/// different dispatchers) to register futures in a list. The provider owner
/// can inspect, conditionally resolve, or bulk-abort pending futures from
/// anywhere in the list.
///
/// @tparam T The type of value provided by the futures.
/// @tparam FutureType The type of future vended, defaulting to
/// `ValueFuture<T>`.
template <typename T, typename FutureType>
class ValueListProvider {
 public:
  constexpr ValueListProvider() = default;

  ValueListProvider(ValueListProvider&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    std::lock_guard lock(internal::ValueProviderLock());
    list_ = std::move(other.list_);
  }

  ValueListProvider& operator=(ValueListProvider&& other) noexcept
      PW_LOCKS_EXCLUDED(internal::ValueProviderLock()) {
    if (this != &other) {
      std::lock_guard lock(internal::ValueProviderLock());
      PW_ASSERT(list_.empty());  // ensure any futures were resolved
      list_ = std::move(other.list_);
    }
    return *this;
  }

  ValueListProvider(const ValueListProvider&) = delete;
  ValueListProvider& operator=(const ValueListProvider&) = delete;

  ~ValueListProvider() { PW_ASSERT(list_.empty()); }

  /// Vends a future and automatically registers it in the pending list.
  template <typename... Args>
  FutureType Get(Args&&... args) {
    FutureType future(ValueFuture<T>(FutureState::kPending),
                      std::forward<Args>(args)...);
    {
      std::lock_guard lock(internal::ValueProviderLock());
      list_.Push(future.core_);
    }
    return future;
  }

  /// Returns `true` if the list contains no pending futures.
  bool empty() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return list_.empty();
  }

  /// Returns the number of pending futures.
  size_t size() const {
    std::lock_guard lock(internal::ValueProviderLock());
    return list_.size();
  }

  /// Resolves the first (oldest) pending future in the list.
  template <typename... Args,
            typename U = T,
            std::enable_if_t<!std::is_void_v<U>, int> = 0>
  void ResolveFirst(Args&&... args) {
    std::lock_guard lock(internal::ValueProviderLock());
    if (ValueFuture<T>* future = list_.PopIfAvailable(); future != nullptr) {
      future->ResolveLocked(std::forward<Args>(args)...);
    }
  }

  /// Resolves the first (oldest) pending future in the list.
  template <typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
  void ResolveFirst() {
    std::lock_guard lock(internal::ValueProviderLock());
    list_.ResolveOneIfAvailable();
  }

  /// Iterates through the list and resolves the FIRST future where the callback
  /// returns a value (or true for void).
  ///
  /// @returns `true` if a future was resolved and removed, `false` otherwise.
  template <typename F>
  bool ResolveFirstMatching(F&& callback) {
    std::lock_guard lock(internal::ValueProviderLock());
    auto previous = list_.before_begin();
    auto current = list_.begin();
    while (current != list_.end()) {
      ValueFuture<T>* base_future =
          pw::ContainerOf<&ValueFuture<T>::core_>(&(*current));
      FutureType& future = static_cast<FutureType&>(*base_future);

      if constexpr (std::is_void_v<T>) {
        if (callback(future)) {
          list_.erase_after(previous);
          future.core_.WakeAndMarkReady();
          return true;
        }
      } else {
        auto value_to_resolve = callback(future);
        if (value_to_resolve.has_value()) {
          list_.erase_after(previous);
          future.ResolveLocked(std::move(*value_to_resolve));
          return true;
        }
      }
      previous = current;
      ++current;
    }
    return false;
  }

  /// Iterates through the list and resolves ALL futures where the callback
  /// returns a value (or true for void).
  ///
  /// @returns The number of futures resolved and removed.
  template <typename F>
  size_t ResolveAllMatching(F&& callback) {
    std::lock_guard lock(internal::ValueProviderLock());
    size_t count = 0;
    auto previous = list_.before_begin();
    auto current = list_.begin();
    while (current != list_.end()) {
      ValueFuture<T>* base_future =
          pw::ContainerOf<&ValueFuture<T>::core_>(&(*current));
      FutureType& future = static_cast<FutureType&>(*base_future);

      if constexpr (std::is_void_v<T>) {
        if (callback(future)) {
          auto next = current;
          ++next;
          list_.erase_after(previous);
          future.core_.WakeAndMarkReady();
          count++;
          current = next;
          continue;
        }
      } else {
        auto value_to_resolve = callback(future);
        if (value_to_resolve.has_value()) {
          auto next = current;
          ++next;
          list_.erase_after(previous);
          future.ResolveLocked(std::move(*value_to_resolve));
          count++;
          current = next;
          continue;
        }
      }
      previous = current;
      ++current;
    }
    return count;
  }

  /// Bulk-resolves all pending futures in the list.
  template <typename F>
  void ResolveAll(F&& callback) {
    std::lock_guard lock(internal::ValueProviderLock());
    while (!list_.empty()) {
      ValueFuture<T>& base_future = list_.Pop();
      FutureType& future = static_cast<FutureType&>(base_future);
      if constexpr (std::is_void_v<T>) {
        callback(future);
        future.core_.WakeAndMarkReady();
      } else {
        future.ResolveLocked(callback(future));
      }
    }
  }

 private:
  FutureList<&ValueFuture<T>::core_> list_
      PW_GUARDED_BY(internal::ValueProviderLock());
};

/// @endsubmodule

}  // namespace pw::async2
