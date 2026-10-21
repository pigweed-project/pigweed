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

#include <stddef.h>
#include <stdint.h>

#include "pw_preprocessor/util.h"

// The backend implements this header to provide the following SystemClock
// parameters, for more detail on the parameters see the SystemClock usage of
// them below:
//   PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_NUMERATOR
//   PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_DENOMINATOR
//   constexpr pw::chrono::Epoch pw::chrono::backend::kSystemClockEpoch;
//   constexpr bool pw::chrono::backend::kSystemClockFreeRunning;
//   constexpr bool pw::chrono::backend::kSystemClockNmiSafe;
#include "pw_chrono_backend/system_clock_config.h"  // IWYU pragma: export

#ifdef __cplusplus

#include <chrono>
#include <ratio>

#include "pw_chrono/virtual_clock.h"
#include "pw_numeric/saturating_arithmetic.h"

/// Portable std::chrono library for embedded
namespace pw::chrono {

/// @module{pw_chrono}

namespace backend {

/// The ARM AEBI does not permit the opaque 'time_point' to be passed via
/// registers, ergo the underlying fundamental type is forward declared.
/// A SystemCLock tick has the units of one SystemClock::period duration.
/// This must be thread and IRQ safe and provided by the backend.
///
int64_t GetSystemClockTickCount();

}  // namespace backend

/// The `SystemClock` represents an unsteady, monotonic clock.
///
/// The epoch of this clock is unspecified and may not be related to wall time
/// (for example, it can be time since boot). The time between ticks of this
/// clock may vary due to sleep modes and potential interrupt handling.
/// `SystemClock` meets the requirements of C++'s `TrivialClock` and Pigweed's
/// `PigweedClock.`
///
/// `SystemClock` is compatible with C++'s `Clock` & `TrivialClock` including:
/// - `SystemClock::rep`
/// - `SystemClock::period`
/// - `SystemClock::duration`
/// - `SystemClock::time_point`
/// - `SystemClock::is_steady`
/// - `SystemClock::now()`
///
/// Example:
///
/// @code
///   SystemClock::time_point before = SystemClock::now();
///   TakesALongTime();
///   SystemClock::duration time_taken = SystemClock::now() - before;
///   bool took_way_too_long = false;
///   if (time_taken > std::chrono::seconds(42)) {
///     took_way_too_long = true;
///   }
/// @endcode
///
/// This code is thread & IRQ safe, it may be NMI safe depending on is_nmi_safe.
///
struct SystemClock {
  using rep = int64_t;
  /// The period must be provided by the backend.
  using period = std::ratio<PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_NUMERATOR,
                            PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_DENOMINATOR>;
  /// Alias for durations representable with this clock.
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SystemClock>;
  /// The epoch must be provided by the backend.
  static constexpr Epoch epoch = backend::kSystemClockEpoch;

  /// The time points of this clock cannot decrease, however the time between
  /// ticks of this clock may slightly vary due to sleep modes. The duration
  /// during sleep may be ignored or backfilled with another clock.
  static constexpr bool is_monotonic = true;
  static constexpr bool is_steady = false;

  /// The now() function may not move forward while in a critical section or
  /// interrupt. This must be provided by the backend.
  static constexpr bool is_free_running = backend::kSystemClockFreeRunning;

  /// The clock must stop while in halting debug mode.
  static constexpr bool is_stopped_in_halting_debug_mode = true;

  /// The now() function can be invoked at any time.
  static constexpr bool is_always_enabled = true;

  /// The now() function may work in non-maskable interrupt contexts (e.g.
  /// exception/fault handlers), depending on the backend. This must be provided
  /// by the backend.
  static constexpr bool is_nmi_safe = backend::kSystemClockNmiSafe;

  /// This is thread and IRQ safe. This must be provided by the backend.
  static time_point now() noexcept {
    return time_point(duration(backend::GetSystemClockTickCount()));
  }

  /// This is purely a helper, identical to directly using std::chrono::ceil, to
  /// convert a duration type which cannot be implicitly converted where the
  /// result is rounded up.
  template <class Rep, class Period>
  static constexpr duration for_at_least(std::chrono::duration<Rep, Period> d) {
    return std::chrono::ceil<duration>(d);
  }

  /// Computes the nearest time_point after the specified duration has elapsed.
  ///
  /// This is useful for translating delay or timeout durations into deadlines.
  ///
  /// The returned time_point will either be strictly after the specified
  /// duration from now, or the latest expressible time_point for the
  /// SystemClock's epoch, whichever is sooner.
  static time_point TimePointAfterAtLeast(duration after_at_least) {
    rep ticks = backend::GetSystemClockTickCount() + 1;
    return time_point(duration(add_sat(ticks, after_at_least.count())));
  }
};

// NOTE: VirtualClock here is specialized on SystemClock in order to provide
// the `RealClock` function.
//
// SystemClock is defined in this file, so there's no risk of an ODR violation
// as other libraries are unable to spell VirtualClock<SystemClock> unless
// they have first included this file.

/// An abstract interface representing a SystemClock.
///
/// This interface allows decoupling code that uses time from the code that
/// creates a point in time. You can use this to your advantage by injecting
/// Clocks into interfaces rather than having implementations call
/// `SystemClock::now()` directly. However, this comes at a cost of a vtable per
/// implementation and more importantly passing and maintaining references to
/// the VirtualClock for all of the users.
///
/// The `VirtualSystemClock::RealClock()` function returns a reference to the
/// real global SystemClock.
///
/// Example:
///
/// @code
///   void DoFoo(VirtualSystemClock& system_clock) {
///     SystemClock::time_point now = clock.now();
///     // ... Code which consumes now.
///   }
///
///   // Production code:
///   DoFoo(VirtualSystemCLock::RealClock);
///
///   // Test code:
///   MockClock test_clock();
///   DoFoo(test_clock);
/// @endcode
///
/// This interface is thread and IRQ safe.
template <>
class VirtualClock<SystemClock> {
 public:
  /// Returns a reference to the real system clock to aid instantiation.
  static VirtualClock<SystemClock>& RealClock();

  virtual ~VirtualClock() = default;

  /// Returns the current time.
  virtual SystemClock::time_point now() = 0;
};

using VirtualSystemClock = VirtualClock<SystemClock>;

}  // namespace pw::chrono

// The backend can opt to include an inlined implementation of the following:
//   int64_t GetSystemClockTickCount();
#if __has_include("pw_chrono_backend/system_clock_inline.h")
#include "pw_chrono_backend/system_clock_inline.h"  // IWYU pragma: export
#endif  // __has_include("pw_chrono_backend/system_clock_inline.h")

#endif  // __cplusplus

// C API Users should not create pw_chrono_SystemClock_Duration's directly,
// instead it is strongly recommended to use macros which express the duration
// in time units, instead of non-portable ticks.
//
// The following macros round up just like std::chrono::ceil, this is the
// recommended rounding to maintain the "at least" contract of timeouts and
// deadlines (note the *_CEIL macros are the same only more explicit):
//   PW_SYSTEM_CLOCK_MS(milliseconds)
//   PW_SYSTEM_CLOCK_S(seconds)
//   PW_SYSTEM_CLOCK_MIN(minutes)
//   PW_SYSTEM_CLOCK_H(hours)
//   PW_SYSTEM_CLOCK_MS_CEIL(milliseconds)
//   PW_SYSTEM_CLOCK_S_CEIL(seconds)
//   PW_SYSTEM_CLOCK_MIN_CEIL(minutes)
//   PW_SYSTEM_CLOCK_H_CEIL(hours)
//
// The following macros round down like std::chrono::{floor,duration_cast},
// these are discouraged but sometimes necessary:
//   PW_SYSTEM_CLOCK_MS_FLOOR(milliseconds)
//   PW_SYSTEM_CLOCK_S_FLOOR(seconds)
//   PW_SYSTEM_CLOCK_MIN_FLOOR(minutes)
//   PW_SYSTEM_CLOCK_H_FLOOR(hours)
#include "pw_chrono/internal/system_clock_macros.h"  // IWYU pragma: export

PW_EXTERN_C_START

typedef struct {
  int64_t ticks;
} pw_chrono_SystemClock_Duration;

typedef struct {
  pw_chrono_SystemClock_Duration duration_since_epoch;
} pw_chrono_SystemClock_TimePoint;
typedef int64_t pw_chrono_SystemClock_Nanoseconds;

// Returns the current time, see SystemClock::now() for more detail.
pw_chrono_SystemClock_TimePoint pw_chrono_SystemClock_Now(void);

// Returns the change in time between the current_time - last_time.
pw_chrono_SystemClock_Duration pw_chrono_SystemClock_TimeElapsed(
    pw_chrono_SystemClock_TimePoint last_time,
    pw_chrono_SystemClock_TimePoint current_time);

// For lossless time unit conversion, the seconds per tick ratio that is
// numerator/denominator should be used:
//   PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_NUMERATOR
//   PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_DENOMINATOR

// Warning, this may be lossy due to the use of std::chrono::floor,
// rounding towards zero.
pw_chrono_SystemClock_Nanoseconds pw_chrono_SystemClock_DurationToNsFloor(
    pw_chrono_SystemClock_Duration duration);

PW_EXTERN_C_END
