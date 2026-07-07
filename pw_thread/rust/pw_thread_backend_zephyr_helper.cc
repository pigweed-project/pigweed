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

#include <zephyr/kernel.h>

#include <algorithm>
#include <cstdint>
#include <limits>

// TODO: https://pwbug.dev/528377156 - These C wrappers are required as they
// depend on macros or inlined functions.  Investigate what can be moved to
// rust with bindgen.

extern "C" void pw_thread_zephyr_Yield() { k_yield(); }

extern "C" int32_t pw_thread_zephyr_Sleep(int64_t ticks) {
#ifdef CONFIG_TIMEOUT_64BIT
  // Note that unlike many other RTOSes, for a duration timeout in ticks, the
  // core kernel wait routine, z_add_timeout, for relative timeouts will always
  // add +1 tick to the duration to ensure proper "wait for at least" behavior
  // while in between a tick. This means that we do not need to add anything
  // here and the kernel will guarantee we wait the proper number of ticks plus
  // some time in the range of [1,2) extra ticks.
  return k_sleep(K_TICKS(ticks));
#else
  // We need some value we know won't overflow any internal math in the kernel.
  // We will use half of the uint32_t space as a reasonable midpoint, with
  // enough headroom that is a sufficiently long wait (~2.5 days at 10 KHz).
  // Note that if we were to use the return value of k_sleep, the number of
  // milliseconds remaining that it returns is capped at INT32_MAX, and thus
  // we'd like to be fairly certain that the requested duration of remaining
  // wait can be accurately represented if we were to reasonably return.
  int32_t remaining = 0;
  constexpr int64_t kLongTimeout = std::numeric_limits<int32_t>::max();
  while (ticks > kLongTimeout) {
    remaining = k_sleep(K_TICKS(kLongTimeout));
    if (remaining != 0) {
      return remaining;
    }
    ticks -= kLongTimeout;
  }
  // We do not add a +1 offset here. See the comment above for the 64-bit wait.
  // We do need to cast to a 32-bit value to properly downsize the duration if
  // it uses a 64-bit representation. We know the represented value must fit in
  // an unsigned 32-bit number, as we tested for negativity at the start of this
  // function, and the exit condition of the above loop means we must be <= to
  // INT32_MAX which must be representable in a uint32_t.
  return k_sleep(K_TICKS(static_cast<uint32_t>(ticks)));
#endif
}

extern "C" int32_t pw_thread_zephyr_SleepUntil(int64_t ticks) {
#ifdef CONFIG_TIMEOUT_64BIT
  // With 64-bit timeouts we can wait on a time_point, so do this directly when
  // Zephyr has been configured this way. We will sleep until the time since the
  // epoch start (boot, for the case of a monotonic system clock) we'd like to
  // wait for. Even if enough time has passed such that we're making this call
  // after the wakeup time has passed -- so if we were preempted between the
  // yield and here and we passed the deadline -- we'll then sleep for a single
  // tick.
  return k_sleep(K_TIMEOUT_ABS_TICKS(ticks));
#else
  // With 32-bit timers, sleeping until a time point is not supported. Instead
  // fall back to sleep for the duration until the upcoming time point. Note
  // that the "at least" wait is not needed as zephyr already will add this for
  // duration waits internally.
  return pw_thread_zephyr_Sleep(ticks - k_uptime_ticks());
#endif
}
