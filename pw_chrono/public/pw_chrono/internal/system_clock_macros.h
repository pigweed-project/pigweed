// Copyright 2020 The Pigweed Authors
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

// IWYU pragma: private, include "pw_chrono/system_clock.h"

#include <stdbool.h>

#include "pw_assert/assert.h"
#include "pw_numeric/checked_arithmetic.h"

// The C implementation of this macro requires a C99 compound literal. In C++,
// avoid the compound literal in case -Wc99-extensions is enabled.
#ifdef __cplusplus
#define _PW_SYSTEM_CLOCK_DURATION(num_ticks) \
  (pw_chrono_SystemClock_Duration{.ticks = (num_ticks)})
#else
#define _PW_SYSTEM_CLOCK_DURATION(num_ticks) \
  ((pw_chrono_SystemClock_Duration){.ticks = (num_ticks)})
#endif  // __cplusplus

/// Returns the number of ticks for a given number of time units.
///
/// The seconds per time unit is the reciprocal of the given numerator and
/// denominator. The ticks per second is the reciprocal of the system clock
/// period. Together 'count * (seconds/time unit) * (ticks/second)' gives the
/// total number of ticks, which must then be rounded up or down to an integer
/// value.
///
/// This function will crash if the given `count` causes it an overflow. For any
/// reasonable time granularities and time units, e.g. nanoseconds and hours,
/// this only occurs when converting durations representing hundreds of billions
/// of years, and very likely indicates an error.
///
/// The numerator and denominator parameters are NOT checked for overflow, as
/// this method should not be called directly. Instead, use the
/// `PW_SYSTEM_CLOCK_*_CEIL` and `PW_SYSTEM_CLOCK_*_FLOOR` macros below, which
/// provide values that will not overflow for any supported time unit.
#ifdef __cplusplus
constexpr
#else
static inline
#endif
    int64_t pw_chrono_internal_TimeToTicks(
        int64_t count,
        int64_t time_unit_seconds_numerator,
        int64_t time_unit_seconds_denominator,
        bool round_up) {
  int64_t divisor = time_unit_seconds_numerator *
                    PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_NUMERATOR;
  int64_t dividend = time_unit_seconds_denominator *
                     PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_DENOMINATOR;
  PW_ASSERT(!PW_MUL_OVERFLOW(dividend, count, &dividend));
  if (round_up) {
    PW_ASSERT(!PW_ADD_OVERFLOW(dividend, divisor - 1, &dividend));
  }
  return dividend / divisor;
}

#define _PW_SYSTEM_CLOCK_TIME_TO_DURATION_CEIL(                        \
    count, time_unit_seconds_numerator, time_unit_seconds_denominator) \
  _PW_SYSTEM_CLOCK_DURATION(                                           \
      pw_chrono_internal_TimeToTicks(count,                            \
                                     time_unit_seconds_numerator,      \
                                     time_unit_seconds_denominator,    \
                                     /* round_up: */ 1))
#define _PW_SYSTEM_CLOCK_TIME_TO_DURATION_FLOOR(                       \
    count, time_unit_seconds_numerator, time_unit_seconds_denominator) \
  _PW_SYSTEM_CLOCK_DURATION(                                           \
      pw_chrono_internal_TimeToTicks(count,                            \
                                     time_unit_seconds_numerator,      \
                                     time_unit_seconds_denominator,    \
                                     /* round_up: */ 0))

// clang-format off

#define PW_SYSTEM_CLOCK_MS_CEIL(milliseconds) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_CEIL(milliseconds, 1000, 1)
#define PW_SYSTEM_CLOCK_S_CEIL(seconds) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_CEIL(seconds,         1, 1)
#define PW_SYSTEM_CLOCK_MIN_CEIL(minutes) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_CEIL(minutes,         1, 60)
#define PW_SYSTEM_CLOCK_H_CEIL(hours) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_CEIL(hours,           1, 60 * 60)

#define PW_SYSTEM_CLOCK_MS_FLOOR(milliseconds) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_FLOOR(milliseconds, 1000, 1)
#define PW_SYSTEM_CLOCK_S_FLOOR(seconds) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_FLOOR(seconds,         1, 1)
#define PW_SYSTEM_CLOCK_MIN_FLOOR(minutes) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_FLOOR(minutes,         1, 60)
#define PW_SYSTEM_CLOCK_H_FLOOR(hours) \
  _PW_SYSTEM_CLOCK_TIME_TO_DURATION_FLOOR(hours,           1, 60 * 60)

// clang-format on

#define PW_SYSTEM_CLOCK_MS(milliseconds) PW_SYSTEM_CLOCK_MS_CEIL(milliseconds)
#define PW_SYSTEM_CLOCK_S(seconds) PW_SYSTEM_CLOCK_S_CEIL(seconds)
#define PW_SYSTEM_CLOCK_MIN(minutes) PW_SYSTEM_CLOCK_MIN_CEIL(minutes)
#define PW_SYSTEM_CLOCK_H(hours) PW_SYSTEM_CLOCK_H_CEIL(hours)
