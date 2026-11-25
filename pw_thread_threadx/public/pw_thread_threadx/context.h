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

#include <cstdint>
#include <cstring>

#include "pw_function/function.h"
#include "pw_span/span.h"
#include "pw_string/util.h"
#include "pw_thread_threadx/config.h"
#include "tx_api.h"
#include "tx_thread.h"

namespace pw::thread {

class Thread;  // Forward declare Thread which depends on Context.

}  // namespace pw::thread

namespace pw::thread::threadx {

class Options;

/// @module{pw_thread_threadx}

/// Static thread context allocation including the TCB, an event group for
/// joining if enabled, and an external statically allocated stack.
///
/// Example usage:
///
/// @code{.cpp}
///   std::array<ULONG, kFooStackSizeWords> example_thread_stack;
///   pw::thread::threadx::Context example_thread_context(example_thread_stack);
///   void StartExampleThread() {
///     pw::thread::DetachedThread(
///         pw::thread::threadx::Options()
///             .set_name("example_thread")
///             .set_priority(kFooPriority)
///             .set_context(example_thread_context),
///         example_thread_function);
///   }
/// @endcode
class Context {
 public:
  /// Constructs a thread context using an externally allocated stack.
  ///
  /// @pre `stack_span.size() >= config::kMinimumStackSizeWords`, and the
  /// backing storage must outlive this `Context` and any thread using it.
  ///
  /// @param[in] stack_span Pre-allocated stack storage in `ULONG` words.
  explicit Context(span<ULONG> stack_span) : tcb_{}, stack_span_(stack_span) {}
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  /// Returns the underlying ThreadX `TX_THREAD` thread control block.
  ///
  /// Intended for unit test and `pw::Thread` use only.
  ///
  /// @returns A reference to the underlying `TX_THREAD` TCB.
  TX_THREAD& tcb() { return tcb_; }

 private:
  friend Thread;
  void CreateThread(const threadx::Options& options,
                    Function<void()>&& thread_fn);

  span<ULONG> stack() { return stack_span_; }

  bool in_use() const { return in_use_; }
  void set_in_use(bool in_use = true) { in_use_ = in_use; }

  const char* name() const { return name_.data(); }
  void set_name(const char* name) { string::Copy(name, name_); }

  void set_thread_routine(Function<void()>&& rvalue) {
    fn_ = std::move(rvalue);
  }

  bool detached() const { return detached_; }
  void set_detached(bool value = true) { detached_ = value; }

  bool thread_done() const { return thread_done_; }
  void set_thread_done(bool value = true) { thread_done_ = value; }

#if PW_THREAD_JOINING_ENABLED
  TX_EVENT_FLAGS_GROUP& join_event_group() { return event_group_; }
#endif  // PW_THREAD_JOINING_ENABLED

  static void ThreadEntryPoint(ULONG void_context_ptr);
  static void DeleteThread(Context& context);

  TX_THREAD tcb_;
  span<ULONG> stack_span_;

  Function<void()> fn_;
#if PW_THREAD_JOINING_ENABLED
  // Note that the ThreadX life cycle of this event group is managed together
  // with the thread life cycle, not this object's life cycle.
  TX_EVENT_FLAGS_GROUP event_group_;
#endif  // PW_THREAD_JOINING_ENABLED
  bool in_use_ = false;
  bool detached_ = false;
  bool thread_done_ = false;

  // The TCB does not have storage for the name, ergo we provide storage for
  // the thread's name which can be truncated down to just a null delimiter.
  std::array<char, config::kMaximumNameLength + 1> name_;
};

/// Static thread context allocation including the stack along with the
/// `Context`.
///
/// Example usage:
///
/// @code{.cpp}
///   pw::thread::threadx::ContextWithStack<kFooStackSizeWords>
///       example_thread_context;
///   void StartExampleThread() {
///     pw::Thread(
///         pw::thread::threadx::Options()
///             .set_name("static_example_thread")
///             .set_priority(kFooPriority)
///             .set_context(example_thread_context),
///         example_thread_function)
///         .detach();
///   }
/// @endcode
///
/// @tparam kStackSizeWords Stack size in `ULONG` words. Must be at least
/// `config::kMinimumStackSizeWords`.
template <size_t kStackSizeWords = config::kDefaultStackSizeWords>
class ContextWithStack final : public Context {
 public:
  /// Constructs a thread context with an internally allocated stack.
  constexpr ContextWithStack() : Context(stack_storage_) {
    static_assert(kStackSizeWords >= config::kMinimumStackSizeWords);
  }

 private:
  std::array<ULONG, kStackSizeWords> stack_storage_;
};

/// @endmodule

}  // namespace pw::thread::threadx
