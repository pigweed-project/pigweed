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

// Baremetal ARM Cortex-M cycle counter backend using the architectural 24-bit
// SysTick down-counter (0xE000E010..0xE000E018) configured for free-running
// wraparound at processor clock rate without interrupts.
//
// Use this backend on baremetal Cortex-M targets that do not implement the DWT
// CYCCNT register (such as Cortex-M0/M0+/M23 or QEMU Cortex-M emulation with
// -icount). Do not use this backend when an RTOS scheduler owns SysTick.

#pragma once

#include <cstdint>

#include "pw_perf_test/internal/duration_unit.h"

namespace pw::perf_test::internal::backend {

// Architectural ARMv6-M / ARMv7-M / ARMv8-M 24-bit SysTick registers.
inline volatile uint32_t& kSysTickCsr =
    *reinterpret_cast<volatile uint32_t*>(0xE000E010);
inline volatile uint32_t& kSysTickRvr =
    *reinterpret_cast<volatile uint32_t*>(0xE000E014);
inline volatile uint32_t& kSysTickCvr =
    *reinterpret_cast<volatile uint32_t*>(0xE000E018);

inline constexpr uint32_t kSysTickMaxReload = 0x00FFFFFFu;
inline constexpr uint32_t kSysTickEnableCpuClock = 0x00000005u;

using Timestamp = uint32_t;

inline constexpr DurationUnit kDurationUnit = DurationUnit::kClockCycle;

[[nodiscard]] inline bool TimerPrepare() {
  kSysTickCsr = 0;
  kSysTickRvr = kSysTickMaxReload;
  kSysTickCvr = 0;
  kSysTickCsr = kSysTickEnableCpuClock;
  return (kSysTickCsr & kSysTickEnableCpuClock) == kSysTickEnableCpuClock;
}

inline void TimerCleanup() { kSysTickCsr &= ~0x00000001u; }

inline Timestamp GetCurrentTimestamp() {
  return kSysTickMaxReload - (kSysTickCvr & kSysTickMaxReload);
}

// Warning: this duration will overflow if a single iteration exceeds 2^24
// (16,777,216) clock cycles.
inline int64_t GetDuration(Timestamp begin, Timestamp end) {
  return static_cast<int64_t>((end - begin) & kSysTickMaxReload);
}

}  // namespace pw::perf_test::internal::backend
