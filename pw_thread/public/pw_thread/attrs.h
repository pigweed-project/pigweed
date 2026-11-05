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

#include <cstddef>
#include <type_traits>

#include "pw_span/span.h"
#include "pw_thread/priority.h"
#include "pw_thread/stack.h"

namespace pw {

/// @module{pw_thread}

/// Generic attributes of a thread. `ThreadAttrs` is used with a `ThreadContext`
/// to create threads.
///
/// `ThreadAtts` provides three attributes: name, priority, stack size, and
/// stack location. These attributes should be considered only as hints -- the
/// pw_thread backend may not support them.
class ThreadAttrs {
 public:
  /// Initializes attributes to their backend-defined defaults.
  constexpr ThreadAttrs()
      : name_(""),
        priority_(),
        stack_(nullptr),
        stack_size_(thread::backend::kDefaultStackSizeBytes) {}

  /// Thread attributes can be copied to share properties between threads.
  constexpr ThreadAttrs(const ThreadAttrs&) = default;
  constexpr ThreadAttrs& operator=(const ThreadAttrs&) = default;

  /// Name hint as a null-terminated string. Never null.
  constexpr const char* name() const { return name_; }

  constexpr ThreadAttrs& set_name(const char* name) {
    PW_DASSERT(name != nullptr);
    name_ = name;
    return *this;
  }

  constexpr ThreadAttrs& set_name(std::nullptr_t) = delete;

  constexpr ThreadPriority priority() const { return priority_; }

  /// Sets a thread priority hint.
  constexpr ThreadAttrs& set_priority(ThreadPriority priority) {
    priority_ = priority;
    return *this;
  }

  /// Returns a span of the native stack to use for this thread. The stack may
  /// not be in terms of bytes! Backends that use `void*` for stacks return a
  /// `std::byte` span. If the backend doesn't support user-specified stacks,
  ///
  /// This function is NOT `constexpr` if the backend uses `void*` for stacks.
  constexpr auto native_stack() const {
    PW_ASSERT(has_external_stack());
    return internal::ThreadStackSpan(stack_, stack_size_);
  }

  /// Returns a pointer to the native stack to use for this thread.
  ///
  /// @warning This function is NOT portable!
  constexpr auto native_stack_pointer() const { return stack_; }

  /// Returns the size of the stack in native units (not necessarily bytes),
  /// using the native type (typically an unsigned integer).
  ///
  /// @warning This function is NOT portable!
  constexpr auto native_stack_size() const {
    PW_ASSERT(has_external_stack());
    return stack_size_;
  }

  /// Returns the size of the stack in bytes.
  constexpr size_t stack_size_bytes() const {
    if (has_external_stack()) {
      return internal::NativeStackSizeBytes(stack_size_);
    }
    return stack_size_;
  }

  /// Sets the thread stack size to use for a stack provided by the
  /// `ThreadContext`. If 0, the thread backend's minimum stack size is used.
  /// @pre An external stack has not been set with `set_stack()`.
  constexpr ThreadAttrs& set_stack_size_bytes(size_t stack_size_bytes) {
    PW_ASSERT(!has_external_stack());
    stack_size_ = stack_size_bytes;
    return *this;
  }

  /// Sets the thread to use the provided stack, instead of a stack integrated
  /// into the `ThreadContext`.
  template <size_t kStackSizeBytes>
  constexpr ThreadAttrs& set_stack(ThreadStack<kStackSizeBytes>& stack) {
    stack_ = stack.native_pointer();
    stack_size_ = stack.native_size();
    return *this;
  }

  /// Clears a previous call to `set_stack`.
  constexpr ThreadAttrs& clear_stack() {
    stack_ = nullptr;
    stack_size_ = thread::backend::kDefaultStackSizeBytes;
    return *this;
  }

  /// True if the `ThreadAttrs` use an externally allocated stack, rather than
  /// one integrated with the `ThreadContext`.
  [[nodiscard]] constexpr bool has_external_stack() const {
    return stack_ != nullptr;
  }

 private:
  const char* name_;
  ThreadPriority priority_;

  internal::ThreadStackPointer stack_;
  size_t stack_size_;
};

}  // namespace pw
