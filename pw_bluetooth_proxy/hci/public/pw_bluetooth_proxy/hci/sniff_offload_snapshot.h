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

#include "pw_function/function.h"

namespace pw::bluetooth::proxy::hci {

struct SniffSnapshot {
  bool sniff_enabled = false;
  uint16_t subrating_max_latency = 0;
  uint16_t subrating_min_remote_timeout = 0;
  uint16_t subrating_min_local_timeout = 0;
  bool suppress_mode_change_event = false;
  bool suppress_sniff_subrating_event = false;
};

using SniffStateUpdate = SniffSnapshot;

/// Callback type invoked when the Sniff subsystem state mutates.
///
/// @warning **Re-entrancy Safety:** Do not invoke proxy methods from within
/// this callback; it is called synchronously while holding internal mutexes.
using SniffStateUpdateCallback =
    pw::Function<void(const SniffStateUpdate& update)>;

}  // namespace pw::bluetooth::proxy::hci
