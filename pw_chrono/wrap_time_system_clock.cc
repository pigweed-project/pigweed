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

// This file provides an implementation of time and gettimeofday that can be
// used with ld's --wrap option.

#include <time.h>

#if __has_include(<sys/time.h>)
#include <sys/time.h>
#endif  // __has_include(<sys/time.h>)

#include <sys/time.h>

#include <chrono>

#include "pw_chrono/system_clock.h"

namespace pw::chrono {

extern "C" time_t __wrap_time(time_t* t) {
  const pw::chrono::SystemClock::time_point now =
      pw::chrono::SystemClock::now();
  const time_t ret = static_cast<time_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count());
  if (t) {
    *t = ret;
  }
  return ret;
}

#if __has_include(<sys/time.h>)

extern "C" int __wrap_gettimeofday(struct timeval* tv, void* tz) {
  using namespace std::chrono_literals;
  // The use of the timezone structure is obsolete (see docs "man
  // gettimeofday"). Thus we don't consider it.
  (void)tz;
  // Get time since epoch from system clock.
  const pw::chrono::SystemClock::time_point now =
      pw::chrono::SystemClock::now();
  const auto usec_since_epoch =
      std::chrono::duration_cast<std::chrono::microseconds>(
          now.time_since_epoch());
  tv->tv_sec = static_cast<time_t>(usec_since_epoch / 1s);
  tv->tv_usec =
      static_cast<decltype(tv->tv_usec)>((usec_since_epoch % 1s).count());
  return 0;
}

#endif  // __has_include(<sys/time.h>)

}  // namespace pw::chrono
