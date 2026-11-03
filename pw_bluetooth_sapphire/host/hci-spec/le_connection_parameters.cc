// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/hci-spec/le_connection_parameters.h"

#include <cpp-string/string_printf.h>

namespace bt::hci_spec {

namespace {

// The length of a timeslice in the parameters, in milliseconds.
constexpr static float kTimesliceMs = 1.25f;

}  // namespace

LEConnectionParameters::LEConnectionParameters(uint16_t interval,
                                               uint16_t latency,
                                               uint16_t supervision_timeout)
    : interval_(interval),
      latency_(latency),
      supervision_timeout_(supervision_timeout) {}

LEConnectionParameters::LEConnectionParameters()
    : interval_(0), latency_(0), supervision_timeout_(0) {}

bool hci_spec::LEConnectionParameters::operator==(
    const hci_spec::LEConnectionParameters& other) const {
  return interval_ == other.interval_ && latency_ == other.latency_ &&
         supervision_timeout_ == other.supervision_timeout_;
}

std::string hci_spec::LEConnectionParameters::ToString() const {
  return bt_lib_cpp_string::StringPrintf(
      "interval: %.2f ms, latency: %.2f ms, timeout: %u ms",
      static_cast<float>(interval_) * kTimesliceMs,
      static_cast<float>(latency_) * kTimesliceMs,
      supervision_timeout_ * 10u);
}

bool LEPreferredConnectionParameters::operator==(
    const LEPreferredConnectionParameters& other) const {
  return min_interval_ == other.min_interval_ &&
         max_interval_ == other.max_interval_ &&
         max_latency_ == other.max_latency_ &&
         supervision_timeout_ == other.supervision_timeout_;
}

}  // namespace bt::hci_spec
