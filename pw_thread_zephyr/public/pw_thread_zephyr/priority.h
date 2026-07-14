// Copyright 2025 The Pigweed Authors
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

#include "pw_thread_zephyr/config.h"

namespace pw::thread::zephyr {

using PriorityType = int;

// TODO: https://pwbug.dev/418247843 - expose
// CONFIG_PIGWEED_THREAD_NUM_COOP_PRIORITIES when cooperative threads are
// supported.

/// The lowest priority of a thread
inline constexpr int kLowestPriority =
    ::pw::thread::zephyr::config::kNumPreemptPriorities - 1;

/// The highest priority of a preemptive thread
inline constexpr int kHighestPriority = 0;

/// The default thread priority
inline constexpr int kDefaultPriority =
    ::pw::thread::zephyr::config::kDefaultPriority;

static_assert(kDefaultPriority <= kLowestPriority);
static_assert(kDefaultPriority >= kHighestPriority);

}  // namespace pw::thread::zephyr
