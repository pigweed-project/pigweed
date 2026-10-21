// Copyright 2021 The Pigweed Authors
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

#include "pw_function/function.h"
#include "pw_thread/attrs.h"
#include "pw_thread/context.h"
#include "pw_thread/id.h"
#include "pw_thread/native_options.h"
#include "pw_thread/options.h"

// clang-format off
// The backend's thread_native header must provide PW_THREAD_JOINING_ENABLED.
#include "pw_thread_backend/thread_native.h"  // IWYU pragma: export
// clang-format on

// Detect if the default "null" thread creation backend is seletecd.
#ifdef _PW_THREAD_GENERIC_CREATION_UNSUPPORTED

#define PW_THREAD_GENERIC_CREATION_IS_SUPPORTED 0
#undef _PW_THREAD_GENERIC_CREATION_UNSUPPORTED

#else

// Indicates whether generic thread creation is implemented.
// TODO: b/385440717 - Remove this flag when generic thread creation is
// implemented for all pw_thread backends.
#define PW_THREAD_GENERIC_CREATION_IS_SUPPORTED 1

#endif  // _PW_THREAD_GENERIC_CREATION_UNSUPPORTED

namespace pw {

/// @module{pw_thread}

/// Thread creation and execution library
namespace thread {

/// The class `Thread` can represent a single thread of execution. Threads allow
/// multiple functions to execute concurrently.
///
/// Threads may begin execution immediately upon construction of the associated
/// thread object (pending any OS scheduling delays), starting at the top-level
/// function provided as a constructor argument. The return value of the
/// top-level function is ignored. The top-level function may communicate its
/// return value by modifying shared variables (which may require
/// synchronization, see `pw_sync` and `std::atomic`)
///
/// `Thread` objects may also be in the state that does not represent any thread
/// (after default construction, move from, detach, or join), and a thread of
/// execution may be not associated with any thread objects (after detach).
///
/// No two `Thread` objects may represent the same thread of execution; `Thread`
/// is not CopyConstructible or CopyAssignable, although it is MoveConstructible
/// and MoveAssignable.
class Thread {
 public:
  /// The type of the native handle for the thread. As with `std::thread`, use
  /// is inherently non-portable.
  using native_handle_type = backend::NativeThreadHandle;

  /// The class id is a lightweight, trivially copyable class that serves as a
  /// unique identifier of Thread objects.
  ///
  /// Instances of this class may also hold the special distinct value that does
  /// not represent any thread. Once a thread has finished, the value of its
  /// `Thread::id` may be reused by another thread.
  ///
  /// This class is designed for use as key in associative containers, both
  /// ordered and unordered.
  ///
  /// The backend must ensure that:
  ///
  /// 1. There is a default construct which does not represent a thread.
  /// 2. Compare operators (`==`, `!=`, `<`, `<=`, `>`, `>=`) are provided to
  ///    compare and sort IDs.
  using id = ::pw::thread::backend::NativeId;

  /// Creates a new thread object which does not represent a thread of execution
  /// yet.
  Thread();

  /// Creates a thread from a void-returning function or lambda.
  ///
  /// This function accepts any callable (including lambdas) which returns
  /// ``void``. When using a lambda, the captures must not exceed the inline
  /// size of ``pw::Function`` (usually a single pointer) unless dynamic
  /// allocation is enabled.
  ///
  /// To invoke a member method of a class a static lambda closure can be used
  /// to ensure the dispatching closure is not destructed before the thread is
  /// done executing. For example:
  ///
  /// @code{.cpp}
  /// class Foo {
  ///  public:
  ///   void DoBar() {}
  /// };
  /// Foo foo;
  ///
  /// // Now use the lambda closure as the thread entry, passing the foo's
  /// // this as the argument.
  /// Thread thread(options, [&foo]() { foo.DoBar(); });
  /// thread.detach();
  /// @endcode
  ///
  /// @post The thread get EITHER detached or joined.
  Thread(const Options& options, Function<void()>&& entry);

  // Creates a thread with a ThreadContext associated with a ThreadAttrs.
  template <const ThreadAttrs& kAttributes>
  Thread(ThreadContextFor<kAttributes>& context, Function<void()>&& entry)
      : Thread(GetThreadOptions(context), std::move(entry)) {}

  // Creates a thread from context and attributes. Performs a runtime check
  // that the ThreadContext's stack is large enough, which can be avoided by
  // using one of the other constructors.
  template <size_t kStackSizeHintBytes>
  Thread(ThreadContext<kStackSizeHintBytes>& context,
         const ThreadAttrs& attributes,
         Function<void()>&& entry)
      : Thread(GetThreadOptions(context, attributes), std::move(entry)) {}

  // Creates a thread with the provided context and attributes. The
  // attributes must have a ThreadStack set.
  Thread(ThreadContext<>& context,
         const ThreadAttrs& attributes,
         Function<void()>&& entry)
      : Thread(GetThreadOptions(context, attributes), std::move(entry)) {}

  /// @post The other thread no longer represents a thread of execution.
  Thread& operator=(Thread&& other);

  /// @pre The thread must have been EITHER detached or joined.
  ~Thread();

  Thread(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(const Thread&) = delete;

  /// Returns a value of `Thread::id` identifying the thread associated with
  /// `*this`. If there is no thread associated, default constructed
  /// `Thread::id` is returned.
  id get_id() const;

  /// Checks if the `Thread` object identifies an active thread of execution
  /// which has not yet been detached. Specifically, returns true if `get_id()
  /// != pw::Thread::id()` and `detach()` has NOT been invoked. So a default
  /// constructed thread is not joinable and neither is one which was detached.
  ///
  /// A thread that has not started or has finished executing code which was
  /// never detached, but has not yet been joined is still considered an active
  /// thread of execution and is therefore joinable.
  bool joinable() const { return get_id() != id(); }

#if PW_THREAD_JOINING_ENABLED
  /// Blocks the current thread until the thread identified by `*this` finishes
  /// its execution.
  ///
  /// The completion of the thread identified by *this synchronizes with the
  /// corresponding successful return from join().
  ///
  /// No synchronization is performed on `*this` itself. Concurrently calling
  /// `join()` on the same thread object from multiple threads constitutes a
  /// data race that results in undefined behavior.
  ///
  /// @pre The thread must have been NEITHER detached nor joined.
  ///
  /// @post After calling detach `*this` no longer owns any thread.
  void join();
#else
  template <typename..., bool kJoiningEnabled = false>
  void join() {
    static_assert(kJoiningEnabled,
                  "The selected pw_thread_THREAD backend does not have join() "
                  "enabled (AKA PW_THREAD_JOINING_ENABLED = 1)");
  }
#endif  // PW_THREAD_JOINING_ENABLED

  /// Separates the thread of execution from the thread object, allowing
  /// execution to continue independently. Any allocated resources will be freed
  /// once the thread exits.
  ///
  /// @pre The thread must have been NEITHER detached nor joined.
  ///
  /// @post After calling detach *this no longer owns any thread.
  void detach();

  /// Exchanges the underlying handles of two thread objects.
  void swap(Thread& other);

  /// Returns the native handle for the thread. As with `std::thread`, use is
  /// inherently non-portable.
  native_handle_type native_handle();

 private:
  // Note that just like std::thread, this is effectively just a pointer or
  // reference to the native thread -- this does not contain any memory needed
  // for the thread to execute.
  //
  // This may contain more than the native thread handle to enable functionality
  // which is not always available such as joining, which may require a
  // reference to a binary semaphore, or passing arguments to the thread's
  // function.
  backend::NativeThread native_type_;
};

}  // namespace thread

/// `pw::thread::Thread` will be renamed to `pw::Thread`. New code should refer
/// to `pw::Thread`.
using Thread = ::pw::thread::Thread;  // Must use `=` for Doxygen to find this.

}  // namespace pw

#include "pw_thread_backend/thread_inline.h"  // IWYU pragma: export
