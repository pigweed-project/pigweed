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

#include <atomic>

// Control whether 64-bit metrics are enabled.
//
// By default, 64-bit metrics are enabled automatically on 64-bit host platforms
// (e.g. x86_64, AArch64). On 32-bit target platforms (e.g. ARM Cortex-M),
// 64-bit metrics are disabled by default to prevent linker errors for atomic
// libcalls (__atomic_load_8).
//
// Downstream projects targeting 32-bit platforms can opt-in to 64-bit metrics
// by defining PW_METRIC_CONFIG_ENABLE_64BIT=1 and linking the appropriate
// atomic helper library (e.g. @pigweed//pw_atomic:cortex_m_64bit_atomics).
#ifndef PW_METRIC_CONFIG_ENABLE_64BIT
#if (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ >= 8) || \
    defined(_WIN64) || defined(__LP64__) || defined(_LP64) ||   \
    defined(__x86_64__) || defined(__aarch64__)
#define PW_METRIC_CONFIG_ENABLE_64BIT 1
#else
#define PW_METRIC_CONFIG_ENABLE_64BIT 0
#endif
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
