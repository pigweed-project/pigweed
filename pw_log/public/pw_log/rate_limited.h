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

#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"

// rate_limited adds a wrapper around a normal PW_LOG call to provide a rate
// limitor parameter to suppress chatty logs and provide info on how many logs
// were suppressed
//   PW_LOG_EVERY_N_DURATION(level, min_interval_between_logs, msg, ...)
//     - Required.
//       level - An integer level as defined by pw_log/levels.h
//       min_interval_between_logs - A std::chrono::duration of the minimum
//       interval between
//              two of the same logs.
//       msg - Formattable message, same as you would use for PW_LOG or variants
//
// Does not check that input parameters have changed to un-suppress logs.

namespace pw::log::internal {

class RateLimiter {
 public:
  struct PollResult {
    uint16_t count;
    uint16_t logs_per_s;
  };

  explicit RateLimiter() {}
  ~RateLimiter() = default;

  PollResult Poll(chrono::SystemClock::duration min_interval_between_logs);

 private:
  uint32_t count_ = 0;
  chrono::SystemClock::time_point last_timestamp_;
};

}  // namespace pw::log::internal

/// @module{pw_log}

/// Logs a message at the given level, only if it hasn't been logged within
/// `min_interval_between_logs`.
///
/// @param level An integer level as defined by `pw_log/levels.h`
/// @param min_interval_between_logs A `pw::chrono::SystemClock::duration` that
/// defines the minimum time interval between unsuppressed logs
/// @param msg Formattable message, same as you would use for `PW_LOG` or
/// variants
///
/// Includes a summary of how many logs were skipped, and a rough rate in
/// integer seconds.
///
/// @warning This macro is **NOT** thread-safe. The underlying object being
/// modified by multiple threads calling the macro context may result in
/// undefined behavior.
///
/// Intended to supplement and replace widespread use of `EVERY_N` for logging.
/// The main benefit this provides is responsiveness for bursty logs.
/// `LOG_RATE_LIMITED` will log as soon as a burst starts - provided the
/// `min_interval_between_logs` has elapsed - while `EVERY_N` may sit idle for a
/// full period depending on the count state.
///
/// Note that this will not log until called again, so the summary may include
/// skipped logs from a prior burst.
#define PW_LOG_EVERY_N_DURATION(level, min_interval_between_logs, msg, ...)   \
  do {                                                                        \
    static pw::log::internal::RateLimiter rate_limiter;                       \
                                                                              \
    if (auto result = rate_limiter.Poll(min_interval_between_logs);           \
        result.count == std::numeric_limits<uint16_t>::max()) {               \
      PW_LOG(level,                                                         \
             PW_LOG_LEVEL,                                                  \
             PW_LOG_MODULE_NAME,                                            \
             PW_LOG_FLAGS,                                                  \
             msg " (skipped %d or more, %d/s)",                             \
             ##__VA_ARGS__,                                                 \
             static_cast<unsigned>(result.count),                           \
             static_cast<unsigned>(result.logs_per_s)); \
    } else if (result.count != 0) {                                           \
      PW_LOG(level,                                                         \
             PW_LOG_LEVEL,                                                  \
             PW_LOG_MODULE_NAME,                                            \
             PW_LOG_FLAGS,                                                  \
             msg " (skipped %d, %d/s)",                                     \
             ##__VA_ARGS__,                                                 \
             static_cast<unsigned>(result.count - 1),                       \
             static_cast<unsigned>(result.logs_per_s)); \
    }                                                                         \
  } while (0)

/// @}
