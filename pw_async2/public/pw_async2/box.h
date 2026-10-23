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

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
namespace pw::async2 {

namespace internal {

template <typename T>
class BoxedFutureBase {
 public:
  using value_type = T;

  BoxedFutureBase() = default;
  BoxedFutureBase(const BoxedFutureBase&) = delete;
  BoxedFutureBase& operator=(const BoxedFutureBase&) = delete;
  BoxedFutureBase(BoxedFutureBase&&) = default;
  BoxedFutureBase& operator=(BoxedFutureBase&&) = default;

  virtual ~BoxedFutureBase() = default;
  virtual bool is_pendable() const = 0;
  virtual bool is_complete() const = 0;
  virtual Poll<T> Pend(Context& cx) = 0;
};

template <typename T, typename FutureType>
class BoxedFutureImpl final : public BoxedFutureBase<T> {
 public:
  explicit BoxedFutureImpl(FutureType&& future) : future_(std::move(future)) {}

  bool is_pendable() const override { return future_.is_pendable(); }
  bool is_complete() const override { return future_.is_complete(); }
  Poll<T> Pend(Context& cx) override { return future_.Pend(cx); }

 private:
  FutureType future_;
};

}  // namespace internal

/// A type-erased future that produces a value of type `T`.
///
/// `BoxedFuture` wraps another future, storing it dynamically via a
/// `pw::Allocator`. This hides the concrete type of the future, which is
/// useful when storing or returning futures of different concrete types.
template <typename T>
class BoxedFuture final {
 public:
  using value_type = T;

  /// Constructs an empty `BoxedFuture`.
  BoxedFuture() : future_(nullptr) { state_.completed = false; }

  BoxedFuture(const BoxedFuture&) = delete;
  BoxedFuture& operator=(const BoxedFuture&) = delete;

  BoxedFuture(BoxedFuture&& other) noexcept : future_(other.future_) {
    MoveFrom(other);
  }

  BoxedFuture& operator=(BoxedFuture&& other) noexcept {
    Destroy();
    future_ = other.future_;
    MoveFrom(other);
    return *this;
  }

  ~BoxedFuture() { Destroy(); }

  /// Returns whether `Pend()` can be called.
  bool is_pendable() const {
    return future_ != nullptr && future_->is_pendable();
  }

  /// Returns whether the future has completed.
  bool is_complete() const {
    return future_ == nullptr ? state_.completed : future_->is_complete();
  }

  /// Advances the future.
  Poll<T> Pend(Context& cx) {
    PW_ASSERT(future_ != nullptr);
    Poll<T> result = future_->Pend(cx);
    if (result.IsReady()) {
      Destroy();
      future_ = nullptr;
      state_.completed = true;
    }
    return result;
  }

 private:
  template <typename FutureType>
  friend BoxedFuture<typename std::decay_t<FutureType>::value_type> BoxFuture(
      Allocator& alloc, FutureType&& future);

  explicit BoxedFuture(internal::BoxedFutureBase<T>* future,
                       Allocator* allocator)
      : future_(future) {
    state_.allocator = allocator;
  }

  void MoveFrom(BoxedFuture& other) {
    if (future_ != nullptr) {
      state_.allocator = other.state_.allocator;
    } else {
      state_.completed = other.state_.completed;
    }
    other.future_ = nullptr;
    other.state_.completed = false;
  }

  void Destroy() {
    if (future_ != nullptr) {
      Allocator* alloc = state_.allocator;
      future_->~BoxedFutureBase();
      alloc->Deallocate(future_);
    }
  }

  internal::BoxedFutureBase<T>* future_;
  union State {
    Allocator* allocator;
    bool completed;
  } state_;
};

/// Type-erases a future, wrapping it in a `BoxedFuture` allocated using the
/// specified allocator.
///
/// Example:
/// @code{.cpp}
///   BoxedFuture<int> future =
///       BoxFuture(allocator, SomethingReturningAFuture());
///   if (future.is_pendable()) {
///     // Use the future
///   } else {
///     // Handle allocation failure
///   }
/// @endcode
template <typename FutureType>
BoxedFuture<typename std::decay_t<FutureType>::value_type> BoxFuture(
    Allocator& alloc, FutureType&& future) {
  static_assert(!std::is_lvalue_reference_v<FutureType>);
  using ValueType = typename std::decay_t<FutureType>::value_type;
  using ImplType =
      internal::BoxedFutureImpl<ValueType, std::decay_t<FutureType>>;
  auto ptr = alloc.New<ImplType>(std::forward<FutureType>(future));
  if (ptr == nullptr) {
    return BoxedFuture<ValueType>();
  }
  return BoxedFuture<ValueType>(ptr, &alloc);
}

}  // namespace pw::async2
