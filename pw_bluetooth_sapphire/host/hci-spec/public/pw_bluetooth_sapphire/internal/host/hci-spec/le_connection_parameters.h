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

#pragma once
#include <cpp-string/string_printf.h>

#include <cstdint>
#include <string>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

namespace bt::hci_spec {

// All LE connection parameters in this file are given in terms of a
// "multiplier" that to controller uses to calculate actual duration values.
// The exact value calculations and ranges depend on the context in which these
// values are used (e.g. see LECreateConnectionCommand).
//
// See the specification section referenced above each LEConnectionParameters
// getter below. See hci_constants.h for min/max allowed ranges for each
// parameter.

// Connection parameters for a LE connection. Used to represent parameters in
// active use by the link layer for a particular logical link.
class LEConnectionParameters final {
 public:
  LEConnectionParameters(uint16_t interval,
                         uint16_t latency,
                         uint16_t supervision_timeout);

  // Default constructor initializes all values to 0. This is intended for cases
  // that require default initialization, e.g. when this structure is used
  // inside a container that default initializes its contents.
  LEConnectionParameters();

  // The connection interval indicates the frequency of link layer connection
  // events over which data channel PDUs can be transmitted. See Core Spec v5.0,
  // Vol 6, Part B, Section 4.5.1 for more information on the link layer
  // connection events.
  uint16_t interval() const { return interval_; }

  // The maximum allowed connection latency. See Core Spec v5.0, Vol 6, Part
  // B, Section 4.5.2.
  uint16_t latency() const { return latency_; }

  // This defines the maximum time between two received data packet PDUs
  // before the connection is considered lost. See Core Spec v5.0, Vol 6, Part
  // B, Section 4.5.2. This value is in centiseconds and must be within
  // the range 100 ms - 32 s (10 cs - 3200 cs).
  uint16_t supervision_timeout() const { return supervision_timeout_; }

  // Equality operator
  bool operator==(const LEConnectionParameters& other) const;

  std::string ToString() const;

 private:
  uint16_t interval_;
  uint16_t latency_;
  uint16_t supervision_timeout_;
};

// Preferred connection parameters for LE connection. Used to represent
// parameters that act as a hint to the controller. This is used for:
//
//   * initiating a new connection as a central
//   * to represent a peripheral's preferred connection parameters
class LEPreferredConnectionParameters final {
 public:
  constexpr LEPreferredConnectionParameters(uint16_t min_interval,
                                            uint16_t max_interval,
                                            uint16_t max_latency,
                                            uint16_t supervision_timeout)
      : min_interval_(min_interval),
        max_interval_(max_interval),
        max_latency_(max_latency),
        supervision_timeout_(supervision_timeout) {}

  // Default constructor initializes all values to 0. This is intended for cases
  // that require default initialization, e.g. when this structure is used
  // inside a container that default initializes its contents.
  constexpr LEPreferredConnectionParameters()
      : min_interval_(0),
        max_interval_(0),
        max_latency_(0),
        supervision_timeout_(0) {}

  // See hci_spec::LEConnectionParameters for a description of these fields. See
  // hci_constants.h for the allowed ranges for these values.
  constexpr uint16_t min_interval() const { return min_interval_; }
  constexpr uint16_t max_interval() const { return max_interval_; }
  constexpr uint16_t max_latency() const { return max_latency_; }
  constexpr uint16_t supervision_timeout() const {
    return supervision_timeout_;
  }

  // Returns true if the parameters are within the ranges allowed for the
  // HCI_LE_Connection_Update command (Core Spec v6.2, Vol 4, Part E,
  // Sec 7.8.18), the minimum interval is not larger than the maximum interval,
  // and the supervision timeout satisfies
  //
  //   Supervision_Timeout > (1 + Max_Latency) * Connection_Interval_Max * 2
  //
  // (Core Spec v6.2, Vol 4, Part E, Sec 7.8.18 and Vol 6, Part B, Sec 4.5.2).
  //
  // NOTE: The Peripheral Preferred Connection Parameters characteristic allows
  // any field to be 0xFFFF to indicate "no specific value requested" (Core Spec
  // v6.2, Vol 3, Part C, Sec 12.3). pw_bluetooth_sapphire does not support
  // "no preference" values, so parameters containing 0xFFFF are invalid.
  constexpr bool IsValid() const {
    return min_interval_ <= max_interval_ &&
           min_interval_ >= kLEConnectionIntervalMin &&
           max_interval_ <= kLEConnectionIntervalMax &&
           max_latency_ <= kLEConnectionLatencyMax &&
           supervision_timeout_ >= kLEConnectionSupervisionTimeoutMin &&
           supervision_timeout_ <= kLEConnectionSupervisionTimeoutMax &&
           SupervisionTimeoutIsValid();
  }

  // Equality operator
  bool operator==(const LEPreferredConnectionParameters& other) const;

 private:
  // Returns true if the supervision timeout satisfies the requirement in the
  // Core Spec v6.2, Vol 6, Part B, Sec 4.5.2. The subrate factor in that
  // formula is not considered because it is reset to 1 when the connection
  // interval is changed with HCI_LE_Connection_Update (Vol 4, Part E,
  // Sec 7.8.18).
  constexpr bool SupervisionTimeoutIsValid() const {
    // With supervision_timeout_ in units of 10 ms and max_interval_ in units
    // of 1.25 ms, the requirement is:
    //
    //   supervision_timeout_ * 10 > (1 + max_latency_) * max_interval_ * 2.5
    //
    // Dividing both sides by 2.5:
    //
    //   supervision_timeout_ * 4 > (1 + max_latency_) * max_interval_
    return static_cast<uint64_t>(supervision_timeout_) * 4 >
           (1 + static_cast<uint64_t>(max_latency_)) *
               static_cast<uint64_t>(max_interval_);
  }

  uint16_t min_interval_;
  uint16_t max_interval_;
  uint16_t max_latency_;
  uint16_t supervision_timeout_;
};

}  // namespace bt::hci_spec
