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
#include <cstddef>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_assert/assert.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_containers/internal/optional.h"

namespace pw::async2 {
namespace internal {

[[noreturn]] void CrashDueToCoroutineAllocationFailure();

}  // namespace internal

// Forward-declare `Coro` so that it can be referenced by the promise type APIs.
template <typename T>
class Coro;

enum class ReturnValuePolicy : bool;

/// @submodule{pw_async2,coroutines}

/// Context required for creating and executing coroutines.
class CoroContext {
 public:
  /// Creates a `CoroContext`, which uses the provided allocator to allocate
  /// coroutine state. A `CoroContext` may be used to invoke other coroutines.
  /// Its allocator can be used for other allocations, if desired.
  ///
  /// Supports implicit conversion to simplify creating coroutines.
  constexpr CoroContext(Allocator& allocator) : allocator_(&allocator) {}

  constexpr CoroContext(const CoroContext&) = default;
  constexpr CoroContext& operator=(const CoroContext&) = default;

  constexpr Allocator& allocator() const { return *allocator_; }

 private:
  Allocator* allocator_;
};

/// @endsubmodule

// The internal coroutine API implementation details enabling `Coro<T>`.
//
// Users of `Coro<T>` need not concern themselves with these details, unless
// they think it sounds like fun ;)
namespace internal {

/// A wrapper for `std::coroutine_handle` that assumes unique ownership of the
/// underlying `PromiseType`. This is an internal type and not part of the
/// public `pw_async2` API.
///
/// This type will `destroy()` the underlying promise in its destructor, or
/// when `Release()` is called.
template <typename PromiseType>
class OwningCoroutineHandle final {
 public:
  // Construct a null (`!IsValid()`) handle.
  constexpr OwningCoroutineHandle(std::nullptr_t) : promise_handle_(nullptr) {}

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
      Deallocator& dealloc = promise_handle_.promise().deallocator();
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
template <typename CoroOrFuture, typename PromiseType>
class Awaitable;

template <typename T>
struct is_coro : std::false_type {};

template <typename T>
struct is_coro<Coro<T>> : std::true_type {};

template <typename T>
concept IsCoro = is_coro<T>::value;

enum class CoroPollState : uint8_t {
  kPending,
  kAborted,
  kReady,
};

template <typename T>
using CoroPoll = ::pw::containers::internal::Optional<T, CoroPollState::kReady>;

/// The `promise_type` of `Coro<T>`. This is an internal implementation detail,
/// and not part of the public `pw_async2` API.
///
/// To understand this type, reference C++20 coroutine API documentation.
class CoroPromiseBase {
 public:
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

  // Ignore exceptions in coroutines.
  //
  // Pigweed is not designed to be used with exceptions: `Result` or a
  // similar type should be used to propagate errors.
  void unhandled_exception() { PW_ASSERT(false); }

  // Allocate the space for both this `CoroPromise<T>` and the coroutine state.
  //
  // This override does not accept alignment.
  template <typename... Args>
  static void* operator new(std::size_t size,
                            CoroContext coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, alignof(std::max_align_t));
  }

  // Allocate the space for both this `CoroPromise<T>` and the coroutine state.
  //
  // This override accepts alignment.
  template <typename... Args>
  static void* operator new(std::size_t size,
                            std::align_val_t align,
                            CoroContext coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, static_cast<size_t>(align));
  }

  // Method-receiver form.
  //
  // This override does not accept alignment.
  template <typename MethodReceiver, typename... Args>
  static void* operator new(std::size_t size,
                            const MethodReceiver&,
                            CoroContext coro_cx,
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
                            CoroContext coro_cx,
                            const Args&...) noexcept {
    return SharedNew(coro_cx, size, static_cast<size_t>(align));
  }

  // Deallocate the space for both this `CoroPromise<T>` and the coroutine
  // state.
  //
  // In reality, we do nothing here!!!
  //
  // Coroutines do not support `destroying_delete`, so we can't access
  // `dealloc_` here, and therefore have no way to deallocate.
  // Instead, deallocation is handled by `OwningCoroutineHandle<T>::Release`.
  static void operator delete(void*) {}

  CoroPollState AdvanceAwaitable(Context& cx) {
    if (pending_awaitable_ == nullptr) {
      return CoroPollState::kReady;
    }
    return pending_awaitable_func_(pending_awaitable_, cx);
  }

  Deallocator& deallocator() const { return dealloc_; }

  template <typename AwaitableType>
  void SuspendAwaitable(AwaitableType& awaitable) {
    pending_awaitable_ = &awaitable;
    pending_awaitable_func_ = [](void* obj, Context& lambda_cx) {
      return static_cast<AwaitableType*>(obj)->Advance(lambda_cx);
    };
  }

 protected:
  CoroPromiseBase(CoroContext cx)
      : dealloc_(cx.allocator()), pending_awaitable_(nullptr) {}

 private:
  static void* SharedNew(CoroContext coro_cx,
                         std::size_t size,
                         std::size_t align) noexcept;

  Deallocator& dealloc_;

  // Attempt to complete the current awaitable value passed to `co_await`,
  // storing its return value inside the `Awaitable` object so that it can
  // be retrieved by the coroutine.
  //
  // Each `co_await` statement creates an `Awaitable` object whose `Pend`
  // method must be completed before the coroutine's `resume()` function can
  // be invoked.
  void* pending_awaitable_;
  CoroPollState (*pending_awaitable_func_)(void*, Context&);
};

template <typename T, typename Derived>
class TypedCoroPromise : public CoroPromiseBase {
 public:
  using value_type = T;

  // Get the `Coro<T>` after successfully allocating the coroutine space
  // and constructing `this`.
  Coro<T> get_return_object();

  // Create an invalid (nullptr) `Coro<T>` if `operator new` fails.
  static Coro<T> get_return_object_on_allocation_failure();

  // Indicate that allocation failed for a nested coroutine.
  void MarkNestedCoroutineAllocationFailure() {
    output_->reset(CoroPollState::kAborted);
  }

  // Returns a reference to the `Context` passed in.
  Context& cx() { return *context_; }

  // Sets the `Context` to use when advancing the awaitable and the pointer
  // where to store return values.
  void SetContextAndOutput(Context& cx, internal::CoroPoll<T>& output) {
    context_ = &cx;
    this->output_ = &output;
  }

  // Coroutine API functions

  // Handle a `co_await` call by accepting either a future or a coroutine and
  // returning an `Awaitable` wrapper that yields a `value_type` once complete.
  template <typename CoroOrFuture>
    requires(!std::is_reference_v<CoroOrFuture>)
  Awaitable<CoroOrFuture, Derived> await_transform(
      CoroOrFuture&& coro_or_future) {
    return std::forward<CoroOrFuture>(coro_or_future);
  }

  template <typename CoroOrFuture>
  Awaitable<CoroOrFuture*, Derived> await_transform(
      CoroOrFuture& coro_or_future) {
    return &coro_or_future;
  }

 protected:
  using CoroPromiseBase::CoroPromiseBase;

  internal::CoroPoll<T>& output() const { return *output_; }

 private:
  Context* context_;
  internal::CoroPoll<T>* output_;
};

template <typename T>
class CoroPromise final : public TypedCoroPromise<T, CoroPromise<T>> {
 public:
  // Construct the `CoroPromise` using the arguments passed to a function
  // returning `Coro<T>`.
  //
  // The first argument *must* be a `CoroContext`. The other arguments are
  // unused, but must be accepted in order for this to compile.
  template <typename... Args>
  CoroPromise(CoroContext cx, const Args&...)
      : TypedCoroPromise<T, CoroPromise>(cx) {}

  // Method-receiver version.
  template <typename MethodReceiver, typename... Args>
  CoroPromise(const MethodReceiver&, CoroContext cx, const Args&...)
      : TypedCoroPromise<T, CoroPromise>(cx) {}

  // Store the `co_return` arg in the memory provided by the `Pend` wrapper.
  template <std::convertible_to<T> From>
  void return_value(From&& value) {
    this->output() = std::forward<From>(value);
  }
};

// Handle void-returning coroutines.
//
// C++ does not allow a promise type to declare both return_value() and
// return_void(), so use a specialization.
template <>
class CoroPromise<void> final
    : public TypedCoroPromise<void, CoroPromise<void>> {
 public:
  template <typename... Args>
  CoroPromise(CoroContext cx, const Args&...)
      : TypedCoroPromise<void, CoroPromise>(cx) {}

  template <typename MethodReceiver, typename... Args>
  CoroPromise(const MethodReceiver&, CoroContext cx, const Args&...)
      : TypedCoroPromise<void, CoroPromise>(cx) {}

  // Mark the output as ready.
  void return_void() { this->output().emplace(); }
};

// The object created by invoking `co_await` in a `Coro<T>` function.
//
// This wraps a `Coro` or future and implements the awaitable interface expected
// by the standard coroutine API.
template <typename CoroOrFuture, typename PromiseType>
class Awaitable final {
 public:
  // The concrete type this Awaitable wraps.
  using await_type = std::remove_pointer_t<CoroOrFuture>;
  // The type produced by this awaitable.
  using value_type = typename await_type::value_type;

  Awaitable(CoroOrFuture&& coro_or_future)
      : state_(std::in_place_index<0>, std::move(coro_or_future)) {}

  // Confirms that `await_suspend` must be invoked.
  bool await_ready() { return false; }

  // Returns whether or not the current coroutine should be suspended.
  //
  // This is invoked once as part of every `co_await` call after
  // `await_ready` returns `false`.
  //
  // In the process, this method attempts to complete the inner `await_type`
  // before suspending this coroutine.
  bool await_suspend(const std::coroutine_handle<PromiseType>& promise)
    requires Future<await_type>
  {
    Context& cx = promise.promise().cx();
    if (Advance(cx) == CoroPollState::kPending) {
      promise.promise().SuspendAwaitable(*this);
      return true;
    }
    return false;
  }

  bool await_suspend(const std::coroutine_handle<PromiseType>& promise)
    requires IsCoro<await_type>
  {
    Context& cx = promise.promise().cx();
    CoroPollState state = Advance(cx);
    if (state == CoroPollState::kPending) {
      promise.promise().SuspendAwaitable(*this);
      return true;
    }
    if (state == CoroPollState::kAborted) {
      promise.promise().MarkNestedCoroutineAllocationFailure();
      return true;
    }
    return false;
  }

  // Returns `return_value`.
  //
  // This is automatically invoked by the language runtime when the promise's
  // `resume()` method is called.
  value_type&& await_resume()
    requires(!std::is_void_v<value_type>)
  {
    // await_resume() is never called after allocation failure because the
    // coroutine is destroyed instead of being resumed again.
    return std::move(std::get<1>(state_));
  }

  void await_resume()
    requires(std::is_void_v<value_type>)
  {}

  // Attempts to complete the `CoroOrFuture` value, storing its return value
  // upon completion.
  //
  // This method must return `kReady` before the coroutine can be safely
  // resumed, as otherwise the return value will not be available when
  // `await_resume` is called to produce the result of `co_await`.
  CoroPollState Advance(Context& cx)
    requires Future<await_type>
  {
    Poll<value_type> poll_res(get().Pend(cx));
    if (poll_res.IsPending()) {
      return CoroPollState::kPending;
    }
    if constexpr (std::is_void_v<value_type>) {
      state_.template emplace<1>();
    } else {
      state_.template emplace<1>(std::move(*poll_res));
    }
    return CoroPollState::kReady;
  }

  CoroPollState Advance(Context& cx)
    requires IsCoro<await_type>
  {
    if (!get().ok()) {
      return CoroPollState::kAborted;
    }
    auto result = get().Pend(cx);
    if (result.state() == CoroPollState::kReady) {
      if constexpr (std::is_void_v<value_type>) {
        state_.template emplace<1>();
      } else {
        state_.template emplace<1>(std::move(*result));
      }
    }
    return result.state();
  }

 private:
  await_type& get() {
    if constexpr (std::is_pointer_v<CoroOrFuture>) {
      return *std::get<0>(state_);
    } else {
      return std::get<0>(state_);
    }
  }

  struct Empty {};
  using VariantValueType =
      std::conditional_t<std::is_void_v<value_type>, Empty, value_type>;
  std::variant<CoroOrFuture, VariantValueType> state_;
};

}  // namespace internal

/// @submodule{pw_async2,coroutines}

/// An asynchronous coroutine which implements the C++20 coroutine API.
///
/// # Why coroutines?
/// Coroutines allow a series of asynchronous operations to be written as
/// straight line code. Rather than manually writing a state machine, users can
/// `co_await` any `pw_async2` @ref pw::async2::Future "future" or another
/// coroutine.
///
/// # Allocation
/// Pigweed's `Coro<T>` API supports checked, fallible allocations with
/// `pw::Allocator`. The first argument to any coroutine function must be a
/// `CoroContext&`. This allows the coroutine to allocate space for
/// asynchronously-held stack variables using the `CoroContext`'s allocator.
///
/// Failure to allocate coroutine "stack" space will result in the `Coro<T>`
/// returning `Status::Invalid()`.
///
/// # Creating a coroutine function
/// To create a coroutine, a function must:
/// - Have an annotated return type of `Coro<T>`, where `T` is the type that
///   will be returned from within the coroutine.
/// - Use `co_return <value>` rather than `return <value>` for any
///   `return` statements. For `pw::Status` or `pw::Result`, use `PW_CO_TRY` and
///   `PW_CO_TRY_ASSIGN` rather than `PW_TRY` and `PW_TRY_ASSIGN`.
/// - Accept a `CoroContext` as its first argument. The `CoroContext`'s
///   allocator is used to allocate storage for coroutine stack variables held
///   across a `co_await` point.
///
/// # Using co_await
/// Inside a coroutine function, `co_await <expr>` can be used for pw_async2
/// futures or other coroutines. The result will be a value of type `T`.
template <typename T>
class Coro final {
 public:
  /// Used by the compiler to create a `Coro<T>` from a coroutine function.
  using promise_type = ::pw::async2::internal::CoroPromise<T>;

  /// The type this coroutine returns from a `co_return` expression.
  using value_type = T;

  /// Creates an empty, invalid coroutine object.
  static Coro Empty() {
    return Coro(internal::OwningCoroutineHandle<promise_type>(nullptr));
  }

  /// Whether or not this `Coro<T>` is a valid coroutine.
  ///
  /// This will return `false` if coroutine state allocation failed or if
  /// this `Coro<T>::Pend` method previously returned a `Ready` value.
  [[nodiscard]] bool ok() const { return promise_handle_.IsValid(); }

 private:
  // Allow get_return_object() and get_return_object_on_allocation_failure() to
  // use the private constructor below.
  friend internal::TypedCoroPromise<T, promise_type>;

  // Only allow Awaitable and Coro task wrappers to call Pend.
  template <typename, typename>
  friend class internal::Awaitable;
  template <typename, ReturnValuePolicy>
  friend class CoroTask;
  template <typename, typename E, ReturnValuePolicy>
    requires std::invocable<E>
  friend class FallibleCoroTask;

  /// Attempt to complete this coroutine, returning the result if complete.
  ///
  /// Crashes if `ok()` is false, which occurs when coroutine state allocation
  /// fails.
  internal::CoroPoll<T> Pend(Context& cx) {
    using enum internal::CoroPollState;

    if (!ok()) {
      internal::CrashDueToCoroutineAllocationFailure();
    }

    // DOCSTAG: [pw_async2-coro-resume]
    internal::CoroPoll<T> return_value(kPending);

    // Stash the Context& argument for the coroutine.
    // Reserve space for the return value and point the promise to it.
    promise_handle_.promise().SetContextAndOutput(cx, return_value);

    // If an `Awaitable` value is currently being processed, it must be
    // allowed to complete and store its return value before we can resume
    // the coroutine.
    switch (promise_handle_.promise().AdvanceAwaitable(cx)) {
      case kPending:
        break;
      case kAborted:
        return_value.reset(kAborted);
        promise_handle_.Release();
        break;
      case kReady:
        // Resume the coroutine, triggering `Awaitable::await_resume()` and the
        // returning of the resulting value from `co_await`. The promise's
        // `return_value()` function stores the result in the `return_value`
        // variable at this point.
        promise_handle_.resume();

        // `return_value` now reflects the results of the operation. Unless it's
        // still pending, free the coroutine's memory.
        if (return_value.state() != kPending) {
          // Destroy the coroutine state: it has completed or aborted, and
          // further calls to `resume` would result in undefined behavior.
          promise_handle_.Release();
        }
        break;
    }

    return return_value;
    // DOCSTAG: [pw_async2-coro-resume]
  }

  /// Create a new `Coro<T>` using a (possibly null) handle.
  explicit Coro(internal::OwningCoroutineHandle<promise_type>&& promise_handle)
      : promise_handle_(std::move(promise_handle)) {}

  internal::OwningCoroutineHandle<promise_type> promise_handle_;
};

template <typename Promise>
Coro(internal::OwningCoroutineHandle<Promise>&&)
    -> Coro<typename Promise::value_type>;

/// @endsubmodule

// Implement the remaining internal pieces that require a definition of
// `Coro<T>`.
namespace internal {

template <typename T, typename Derived>
Coro<T> TypedCoroPromise<T, Derived>::get_return_object() {
  return Coro<T>(internal::OwningCoroutineHandle<Derived>(
      std::coroutine_handle<CoroPromise<T>>::from_promise(
          static_cast<Derived&>(*this))));
}

template <typename T, typename Derived>
Coro<T>
TypedCoroPromise<T, Derived>::get_return_object_on_allocation_failure() {
  return Coro<T>(internal::OwningCoroutineHandle<Derived>(nullptr));
}

// Checks that the first argument is a by-value CoroContext.
template <typename... Args>
struct CoroContextIsPassedByValue : std::false_type {};

// If there is only a single argument, it must be a CoroContext value.
template <typename First>
struct CoroContextIsPassedByValue<First> : std::is_same<First, CoroContext> {};

// If there are multiple arguments, the first argument to a free function must
// be CoroContext. For member functions, the first argument is a reference to
// the object and the second must be CoroContext. Note that this trait cannot
// distinguish between a member function and a free function with a reference to
// an object as its first argument.
template <typename First, typename Second, typename... Others>
struct CoroContextIsPassedByValue<First, Second, Others...>
    : std::disjunction<
          std::is_same<First, CoroContext>,
          std::conjunction<std::is_same<Second, CoroContext>,
                           std::is_reference<First>,
                           std::is_class<std::remove_reference_t<First>>>> {};

}  // namespace internal
}  // namespace pw::async2

// Specialize `std::coroutine_traits` to enforce `CoroContext` semantics.
namespace std {

template <typename T, typename... Args>
struct coroutine_traits<pw::async2::Coro<T>, Args...> {
  using promise_type = typename pw::async2::Coro<T>::promise_type;

  static_assert(
      pw::async2::internal::CoroContextIsPassedByValue<Args...>::value,
      "CoroContext must be passed by value as the first argument to a "
      "pw_async2 coroutine");

  static_assert(
      (static_cast<int>(
           std::is_same_v<std::remove_cvref_t<Args>, pw::async2::CoroContext>) +
       ...) == 1,
      "pw_async2 coroutines must have exactly one CoroContext argument");
};

}  // namespace std
