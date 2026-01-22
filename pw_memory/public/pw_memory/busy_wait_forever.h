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

#ifdef __cplusplus

namespace pw {

/// @module{pw_toolchain}

/// Loops infinitely. Call as `pw_BusyWaitForever()` in C.
///
/// Infinite loops without side effects are undefined behavior. Use
/// `pw::BusyWaitForever` in place of an empty `while (true) {}` or `for (;;)
/// {}`.
[[noreturn]] inline void BusyWaitForever() {
  while (true) {
    asm volatile("");
  }
}

}  // namespace pw

// pw_BusyWaitForever is the C name for pw::BusyWaitForever. Only use this alias
// for code that must compile in C and C++.
[[noreturn]] inline void pw_BusyWaitForever() { ::pw::BusyWaitForever(); }

#else

#include "pw_preprocessor/compiler.h"

PW_NO_RETURN static inline void pw_BusyWaitForever(void) {
  while (1) {
    __asm__ volatile("");
  }
}

#endif  // __cplusplus
