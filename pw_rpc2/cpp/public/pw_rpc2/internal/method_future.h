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

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_async2/box.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_bytes/alignment.h"

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
#include "pw_async2/coro.h"
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

namespace pw::async2 {
template <typename T>
class Coro;
}  // namespace pw::async2

namespace pw::rpc2::internal {

/// In-place `BoxedFutureBase<void>` implementation for RPC method futures.
///
/// Constructs `Fut` directly in `ServerCall::future_storage()` via a factory
/// callable using C++17 mandatory copy elision, avoiding stack temporaries,
/// move constructors, and moved-from destructors.
template <typename Fut>
class MethodFutureImpl final : public async2::internal::BoxedFutureBase<void> {
 public:
  template <typename Factory,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<Factory>, MethodFutureImpl> &&
                !std::is_same_v<std::decay_t<Factory>, Fut>>>
  explicit MethodFutureImpl(Factory&& factory)
      : future_(std::forward<Factory>(factory)()) {}

  bool is_pendable() const override { return future_.is_pendable(); }
  bool is_complete() const override { return future_.is_complete(); }

  async2::Poll<void> Pend(async2::Context& cx) override {
    if constexpr (std::is_same_v<Fut, async2::Coro<void>>) {
#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
      // Advance via Awaitable<Coro<void>*, void> so nested coroutine allocation
      // failures return Ready() with is_complete() == false instead of crashing
      // in Coro<void>::Pend().
      async2::internal::Awaitable<async2::Coro<void>*, void> awaitable(
          &future_);
      switch (awaitable.Advance(cx)) {
        case async2::internal::CoroPollState::kPending:
          return async2::Pending();
        case async2::internal::CoroPollState::kAborted:
          future_ = async2::Coro<void>();
          return async2::Ready();
        case async2::internal::CoroPollState::kReady:
          return async2::Ready();
      }
      PW_UNREACHABLE;
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
    } else {
      return future_.Pend(cx);
    }
  }

 private:
  Fut future_;
};

/// A type-erased future that drives an in-place constructed future.
///
/// Unlike `pw::async2::BoxedFuture`, which dynamically allocates memory using
/// an allocator for the boxed future implementation, `BoxedMethodFuture` points
/// to an already allocated/constructed `BoxedFutureBase<void>`.
///
/// `BoxedMethodFuture` owns the lifecycle of the underlying
/// `BoxedFutureBase<void>` but not its allocation. When `BoxedMethodFuture`
/// completes or is destroyed, it calls the virtual destructor
/// `impl_->~BoxedFutureBase()`. It does not deallocate the underlying memory.
class BoxedMethodFuture {
 public:
  using value_type = void;

  /// Constructs an empty `BoxedMethodFuture`.
  constexpr BoxedMethodFuture() = default;

  /// Constructs a `BoxedMethodFuture` wrapping an existing
  /// `BoxedFutureBase<void>`.
  explicit BoxedMethodFuture(async2::internal::BoxedFutureBase<void>* impl)
      : impl_(impl) {}

  /// Destroys the underlying future in-place if still active.
  ~BoxedMethodFuture() { Reset(); }

  BoxedMethodFuture(const BoxedMethodFuture&) = delete;
  BoxedMethodFuture& operator=(const BoxedMethodFuture&) = delete;

  BoxedMethodFuture(BoxedMethodFuture&& other) noexcept
      : impl_(std::exchange(other.impl_, nullptr)) {}

  BoxedMethodFuture& operator=(BoxedMethodFuture&& other) noexcept {
    if (this != &other) {
      Reset();
      impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
  }

  /// Returns whether `Pend()` can be called.
  [[nodiscard]] bool is_pendable() const {
    return has_active_impl() && impl_->is_pendable();
  }

  /// Returns whether the future has completed.
  ///
  /// Once the future has completed, this remains `true` even if the underlying
  /// future implementation has been destroyed.
  [[nodiscard]] bool is_complete() const {
    return impl_ == CompletedSentinel() ||
           (impl_ != nullptr && impl_->is_complete());
  }

  /// Advances the future.
  async2::Poll<void> Pend(async2::Context& cx) {
    PW_ASSERT(is_pendable());
    auto poll = impl_->Pend(cx);
    if (poll.IsReady()) {
      const bool completed = impl_->is_complete();
      Reset();
      if (completed) {
        impl_ = CompletedSentinel();
      }
    }
    return poll;
  }

  /// Destroys the underlying future, releasing any resources it holds.
  ///
  /// The memory backing the future is owned by the caller and is not released.
  /// This does not clear the completion state: a future which has already
  /// completed still reports `is_complete()`.
  void Reset() {
    if (has_active_impl()) {
      impl_->~BoxedFutureBase();
      impl_ = nullptr;
    }
  }

  /// Constructs a `MethodFutureImpl<Fut>` in `buffer` via `factory` and returns
  /// a `BoxedMethodFuture` wrapping it.
  template <typename Fut, typename Factory>
  static BoxedMethodFuture Emplace(void* buffer, Factory&& factory) {
    static_assert(async2::Future<Fut>, "Fut must satisfy pw::async2::Future");
    static_assert(std::is_same_v<typename Fut::value_type, void>,
                  "Fut::value_type must be void");
    using ImplType = MethodFutureImpl<Fut>;
    PW_DASSERT(::pw::IsAlignedAs<ImplType>(buffer));
    auto* impl = ::new (buffer) ImplType(std::forward<Factory>(factory));
    return BoxedMethodFuture(impl);
  }

 private:
  static async2::internal::BoxedFutureBase<void>* CompletedSentinel() {
    return reinterpret_cast<async2::internal::BoxedFutureBase<void>*>(
        uintptr_t{1});
  }

  bool has_active_impl() const {
    return impl_ != nullptr && impl_ != CompletedSentinel();
  }

  async2::internal::BoxedFutureBase<void>* impl_ = nullptr;
};
static_assert(async2::Future<BoxedMethodFuture>);
static_assert(sizeof(BoxedMethodFuture) == sizeof(void*));

}  // namespace pw::rpc2::internal
