// Copyright 2023 The Pigweed Authors
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

#include <zephyr/kernel.h>
#include <zephyr/kernel/thread_stack.h>

#include <cstdint>
#include <cstring>

#include "pw_assert/check.h"
#include "pw_function/function.h"
#include "pw_span/span.h"
#include "pw_string/util.h"
#include "pw_thread_zephyr/config.h"
#include "pw_thread_zephyr/stack.h"

namespace pw::thread {

// Forward declare Thread which depends on Context.
class Thread;

namespace zephyr {

// The maximum length of a thread's name, not including null termination. This
// results in an array of characters which is this length + 1 bytes in every
// pw::Thread's context.
inline constexpr size_t kMaximumNameLength =
    ::pw::thread::zephyr::config::kMaximumNameLength;

// Forward declare Options since we'll need a reference to them.
class Options;

// At the moment, Zephyr RTOS doesn't support dynamic thread stack allocation
// (due to various alignment and size requirements on different architectures).
// Still, we separate the context in two parts:
//
//   1) Context which just contains the Thread Control Block (k_thread) and
//      additional context pw::Thread requires.
//
//   2) ContextWithStack which contains the stack.
class Context {
 public:
  /// Create a default native context
  ///
  /// This context will have no name or stack associated with it.
  constexpr Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  ~Context() = default;

  /// Get the stack associated with the context.
  ///
  /// @return span for the stack that was allocated with this context
  span<z_thread_stack_element> stack() { return stack_; }

  /// Create a thread
  ///
  /// Can be called only once!
  ///
  /// @arg thread_fn The entry point of the thread
  /// @arg options The options to configure the thread
  void CreateThread(Function<void()>&& thread_fn, const Options& options);

 protected:
  constexpr void set_stack(span<z_thread_stack_element> stack) {
    stack_ = stack;
  }

 private:
  friend Thread;

  bool detached() const { return detached_; }
  void set_detached() { detached_ = true; }

  bool thread_done() const { return thread_done_; }
  void set_thread_done() { thread_done_ = true; }

  const char* name() const { return name_.data(); }

  static void ThreadEntryPoint(void* void_context_ptr, void*, void*);

  k_tid_t task_handle_ = nullptr;
  k_thread thread_info_ = {};
  Function<void()> fn_;
  bool detached_ = false;
  bool thread_done_ = false;

  // The TCB may have storage for the name, depending on the setting of
  // CONFIG_THREAD_NAME, and if storage is present, the reserved
  // space will depend on CONFIG_THREAD_MAX_NAME_LEN. In order to provide
  // a consistent interface, we always store the string here, and use
  // k_thread_name_set to set the name for the thread, if it is available.
  // We will defer to our storage when queried for the name, but by setting
  // the name with the RTOS call, raw RTOS access to the thread's name should
  // work properly, though possibly with a truncated name.
  pw::InlineString<kMaximumNameLength> name_;

  span<z_thread_stack_element> stack_ = span<z_thread_stack_element>();
};

// Static thread context allocation including the stack along with the
// Context.
//
// See docs.rst for an usage example.
template <size_t kStackSizeBytes>
class ContextWithStack : public Context {
 public:
  constexpr ContextWithStack() : Context() {
    set_stack(span<z_thread_stack_element>(stack_.data(), stack_.size()));
  }

 private:
  Stack<std::max(kStackSizeBytes, kMinimumStackSizeBytes)> stack_;
};

}  // namespace zephyr
}  // namespace pw::thread
