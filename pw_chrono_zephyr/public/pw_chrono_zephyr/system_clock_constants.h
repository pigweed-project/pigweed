// Copyright 2021 The Pigweed Authors
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

#include <zephyr/kernel.h>

#include "pw_chrono/system_clock.h"

namespace pw::chrono::zephyr {

// Max timeout to be used by users of the Zephyr pw::chrono::SystemClock
// backend provided by this module.
#ifdef CONFIG_TIMEOUT_64BIT
// in `z_add_timeout`, Zephyr keeps track of a list of timer objects, ordered
// by remaining time until expiration. For the 64 bit case, this uses signed
// 64-bit timeout values.
//
// In handling relative times, Zephyr also wants to add one to the timeout
// that is passed in. With K_TICK_MAX defined as INT64_MAX, adding one results
// in a negative integer value, and so Zephyr puts the timer object at the
// front of the list, where it expires immediately.
//
// This does not happen with 32 bit times as K_TICK_MAX is UINT32_MAX - 1,
// which does not overflow when one is added.
//
// By setting kMaxTimeout to K_TICK_MAX - 1, we work around this problem.
//
// Note the Zephyr implementation really should be patched to clamp the
// `dticks` value it computes so that it does not overflow. The implementation
// will also add in an additional non-trivial clock correction under certain
// circumstances which we cannot correct for here.
inline constexpr SystemClock::duration kMaxTimeout =
    SystemClock::duration(K_TICK_MAX - 1);
#else
inline constexpr SystemClock::duration kMaxTimeout =
    SystemClock::duration(K_TICK_MAX);
#endif

}  // namespace pw::chrono::zephyr
