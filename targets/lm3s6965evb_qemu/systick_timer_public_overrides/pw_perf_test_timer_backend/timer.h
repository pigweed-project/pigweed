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

#include <cstdint>

#include "FreeRTOS.h"
#include "pw_perf_test/internal/duration_unit.h"
#include "task.h"

namespace pw::perf_test::internal::backend {

using Timestamp = uint64_t;

inline constexpr DurationUnit kDurationUnit = DurationUnit::kClockCycle;

// Returns a monotonic 64-bit CPU cycle timestamp by combining FreeRTOS's coarse
// tick counter (xTaskGetTickCount) with the ARMv7-M 24-bit SysTick hardware
// down-counter:
//
// - SYST_RVR (0xE000E014, Reload Value Register) holds (N - 1), where N is the
//   number of CPU clock cycles per OS tick.
// - SYST_CVR (0xE000E018, Current Value Register) decrements on each CPU clock
//   cycle from `reload` down to 0. When it transitions from 1 to 0, it reloads
//   `reload` on the next cycle and latches the SysTick exception pending bit
//   (PENDSTSET, bit 26) in SCB->ICSR (0xE000ED04).
// - Inside taskENTER_CRITICAL(), interrupts are masked so the SysTick ISR
//   cannot run and increment xTaskGetTickCount(). If SYST_CVR wraps around
//   before or during the read, SCB->ICSR's PENDSTSET bit will be set. When
//   detected, we re-read SYST_CVR (to guarantee a post-wrap value near
//   `reload`) and add 1 to the local `tick` count.
inline Timestamp GetCurrentTimestamp() {
  volatile uint32_t* const kSysTickLoad =
      reinterpret_cast<volatile uint32_t*>(0xE000E014U);
  volatile uint32_t* const kSysTickVal =
      reinterpret_cast<volatile uint32_t*>(0xE000E018U);
  volatile uint32_t* const kScbIcsr =
      reinterpret_cast<volatile uint32_t*>(0xE000ED04U);
  constexpr uint32_t kPendStSetBit = 1U << 26;
  constexpr Timestamp kCyclesPerTick = configCPU_CLOCK_HZ / configTICK_RATE_HZ;

  const uint32_t reload = *kSysTickLoad;
  if (reload == 0) {
    // Fallback if queried before SysTick is initialized by the scheduler.
    return static_cast<Timestamp>(xTaskGetTickCount()) * kCyclesPerTick;
  }

  taskENTER_CRITICAL();
  TickType_t tick = xTaskGetTickCount();
  uint32_t val = *kSysTickVal;
  if ((*kScbIcsr & kPendStSetBit) != 0) {
    val = *kSysTickVal;
    ++tick;
  }
  taskEXIT_CRITICAL();

  return static_cast<Timestamp>(tick) * (reload + 1ULL) + (reload - val);
}

inline int64_t& CalibratedOverhead() {
  static int64_t overhead = 0;
  return overhead;
}

[[nodiscard]] inline bool TimerPrepare() {
  CalibratedOverhead() = 0;
  int64_t min_overhead = INT64_MAX;
  for (int i = 0; i < 32; ++i) {
    Timestamp start = GetCurrentTimestamp();
    Timestamp end = GetCurrentTimestamp();
    int64_t diff = static_cast<int64_t>(end - start);
    if (diff >= 0 && diff < min_overhead) {
      min_overhead = diff;
    }
  }
  if (min_overhead != INT64_MAX) {
    CalibratedOverhead() = min_overhead;
  }
  return true;
}

inline void TimerCleanup() {}

inline int64_t GetDuration(Timestamp begin, Timestamp end) {
  const int64_t raw = static_cast<int64_t>(end - begin);
  const int64_t calibrated = raw - CalibratedOverhead();
  return calibrated > 0 ? calibrated : 0;
}

}  // namespace pw::perf_test::internal::backend
