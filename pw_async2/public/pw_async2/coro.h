// Copyright 2024 The Pigweed Authors
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

#include <concepts>
#include <coroutine>
#include <variant>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_async2/dispatcher.h"
#include "pw_function/function.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::async2 {

/// @submodule{pw_async2,coroutines}

// Forward-declare `Coro` so that it can be referenced by the promise type APIs.
template <std::constructible_from<pw::Status> T>
class Coro;

/// Context required for creating and executing coroutines.
class CoroContext {
 public:
  /// Creates a `CoroContext` which will allocate coroutine state using
  /// `alloc`.
  explicit CoroContext(pw::allocator::Allocator& alloc) : alloc_(alloc) {}
  pw::allocator::Allocator& alloc() const { return alloc_; }

 private:
  pw::allocator::Allocator& alloc_;
};

/// @endsubmodule

// The internal coroutine API implementation details enabling `Coro<T>`.
//
// Users of `Coro<T>` need not concern themselves with these details, unless
// they think it sounds like fun ;)
namespace internal {

void LogCoroAllocationFailure(size_t requested_size);

template <typename T>
class OptionalWrapper final {
 public:
  // Create an empty container for a to-be-provided value.
  OptionalWrapper() : value_() {}

  // Assign a value.
  template <typename U>
  OptionalWrapper& operator=(U&& value) {
    value_ = std::forward<U>(value);
    return *this;
  }

  // Retrieve the inner value.
  //
  // This operation will fail if no value was assigned.
  operator T() {
    PW_ASSERT(value_.has_value());
    return *value_;
  }

 private:
  std::optional<T> value_;
};

// A container for a to-be-produced value of type `T`.
//
// This is designed to allow avoiding the overhead of `std::optional` when
// `T` is default-initializable.
//
// Values of this type begin as either:
// - a default-initialized `T` if `T` is default-initializable or
// - `std::nullopt`
template <typename T>
using OptionalOrDefault =
    std::conditional<std::is_default_constructible<T>::value,
                     T,
                     OptionalWrapper<T>>::type;

// A wrapper for `std::coroutine_handle` that assumes unique ownership of the
// underlying `PromiseType`.
//
// This type will `destroy()` the underlying promise in its destructor, or
// when `Release()` is called.
template <typename PromiseType>
class OwningCoroutineHandle final {
 public:
  // Construct a null (`!IsValid()`) handle.
  OwningCoroutineHandle(std::nullptr_t) : promise_handle_(nullptr) {}

  /// Take ownership of `promise_handle`.
  OwningCoroutineHandle(std::coroutine_handle<PromiseType>&& promise_handle)
      : promise_handle_(std::move(promise_handle)) {}

  // Empty out `other` and transfers ownership of its `promise_handle`
  // to `this`.
  OwningCoroutineHandle(OwningCoroutineHandle&& other)
      : promise_handle_(std::move(other.promise_handle_)) {
    other.promise_handle_ = nullptr;
  }

  // Empty out `other` and transfers ownership of its `promise_handle`
  // to `this`.
  OwningCoroutineHandle& operator=(OwningCoroutineHandle&& other) {
    Release();
    promise_handle_ = std::move(other.promise_handle_);
    other.promise_handle_ = nullptr;
    return *this;
  }

  // `destroy()`s the underlying `promise_handle` if valid.
  ~OwningCoroutineHandle() { Release(); }

  // Return whether or not this value contains a `promise_handle`.
  //
  // This will return `false` if this `OwningCoroutineHandle` was
  // `nullptr`-initialized, moved from, or if `Release` was invoked.
  [[nodiscard]] bool IsValid() const {
    return promise_handle_.address() != nullptr;
  }

  // Return a reference to the underlying `PromiseType`.
  //
  // Precondition: `IsValid()` must be `true`.
  [[nodiscard]] PromiseType& promise() const {
    return promise_handle_.promise();
  }

  // Whether or not the underlying coroutine has completed.
  //
  // Precondition: `IsValid()` must be `true`.
  [[nodiscard]] bool done() const { return promise_handle_.done(); }

  // Resume the underlying coroutine.
  //
  // Precondition: `IsValid()` must be `true`, and `done()` must be
  // `false`.
  void resume() { promise_handle_.resume(); }

  // Invokes `destroy()` on the underlying promise and deallocates its
  // associated storage.
  void Release() {
    // DOCSTAG: [pw_async2-coro-release]
    void* address = promise_handle_.address();
    if (address != nullptr) {
      pw::allocator::Deallocator& dealloc = promise_handle_.promise().dealloc_;
      promise_handle_.destroy();
      promise_handle_ = nullptr;
      dealloc.Deallocate(address);
    }
    // DOCSTAG: [pw_async2-coro-release]
  }

 private:
  std::coroutine_handle<PromiseType> promise_handle_;
};

// Forward-declare the wrapper type for values passed to `co_await`.
template <typename Pendable, typename PromiseType>
class Awaitable;

// A container for values passed in and out of the promise.
//
// The C++20 coroutine `resume()` function cannot accept arguments no return
// values, so instead coroutine inputs and outputs are funneled through this
// type. A pointer to the `InOut` object is stored in the `CoroPromiseType`
// so that the coroutine object can access it.
template <typename T>
struct InOut final {
  // The `Context` passed into the coroutine via `Pend`.
  Context* input_cx;

  // The output assigned to by the coroutine if the coroutine is `done()`.
  OptionalOrDefault<T>* output;
};

// Attempt to complete the current pendable value passed to `co_await`,
// storing its return value inside the `Awaitable` object so that it can
// be retrieved by the coroutine.
//
// Each `co_await` statement creates an `Awaitable` object whose `Pend`
// method must be completed before the coroutine's `resume()` function can
// be invoked.
//
// `sizeof(void*)` is used as the size since only one pointer capture is
// required in all cases.
using PendFillReturnValueFn =
    pw::InlineFunction<Poll<>(Context&), sizeof(void*)>;

// The `promise_type` of `Coro<T>`.
//
// To understand this type, it may be necessary to refer to the reference
// documentation for the C++20 coroutine API.
template <typename T>
class CoroPromiseType final {
 public:
  // Construct the `CoroPromiseType` using the arguments passed to a
  // function returning `Coro<T>`.
  //
  // The first argument *must* be a `CoroContext`. The other
  // arguments are unused, but must be accepted in order for this to compile.
  template <typename... Args>
  CoroPromiseType(CoroContext& cx, const Args&...)
      : dealloc_(cx.alloc()), currently_pending_(nullptr), in_out_(nullptr) {}

  // Method-receiver version.
  template <typename MethodReceiver, typename... Args>
  CoroPromiseType(const MethodReceiver&, CoroContext& cx, const Args&...)
      : dealloc_(cx.alloc()), currently_pending_(nullptr), in_out_(nullptr) {}

  // Get the `Coro<T>` after successfully allocating the coroutine space
  // and constructing `this`.
  Coro<T> get_return_object();

  // Do not begin executing the `Coro<T>` until `resume()` has been invoked
  // for the first time.
  std::suspend_always initial_suspend() { return {}; }

  // Unconditionally suspend to prevent `destroy()` being invoked.
  //
  // The caller of `resume()` needs to first observe `done()` before the
  // state can be destroyed.
  //
  // Setting this to suspend means that the caller is responsible for invoking
  // `destroy()`.
  std::suspend_always final_suspend() noexcept { return {}; }

  // Store the `co_return` argument in the `InOut<T>` object provided by
  // the `Pend` wrapper.
  template <std::convertible_to<T> From>
  void return_value(From&& value) {
    *in_out_->output = std::forward<From>(value);
  }

  // Ignore exceptions in coroutines.
  //
  // Pigweed is not designed to be used with exceptions: `Result` or a
  // similar type should be used to propagate errors.
  void unhandled_exception() { PW_ASSERT(false); }

  // Create an invalid (nullptr) `Coro<T>` if `operator new` below fails.
  static Coro<T> get_return_object_on_allocation_failure();

  // Allocate the space for both this `CoroPromiseType<T>` and the coroutine
  // state.
  //
  // This override does not accept alignment.
  template <typename... Args>
  static void* operator new(std::size_t size,
                            CoroContext& coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, alignof(std::max_align_t));
  }

  // Allocate the space for both this `CoroPromiseType<T>` and the coroutine
  // state.
  //
  // This override accepts alignment.
  template <typename... Args>
  static void* operator new(std::size_t size,
                            std::align_val_t align,
                            CoroContext& coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, static_cast<size_t>(align));
  }

  // Method-receiver form.
  //
  // This override does not accept alignment.
  template <typename MethodReceiver, typename... Args>
  static void* operator new(std::size_t size,
                            const MethodReceiver&,
                            CoroContext& coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, alignof(std::max_align_t));
  }

  // Method-receiver form.
  //
  // This accepts alignment.
  template <typename MethodReceiver, typename... Args>
  static void* operator new(std::size_t size,
                            std::align_val_t align,
                            const MethodReceiver&,
                            CoroContext& coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, static_cast<size_t>(align));
  }

  static void* SharedNew(CoroContext& coro_cx,
                         std::size_t size,
                         std::size_t align) noexcept {
    auto ptr = coro_cx.alloc().Allocate(pw::allocator::Layout(size, align));
    if (ptr == nullptr) {
      internal::LogCoroAllocationFailure(size);
    }
    return ptr;
  }

  // Deallocate the space for both this `CoroPromiseType<T>` and the
  // coroutine state.
  //
  // In reality, we do nothing here!!!
  //
  // Coroutines do not support `destroying_delete`, so we can't access
  // `dealloc_` here, and therefore have no way to deallocate.
  // Instead, deallocation is handled by `OwningCoroutineHandle<T>::Release`.
  static void operator delete(void*) {}

  // Handle a `co_await` call by accepting a type with a
  // `Poll<U> Pend(Context&)` method, returning an `Awaitable` which will
  // yield a `U` once complete.
  template <typename Pendable>
    requires(!std::is_reference_v<Pendable>)
  Awaitable<Pendable, CoroPromiseType> await_transform(Pendable&& pendable) {
    return pendable;
  }

  template <typename Pendable>
  Awaitable<Pendable*, CoroPromiseType> await_transform(Pendable& pendable) {
    return &pendable;
  }

  // Returns a reference to the `Context` passed in.
  Context& cx() { return *in_out_->input_cx; }

  pw::allocator::Deallocator& dealloc_;
  PendFillReturnValueFn currently_pending_;
  InOut<T>* in_out_;
};

// The object created by invoking `co_await` in a `Coro<T>` function.
//
// This wraps a `Pendable` type and implements the awaitable interface
// expected by the standard coroutine API.
template <typename Pendable, typename PromiseType>
class Awaitable final {
 public:
  // The `value_type` in `Poll<value_type> Pendable::Pend(Context&)`.
  using value_type = std::remove_cvref_t<
      decltype(std::declval<std::remove_pointer_t<Pendable>>()
                   .Pend(std::declval<Context&>())
                   .value())>;

  Awaitable(Pendable&& pendable) : state_(std::forward<Pendable>(pendable)) {}

  // Confirms that `await_suspend` must be invoked.
  bool await_ready() { return false; }

  // Returns whether or not the current coroutine should be suspended.
  //
  // This is invoked once as part of every `co_await` call after
  // `await_ready` returns `false`.
  //
  // In the process, this method attempts to complete the inner `Pendable`
  // before suspending this coroutine.
  bool await_suspend(const std::coroutine_handle<PromiseType>& promise) {
    Context& cx = promise.promise().cx();
    if (PendFillReturnValue(cx).IsPending()) {
      /// The coroutine should suspend since the await-ed thing is pending.
      promise.promise().currently_pending_ = [this](Context& lambda_cx) {
        return PendFillReturnValue(lambda_cx);
      };
      return true;
    }
    return false;
  }

  // Returns `return_value`.
  //
  // This is automatically invoked by the language runtime when the promise's
  // `resume()` method is called.
  value_type&& await_resume() {
    return std::move(std::get<value_type>(state_));
  }

  auto& PendableNoPtr() {
    if constexpr (std::is_pointer_v<Pendable>) {
      return *std::get<Pendable>(state_);
    } else {
      return std::get<Pendable>(state_);
    }
  }

  // Attempts to complete the `Pendable` value, storing its return value
  // upon completion.
  //
  // This method must return `Ready()` before the coroutine can be safely
  // resumed, as otherwise the return value will not be available when
  // `await_resume` is called to produce the result of `co_await`.
  Poll<> PendFillReturnValue(Context& cx) {
    Poll<value_type> poll_res(PendableNoPtr().Pend(cx));
    if (poll_res.IsPending()) {
      return Pending();
    }
    state_ = std::move(*poll_res);
    return Ready();
  }

 private:
  std::variant<Pendable, value_type> state_;
};

}  // namespace internal

/// @submodule{pw_async2,coroutines}

/// An asynchronous coroutine which implements the C++20 coroutine API.
///
/// # Why coroutines?
/// Coroutines allow a series of asynchronous operations to be written as
/// straight line code. Rather than manually writing a state machine, users can
/// `co_await` any Pigweed asynchronous value (types with a
/// `Poll<T> Pend(Context&)` method).
///
/// # Allocation
/// Pigweed's `Coro<T>` API supports checked, fallible, heap-free allocation.
/// The first argument to any coroutine function must be a
/// `CoroContext` (or a reference to one). This allows the
/// coroutine to allocate space for asynchronously-held stack variables using
/// the allocator member of the `CoroContext`.
///
/// Failure to allocate coroutine "stack" space will result in the `Coro<T>`
/// returning `Status::Invalid()`.
///
/// # Creating a coroutine function
/// To create a coroutine, a function must:
/// - Have an annotated return type of `Coro<T>` where `T` is some type
///   constructible from `pw::Status`, such as `pw::Status` or
///   `pw::Result<U>`.
/// - Use `co_return <value>` rather than `return <value>` for any
///   `return` statements. This also requires the use of `PW_CO_TRY` and
///   `PW_CO_TRY_ASSIGN` rather than `PW_TRY` and `PW_TRY_ASSIGN`.
/// - Accept a value convertible to `pw::allocator::Allocator&` as its first
///   argument. This allocator will be used to allocate storage for coroutine
///   stack variables held across a `co_await` point.
///
/// # Using co_await
/// Inside a coroutine function, `co_await <expr>` can be used on any type
/// with a `Poll<T> Pend(Context&)` method. The result will be a value of
/// type `T`.
template <std::constructible_from<pw::Status> T>
class Coro final {
 public:
  /// Creates an empty, invalid coroutine object.
  static Coro Empty() {
    return Coro(internal::OwningCoroutineHandle<promise_type>(nullptr));
  }

  /// Whether or not this `Coro<T>` is a valid coroutine.
  ///
  /// This will return `false` if coroutine state allocation failed or if
  /// this `Coro<T>::Pend` method previously returned a `Ready` value.
  [[nodiscard]] bool IsValid() const { return promise_handle_.IsValid(); }

  /// Attempt to complete this coroutine, returning the result if complete.
  ///
  /// Returns `Status::Internal()` if `!IsValid()`, which may occur if
  /// coroutine state allocation fails.
  Poll<T> Pend(Context& cx) {
    if (!IsValid()) {
      // This coroutine failed to allocate its internal state.
      // (Or `Pend` is being erroniously invoked after previously completing.)
      return Ready(Status::Internal());
    }

    // If an `Awaitable` value is currently being processed, it must be
    // allowed to complete and store its return value before we can resume
    // the coroutine.
    if (promise_handle_.promise().currently_pending_ != nullptr &&
        promise_handle_.promise().currently_pending_(cx).IsPending()) {
      return Pending();
    }
    // DOCSTAG: [pw_async2-coro-resume]
    // Create the arguments (and output storage) for the coroutine.
    internal::InOut<T> in_out;
    internal::OptionalOrDefault<T> return_value;
    in_out.input_cx = &cx;
    in_out.output = &return_value;
    promise_handle_.promise().in_out_ = &in_out;

    // Resume the coroutine, triggering `Awaitable::await_resume()` and the
    // returning of the resulting value from `co_await`.
    promise_handle_.resume();
    if (!promise_handle_.done()) {
      return Pending();
    }

    // Destroy the coroutine state: it has completed, and further calls to
    // `resume` would result in undefined behavior.
    promise_handle_.Release();

    // When the coroutine completed in `resume()` above, it stored its
    // `co_return` value into `return_value`. This retrieves that value.
    return return_value;
    // DOCSTAG: [pw_async2-coro-resume]
  }

  /// Used by the compiler in order to create a `Coro<T>` from a coroutine
  /// function.
  using promise_type = ::pw::async2::internal::CoroPromiseType<T>;

 private:
  // Allow `CoroPromiseType<T>::get_return_object()` and
  // `CoroPromiseType<T>::get_retunr_object_on_allocation_failure()` to
  // use the private constructor below.
  friend promise_type;

  /// Create a new `Coro<T>` using a (possibly null) handle.
  Coro(internal::OwningCoroutineHandle<promise_type>&& promise_handle)
      : promise_handle_(std::move(promise_handle)) {}

  internal::OwningCoroutineHandle<promise_type> promise_handle_;
};

/// @endsubmodule

// Implement the remaining internal pieces that require a definition of
// `Coro<T>`.
namespace internal {

template <typename T>
Coro<T> CoroPromiseType<T>::get_return_object() {
  return internal::OwningCoroutineHandle<CoroPromiseType<T>>(
      std::coroutine_handle<CoroPromiseType<T>>::from_promise(*this));
}

template <typename T>
Coro<T> CoroPromiseType<T>::get_return_object_on_allocation_failure() {
  return internal::OwningCoroutineHandle<CoroPromiseType<T>>(nullptr);
}

}  // namespace internal
}  // namespace pw::async2
