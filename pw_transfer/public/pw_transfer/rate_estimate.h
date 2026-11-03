// Copyright 2022 The Pigweed Authors
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

#include <chrono>
#include <cstdint>
#include <optional>

#include "pw_chrono/system_clock.h"

namespace pw::transfer {

/// @module{pw_transfer}

/// Estimates the data transfer rate in bytes per second over elapsed wall time.
class RateEstimate {
 public:
  /// Constructs an unstarted `RateEstimate`.
  constexpr RateEstimate() : start_time_(std::nullopt), bytes_transferred_(0) {}

  /// Resets the start time to the current time and clears the byte counter.
  void Reset() {
    start_time_ = chrono::SystemClock::now();
    bytes_transferred_ = 0;
  }

  /// Records additional transferred bytes.
  ///
  /// @param[in] new_bytes Number of newly transferred bytes to add.
  constexpr void Update(size_t new_bytes) { bytes_transferred_ += new_bytes; }

  /// Calculates the average transfer rate since `Reset()` was called.
  ///
  /// @returns The estimated transfer rate in bytes per second, or `0` if
  /// `Reset()` has not been called or less than 1 millisecond has elapsed.
  size_t GetRateBytesPerSecond() const;

 private:
  std::optional<chrono::SystemClock::time_point> start_time_;
  size_t bytes_transferred_;
};

/// @endmodule

}  // namespace pw::transfer
