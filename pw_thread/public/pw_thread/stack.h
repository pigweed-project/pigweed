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

#include <array>
#include <cstddef>
#include <type_traits>

#include "pw_span/span.h"
#include "pw_thread_backend/stack.h"  // IWYU pragma: export

namespace pw {

/// @module{pw_thread}

namespace internal {

using ThreadStackPointer =
    decltype(std::declval<thread::backend::Stack<0>>().data());

// Returns a span of the thread stack, with std::byte used for void* stacks.
template <typename T>
constexpr auto ThreadStackSpan(T* pointer, size_t size) {
  if constexpr (std::is_void_v<T>) {
    return span(static_cast<std::byte*>(pointer), size);
  } else {
    return span(pointer, size);
  }
}

// Gets the stack size and handles void* stacks.
constexpr auto NativeStackSizeBytes(size_t size) {
  using T = std::remove_pointer_t<ThreadStackPointer>;
  return size * sizeof(std::conditional_t<std::is_void_v<T>, std::byte, T>);
}

}  // namespace internal

/// Declares a stack to use with a `ThreadContext<>` without an integrated
/// stack.
///
/// Allocating stacks alongside the `ThreadContext` (e.g. with
/// `ThreadContext<1024>`) is simpler, but more limited. Declaring a
/// `ThreadStack` separately gives you more control. For example, you can:
///
/// - Declare thread stacks at specific addresses (e.g. with
///   `PW_PLACE_IN_SECTION`.)
/// - Specify larger-than-native alignments (e.g. `alignas(256)
///   pw::ThreadStack<1024>`).
/// - Poison memory immediately before/after the stack to help detect overflow.
template <size_t kStackSizeBytes>
class ThreadStack {
 public:
  constexpr ThreadStack() : native_stack_{} {}

  /// Native stack "element". Operating systems typically accept a pointer to
  /// this type in their thread creation APIs. May be `void` for APIs that take
  /// a `void*`, such as `pthread_attr_setstack()`.
  using native = internal::ThreadStackPointer;

  /// Returns a pointer to the native stack.
  ///
  /// @warning Calling this function is not portable.
  [[nodiscard]] constexpr native native_pointer() {
    return native_stack_.data();
  }

  /// Returns the size of the stack in terms of the native units -- NOT
  /// necessarily bytes!
  ///
  /// @warning Calling this function is not portable.
  [[nodiscard]] constexpr size_t native_size() const {
    return native_stack_.size();
  }

 private:
  static_assert(
      std::is_same_v<decltype(thread::backend::Stack<kStackSizeBytes>().data()),
                     native>,
      "Stack pointer type must not change with size");
  static_assert(std::is_pointer_v<native>, ".data() must return a pointer");

  thread::backend::Stack<kStackSizeBytes> native_stack_;
};

}  // namespace pw
