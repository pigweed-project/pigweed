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

#include <zephyr/kernel/thread_stack.h>

namespace pw::thread::zephyr {

/// Smallest stack size supported by Zephyr threads.
inline constexpr size_t kMinimumStackSizeBytes =
    CONFIG_PIGWEED_THREAD_MINIMUM_STACK_SIZE;

/// Default stack size for Zephyr threads
inline constexpr size_t kDefaultStackSizeBytes =
    CONFIG_PIGWEED_THREAD_DEFAULT_STACK_SIZE;

template <size_t kStackSizeBytes>
class Stack {
 public:
  // Make sure that we allocate at least kMinimumStackSizeBytes
  static constexpr size_t kResolvedStackSizeBytes =
      std::max(kStackSizeBytes, kMinimumStackSizeBytes);

  explicit constexpr Stack() = default;

  /// @return Pointer to the start of the stack
  constexpr z_thread_stack_element* data() { return stack_; }

  /// @return The size of the stack
  constexpr size_t size() const { return kResolvedStackSizeBytes; }

 private:
#ifdef CONFIG_USERSPACE
  Z_THREAD_STACK_DEFINE_IN(stack_, kResolvedStackSizeBytes, ) = {};
#else
  K_KERNEL_STACK_MEMBER(stack_, kResolvedStackSizeBytes) = {};
#endif
};
}  // namespace pw::thread::zephyr
