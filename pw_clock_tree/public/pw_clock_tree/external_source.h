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

#include "pw_clock_tree/clock_tree.h"
#include "pw_digital_io/digital_io.h"
#include "pw_thread/sleep.h"

/// Clock tree management library
namespace pw::clock_tree {

/// @module{pw_clock_tree}

/// Class that represents an external clock source enabled by a GPIO line.
class ExternalClockSource final : public ClockSource<ElementBlocking> {
 public:
  /// Constructor specifying the digital I/O line controlling the output of the
  /// external clock source, along with optional post-activation and
  /// post-deactivation delays.
  constexpr ExternalClockSource(
      pw::digital_io::DigitalOut& enable_line,
      pw::chrono::SystemClock::duration activation_delay =
          pw::chrono::SystemClock::duration::zero(),
      pw::chrono::SystemClock::duration deactivation_delay =
          pw::chrono::SystemClock::duration::zero())
      : enable_line_(&enable_line),
        activation_delay_(activation_delay),
        deactivation_delay_(deactivation_delay) {}
  constexpr ExternalClockSource(
      pw::chrono::SystemClock::duration activation_delay =
          pw::chrono::SystemClock::duration::zero(),
      pw::chrono::SystemClock::duration deactivation_delay =
          pw::chrono::SystemClock::duration::zero())
      : activation_delay_(activation_delay),
        deactivation_delay_(deactivation_delay) {}

  void SetOutLine(pw::digital_io::DigitalOut& enable_line) {
    enable_line_ = &enable_line;
  }

 private:
  pw::digital_io::DigitalOut* enable_line_ = nullptr;
  pw::chrono::SystemClock::duration activation_delay_;
  pw::chrono::SystemClock::duration deactivation_delay_;

  /// Activate external clock source.
  Status DoEnable() final {
    if (enable_line_ == nullptr) {
      return pw::Status::FailedPrecondition();
    }
    PW_TRY(enable_line_->SetStateActive());
    if (activation_delay_ > pw::chrono::SystemClock::duration::zero()) {
      pw::this_thread::sleep_for(activation_delay_);
    }
    return pw::OkStatus();
  }

  /// Deactivate external clock source.
  Status DoDisable() final {
    if (enable_line_ == nullptr) {
      return pw::Status::FailedPrecondition();
    }
    PW_TRY(enable_line_->SetStateInactive());
    if (deactivation_delay_ > pw::chrono::SystemClock::duration::zero()) {
      pw::this_thread::sleep_for(deactivation_delay_);
    }
    return pw::OkStatus();
  }
};

}  // namespace pw::clock_tree
