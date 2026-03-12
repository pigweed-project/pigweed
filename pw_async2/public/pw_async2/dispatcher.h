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

#include <atomic>
#include <mutex>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/future_task.h"
#include "pw_async2/internal/lock.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_sync/lock_annotations.h"

// Coroutines are supported if the build target depends on //pw_async2:coro.
#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
#include <functional>  // std::invoke

#include "pw_async2/coro.h"
#include "pw_async2/coro_task.h"
#include "pw_async2/fallible_coro_task.h"
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

namespace pw::async2 {

/// @submodule{pw_async2,dispatchers}

/// A single-threaded cooperatively scheduled runtime for async tasks.
///
/// Dispatcher implementations must pop and run tasks with one of the following:
///
/// - `PopAndRunAllReadyTasks()` – Runs tasks until no progress can be made.
///   The dispatcher will be woken when a task is ready to run.
/// - `PopTaskToRun()` and `RunTask()` – Run tasks individually. Dispatcher
///   implementations MUST pop and run tasks until `PopTaskToRun()` returns
///   `nullptr`. The dispatcher will not be woken when a task becomes ready
///   unless `PopTaskToRun()` has returned `nullptr`.
/// - `PopSingleTaskForThisWake()` and `RunTask()` – Run tasks individually. It
///   `PopSingleTaskForThisWake` is intended for use then only a single task (or
///   one final task) should be executed. Is not necessary to call
///   `PopSingleTaskForThisWake()` until it returns `nullptr`. Each call can
///   result in one potentially redundant `DoWake()` call, so `PopTaskToRun`
///   should be used one multiple tasks are executed.
///
/// The base `Dispatcher` performs no allocations internally. `Dispatcher`
/// offers `Post` overloads that allocate a task with the provided allocator,
/// but their use is optional. C++20 coroutines can be posted to a `Dispatcher`
/// with `Post` if C++20 is supported and the build target depends on
/// `//pw_async2:coro`.
class Dispatcher {
 public:
  Dispatcher(Dispatcher&) = delete;
  Dispatcher& operator=(Dispatcher&) = delete;

  Dispatcher(Dispatcher&&) = delete;
  Dispatcher& operator=(Dispatcher&&) = delete;

  /// Removes references to this `Dispatcher` from all linked `Task`s and
  /// `Waker`s.
  virtual ~Dispatcher() PW_LOCKS_EXCLUDED(internal::lock()) { Destroy(); }

  /// Tells the `Dispatcher` to run `Task` to completion. This method does not
  /// block.
  ///
  /// After `Post` is called, `Task::Pend` will be invoked once. If `Task::Pend`
  /// does not complete, the `Dispatcher` will wait until the `Task` is
  /// "awoken", at which point it will call `Pend` again until the `Task`
  /// completes.
  ///
  /// This method is thread-safe and interrupt-safe.
  void Post(Task& task) PW_LOCKS_EXCLUDED(internal::lock()) {
    {
      std::lock_guard lock(internal::lock());
      PostLocked(task);
    }
    Wake();
  }

  /// Posts a dynamically allocated task to the dispatcher. Functions the same
  /// as `Post`, except the dispatcher takes shared reference to the task, which
  /// it frees when the task completes.
  ///
  /// @pre `task` must NOT be `nullptr`.
  template <typename T>
  void PostShared(const SharedPtr<T>& task)
      PW_LOCKS_EXCLUDED(internal::lock()) {
    PW_ASSERT(PostAllocatedTask(task));
  }

  /// Allocates a `TaskType` using `allocator` and posts it to this execution
  /// context as with `PostShared`.
  ///
  /// @returns A `SharedPtr` to the posted task if allocation succeeded, or
  ///     a null `SharedPtr` if allocation failed.
  template <typename TaskType,
            typename... Args,
            typename = std::enable_if_t<std::is_base_of_v<Task, TaskType>>>
  [[nodiscard]] SharedPtr<TaskType> Post(Allocator& allocator, Args&&... args) {
    auto task = allocator.MakeShared<TaskType>(std::forward<Args>(args)...);
    if (!PostAllocatedTask(task)) {
      return nullptr;
    }
    return task;
  }

  /// Allocates a `Task` defined by the provided function and posts it as with
  /// `PostShared`. The function must take a `pw::async2::Context` as its only
  /// argument and return a `Poll<T>`.
  ///
  /// This function supports both explicitly and implicitly specified function
  /// types. If `Func` is explicitly specified, it is used as-is. If `Func` is
  /// not specified, it is deduced as `std::decay_t<Func>`.
  ///
  /// @returns A `SharedPtr` to the posted task if allocation succeeded, or a
  ///     null `SharedPtr` if allocation failed.
  template <
      typename Func = void,
      int&... kExplicitGuard,
      typename Arg,
      typename ActualFunc =
          std::conditional_t<std::is_void_v<Func>, std::decay_t<Arg>, Func>,
      typename = std::enable_if_t<!std::is_base_of_v<Task, ActualFunc>>>
  [[nodiscard]] SharedPtr<FuncTask<ActualFunc>> Post(Allocator& allocator,
                                                     Arg&& func) {
    return Post<FuncTask<ActualFunc>>(allocator, std::forward<Arg>(func));
  }

  /// Runs a function object once on the `Dispatcher`. Allocates the wrapper
  /// task with `allocator`. The `Dispatcher` frees it when it completes.
  ///
  /// Use `RunOnceTask` to declare a task that runs a function once without
  /// dynamic allocation.
  ///
  /// @warning `RunOnce` should be used rarely, such as in tests, truly one-off
  /// cases, or as a last resort for sync-async interop. `pw_async2` should not
  /// be used as a work queue. Overuse of `RunOnceTask` forfeits the benefits of
  /// `pw_async2`, scattering logic across a mess of callbacks instead of
  /// organizing it linearly in a task.
  template <typename Func>
  [[nodiscard]] SharedPtr<RunOnceTask<Func>> RunOnce(Allocator& allocator,
                                                     Func&& func) {
    return Post<RunOnceTask<Func>>(allocator, std::forward<Func>(func));
  }

  /// Allocates and posts a `FutureTask` that runs a future to completion.
  ///
  /// @warning `PostFuture` is intended for test and occasional production use.
  /// A `FutureTask` does not contain logic, and relying too much on
  /// `FutureTasks` could push logic out of async tasks, which nullifies the
  /// benefits of `pw_async2`. Creating a task for each future is also less
  /// efficient than having one task work with multiple futures.
  ///
  /// @returns A `SharedPtr` to the posted task if allocation succeeded, or a
  ///     null `SharedPtr` if allocation failed.
  template <typename Fut>
  [[nodiscard]] SharedPtr<FutureTask<Fut>> PostFuture(Allocator& allocator,
                                                      Fut&& future) {
    return Post<FutureTask<Fut>>(allocator, std::forward<Fut>(future));
  }

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
  /// Allocates and posts a `CoroTask` that runs the provided coroutine to
  /// completion.
  ///
  /// Returns `nullptr` if the coroutine or `CoroTask` failed to allocate.
  /// Crashes if subsequent coroutine allocations fail.
  ///
  /// @returns A `SharedPtr` to the posted task if allocation succeeded, or a
  ///     null `SharedPtr` if allocation failed.
  template <typename T>
  [[nodiscard]] SharedPtr<CoroTask<T>> Post(Allocator& allocator,
                                            Coro<T>&& coro) {
    if (!coro.ok()) {
      return nullptr;
    }
    return Post<CoroTask<T>>(allocator, std::move(coro));
  }

  /// Allocates and posts a `FallibleCoroTask` that runs the provided coroutine
  /// to completion.
  ///
  /// Returns `nullptr` if the coroutine or `FallibleCoroTask` failed to
  /// allocate. Invokes `error_handler` if subsequent coroutine allocations
  /// fail.
  ///
  /// @returns A `SharedPtr` to the posted task if allocation succeeded, or a
  ///     null `SharedPtr` if allocation failed.
  template <typename T,
            typename E = void,
            int&... kExplicitGuard,
            typename Arg,
            typename ErrorHandler =
                std::conditional_t<std::is_void_v<E>, std::decay_t<Arg>, E>>
  [[nodiscard]] SharedPtr<FallibleCoroTask<T, ErrorHandler>> Post(
      Allocator& allocator, Coro<T>&& coro, Arg&& error_handler) {
    if (!coro.ok()) {
      return nullptr;
    }
    return Post<FallibleCoroTask<T, ErrorHandler>>(
        allocator, std::move(coro), std::forward<Arg>(error_handler));
  }

  /// Allocates and posts a `CoroTask` for the provided coroutine function.
  ///
  /// The coroutine function is invoked with the provided arguments. The
  /// allocator from the `CoroContext` (the first argument) is used to allocate
  /// the task.
  ///
  /// Returns `nullptr` if the coroutine or `CoroTask` failed to allocate.
  /// Crashes if subsequent coroutine allocations fail.
  ///
  /// @returns A `SharedPtr` to the posted task if allocation succeeded, or a
  ///     null `SharedPtr` if allocation failed.
  template <auto kCoroFunc, typename... Args>
  [[nodiscard]] auto Post(CoroContext coro_cx, Args&&... args) {
    if constexpr (std::is_member_function_pointer_v<decltype(kCoroFunc)>) {
      return PostSharedMemberCoro<kCoroFunc>(coro_cx,
                                             std::forward<Args>(args)...);
    } else {
      return Post(coro_cx.allocator(),
                  std::invoke(kCoroFunc, coro_cx, std::forward<Args>(args)...));
    }
  }
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

  /// Outputs log statements about the tasks currently registered with this
  /// dispatcher.
  void LogRegisteredTasks() PW_LOCKS_EXCLUDED(internal::lock());

 protected:
  constexpr Dispatcher() = default;

  /// Pops and runs tasks until there are no tasks ready to run.
  ///
  /// This function may be called by dispatcher implementations to run tasks.
  /// This is a high-level function that runs all ready tasks without logging or
  /// metrics. For more control, use `PopTaskToRun` and `RunTask`.
  ///
  /// @retval true The dispatcher has posted tasks, but they are sleeping.
  /// @retval false The dispatcher has no posted tasks.
  bool PopAndRunAllReadyTasks() PW_LOCKS_EXCLUDED(internal::lock());

  /// Pops a task and marks it as running. The task must be passed to `RunTask`.
  ///
  /// `PopTaskToRun` MUST be called repeatedly until it returns `nullptr`, at
  /// which point the dispatcher will request a wake.
  Task* PopTaskToRun() PW_LOCKS_EXCLUDED(internal::lock()) {
    std::lock_guard lock(internal::lock());
    return PopTaskToRunLocked();
  }

  /// `PopTaskToRun` overload that optionally reports the whether the
  /// `Dispatcher` has registered tasks. This allows callers to distinguish
  /// between there being no woken tasks and no posted tasks at all.
  ///
  /// Like the no-argument overload, `PopTaskToRun` MUST be called repeatedly
  /// until it returns `nullptr`.
  ///
  /// @param[out] has_posted_tasks Set to `true` if the dispatcher has at least
  ///     one task posted, potentially including the task that was popped. Set
  ///     to `false` if the dispatcher has no posted tasks.
  /// @returns A pointer to a task that is ready to run, or `nullptr` if there
  ///     are no ready tasks.
  Task* PopTaskToRun(bool& has_posted_tasks)
      PW_LOCKS_EXCLUDED(internal::lock()) {
    std::lock_guard lock(internal::lock());
    Task* task = PopTaskToRunLocked();
    has_posted_tasks = task != nullptr || !sleeping_.empty();
    return task;
  }

  /// Pop a single task to run. Each call to `PopSingleTaskForThisWake` can
  /// result in up to one `DoWake()` call, so use `PopTaskToRun` or
  /// `PopAndRunAllReadyTasks` to run multiple tasks.
  Task* PopSingleTaskForThisWake() PW_LOCKS_EXCLUDED(internal::lock()) {
    std::lock_guard lock(internal::lock());
    SetWantsWake();
    return PopTaskToRunLocked();
  }

  /// Runs the task that was returned from `PopTaskToRun`.
  ///
  /// @warning Do NOT access the `Task` object after `RunTask` returns! The task
  /// could have destroyed, either by the dispatcher or another thread, even if
  /// `RunTask` returns `kActive`. It is only safe to access a popped task
  /// before calling `RunTask`, since it is marked as running and will not be
  /// destroyed until after it runs.
  RunTaskResult RunTask(Task& task) PW_LOCKS_EXCLUDED(internal::lock()) {
    return task.RunInDispatcher();
  }

 private:
  friend class Task;
  friend class Waker;

  // Allow DispatcherForTestFacade to wrap another dispatcher (call Do*).
  template <typename>
  friend class DispatcherForTestFacade;

  // Posts a task, but does not wake the dispatcher, which is done after the
  // lock is released.
  void PostLocked(Task& task) PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  /// Sends a wakeup signal to this `Dispatcher`.
  ///
  /// This method's implementation must ensure that the `Dispatcher` runs at
  /// some point in the future.
  ///
  /// `DoWake()` will only be called once until one of the following occurs:
  ///
  /// - `PopAndRunAllReadyTasks()` is called,
  /// - `PopTaskToRun()` returns `nullptr`, or
  /// - `PopSingleTaskForThisWake()` is called.
  ///
  /// @note The `internal::lock()` may or may not be held here, so it
  /// must not be acquired by `DoWake`, nor may `DoWake` assume that it has been
  /// acquired.
  virtual void DoWake() PW_LOCKS_EXCLUDED(internal::lock()) = 0;

  // Prefer to call Wake() without the lock held, but it can be called with it
  // when necessary (see WakeTask()).
  void Wake() {
    if (wants_wake_.exchange(false, std::memory_order_relaxed)) {
      DoWake();
    }
  }

  Task* PopTaskToRunLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Removes references to this `Dispatcher` from all linked `Task`s and
  // `Waker`s. Use a separate function so thread safety analysis applies.
  void Destroy() PW_LOCKS_EXCLUDED(internal::lock());

  static void UnpostTaskList(IntrusiveForwardList<Task>& list)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  void RemoveWokenTaskLocked(Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    woken_.remove(task);
  }
  void RemoveSleepingTaskLocked(Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    sleeping_.remove(task);
  }
  void AddSleepingTaskLocked(Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    sleeping_.push_front(task);
  }

  // For use by `Waker`.
  void WakeTask(Task& task) PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  void LogTaskWakers(const Task& task)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock());

  // Indicates that this Dispatcher should be woken when Wake() is called. This
  // prevents unnecessary wakes when, for example, multiple wakers wake the same
  // task or multiple tasks are posted before the dipsatcher runs.
  //
  // Must be called while the lock is held to prevent missed wakes.
  void SetWantsWake() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::lock()) {
    wants_wake_.store(true, std::memory_order_relaxed);
  }

  // Checks if `task` contains a value and posts it to the dispatcher as an
  // allocated task. Returns true if the task posted successfully.
  template <typename T>
  bool PostAllocatedTask(const SharedPtr<T>& task)
      PW_LOCKS_EXCLUDED(internal::lock()) {
    return PostAllocatedTask(
        task.get(),
        task.GetControlBlock(
            allocator::internal::ControlBlockHandle::GetInstance_DO_NOT_USE()));
  }

  bool PostAllocatedTask(Task* task,
                         allocator::internal::ControlBlock* control_block)
      PW_LOCKS_EXCLUDED(internal::lock());

#if defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")
  template <auto kCoroMemberFunc, typename Receiver, typename... Args>
  [[nodiscard]] auto PostSharedMemberCoro(CoroContext coro_cx,
                                          Receiver&& receiver,
                                          Args&&... args) {
    return Post(coro_cx.allocator(),
                std::invoke(kCoroMemberFunc,
                            std::forward<Receiver>(receiver),
                            coro_cx,
                            std::forward<Args>(args)...));
  }
#endif  // defined(__cpp_impl_coroutine) && __has_include("pw_async2/coro.h")

  // TODO: b/491844340 - Evaluate IntrusiveForwardList performance and consider
  //     alternatives.
  IntrusiveForwardList<Task> woken_ PW_GUARDED_BY(internal::lock());
  IntrusiveForwardList<Task> sleeping_ PW_GUARDED_BY(internal::lock());

  // Latches wake requests to avoid duplicate DoWake calls.
  std::atomic<bool> wants_wake_ = false;
};

/// @endsubmodule

}  // namespace pw::async2
