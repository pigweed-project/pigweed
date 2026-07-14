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

#include <cstddef>

#if defined(CONFIG_PIGWEED_THREAD)

// When Kconfig is in use, all configuration MUST come through Kconfig.
#if defined(PW_THREAD_ZEPHYR_CONFIG_HEADER) ||                 \
    defined(PW_THREAD_ZEPHYR_CONFIG_NUM_PREEMPT_PRIORITIES) || \
    defined(PW_THREAD_ZEPHYR_CONFIG_DEFAULT_PRIORITY) ||       \
    defined(PW_THREAD_ZEPHYR_CONFIG_MINIMUM_STACK_SIZE) ||     \
    defined(PW_THREAD_ZEPHYR_CONFIG_DEFAULT_STACK_SIZE) ||     \
    defined(PW_THREAD_ZEPHYR_CONFIG_MAX_THREAD_NAME_LEN)
#error \
    "Do not define PW_THREAD_ZEPHYR_CONFIG options or PW_THREAD_ZEPHYR_CONFIG_HEADER when CONFIG_PIGWEED_THREAD is enabled in Kconfig. Configure via Kconfig instead."
#endif  // defined(PW_THREAD_ZEPHYR_CONFIG_...)

#define PW_THREAD_ZEPHYR_CONFIG_NUM_PREEMPT_PRIORITIES \
  CONFIG_PIGWEED_THREAD_NUM_PREEMPT_PRIORITIES
#define PW_THREAD_ZEPHYR_CONFIG_DEFAULT_PRIORITY \
  CONFIG_PIGWEED_THREAD_DEFAULT_PRIORITY
#define PW_THREAD_ZEPHYR_CONFIG_MINIMUM_STACK_SIZE \
  CONFIG_PIGWEED_THREAD_MINIMUM_STACK_SIZE
#define PW_THREAD_ZEPHYR_CONFIG_DEFAULT_STACK_SIZE \
  CONFIG_PIGWEED_THREAD_DEFAULT_STACK_SIZE
#define PW_THREAD_ZEPHYR_CONFIG_MAX_THREAD_NAME_LEN \
  CONFIG_PIGWEED_THREAD_MAX_THREAD_NAME_LEN

#else  // !defined(CONFIG_PIGWEED_THREAD)

// When Kconfig is not in use (e.g., standalone GN/Bazel builds), allow
// PW_THREAD_ZEPHYR_CONFIG_HEADER to provide overrides, falling back to C++
// defaults.
#if defined(PW_THREAD_ZEPHYR_CONFIG_HEADER)
#include PW_THREAD_ZEPHYR_CONFIG_HEADER
#endif  // defined(PW_THREAD_ZEPHYR_CONFIG_HEADER)

#ifndef PW_THREAD_ZEPHYR_CONFIG_NUM_PREEMPT_PRIORITIES
#define PW_THREAD_ZEPHYR_CONFIG_NUM_PREEMPT_PRIORITIES \
  CONFIG_NUM_PREEMPT_PRIORITIES
#endif  // PW_THREAD_ZEPHYR_CONFIG_NUM_PREEMPT_PRIORITIES

#ifndef PW_THREAD_ZEPHYR_CONFIG_DEFAULT_PRIORITY
#define PW_THREAD_ZEPHYR_CONFIG_DEFAULT_PRIORITY CONFIG_MAIN_THREAD_PRIORITY
#endif  // PW_THREAD_ZEPHYR_CONFIG_DEFAULT_PRIORITY

#ifndef PW_THREAD_ZEPHYR_CONFIG_MINIMUM_STACK_SIZE
#define PW_THREAD_ZEPHYR_CONFIG_MINIMUM_STACK_SIZE 256
#endif  // PW_THREAD_ZEPHYR_CONFIG_MINIMUM_STACK_SIZE

#ifndef PW_THREAD_ZEPHYR_CONFIG_DEFAULT_STACK_SIZE
#define PW_THREAD_ZEPHYR_CONFIG_DEFAULT_STACK_SIZE 1024
#endif  // PW_THREAD_ZEPHYR_CONFIG_DEFAULT_STACK_SIZE

#ifndef PW_THREAD_ZEPHYR_CONFIG_MAX_THREAD_NAME_LEN
#define PW_THREAD_ZEPHYR_CONFIG_MAX_THREAD_NAME_LEN 15
#endif  // PW_THREAD_ZEPHYR_CONFIG_MAX_THREAD_NAME_LEN

#endif  // defined(CONFIG_PIGWEED_THREAD)

namespace pw::thread::zephyr::config {

inline constexpr int kNumPreemptPriorities =
    PW_THREAD_ZEPHYR_CONFIG_NUM_PREEMPT_PRIORITIES;
inline constexpr int kDefaultPriority =
    PW_THREAD_ZEPHYR_CONFIG_DEFAULT_PRIORITY;
inline constexpr size_t kMinimumStackSizeBytes =
    PW_THREAD_ZEPHYR_CONFIG_MINIMUM_STACK_SIZE;
inline constexpr size_t kDefaultStackSizeBytes =
    PW_THREAD_ZEPHYR_CONFIG_DEFAULT_STACK_SIZE;
inline constexpr size_t kMaximumNameLength =
    PW_THREAD_ZEPHYR_CONFIG_MAX_THREAD_NAME_LEN;

}  // namespace pw::thread::zephyr::config
