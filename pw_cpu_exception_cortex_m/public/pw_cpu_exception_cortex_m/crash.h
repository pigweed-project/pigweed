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

#include "pw_cpu_exception/state.h"
// The cpu exception cortex m crash backend must provide a definition for the
// PW_CPU_EXCEPTION_CORTEX_M_HANDLE_CRASH function macro through this header.
#include "pw_cpu_exception_cortex_m_backend/crash.h"  // IWYU pragma: export

// Handles crashes given a CPU state and an analysis message with optional
// variable arguments.
//
// Args:
//  state: The CPU state at the time of crash.
//  reason_and_optional_args: A text or format string and arguments with the
//    crash reason.
//
// For example, the implementation below saves the message in a safe location
// and reboots.
//
//   #define PW_CPU_EXCEPTION_CORTEX_M_HANDLE_CRASH(state,
//                                                  reason_and_optional_args...)
//   do {
//     PW_TOKENIZE_TO_BUFFER(persistent_buffer, &size,
//                           reason_and_optional_args);
//     reboot();
//   } while (0)
//
#define PW_CPU_EXCEPTION_CORTEX_M_CRASH(state, ...) \
  PW_CPU_EXCEPTION_CORTEX_M_HANDLE_CRASH(state, __VA_ARGS__)

namespace pw::cpu_exception::cortex_m {

// Analyses the CPU state and crashes calling PW_CPU_EXCEPTION_CORTEX_M_CRASH(),
// Passing along the thread name that led to the crash.
// This can be helpful inside an exception handler to analyze the state for
// later reporting. For example,
//
//   PW_NO_RETURN void pw_cpu_exception_HandleException(void* cpu_state) {
//     AnalyzeCpuStateAndCrash(state);
//     PW_UNREACHABLE;
//   }
//
// This example assumes that the PW_CPU_EXCEPTION_CORTEX_M_HANDLE_CRASH
// implementation does not return.
void AnalyzeCpuStateAndCrash(const pw_cpu_exception_State& state,
                             const char* optional_thread_name = nullptr);
}  // namespace pw::cpu_exception::cortex_m
