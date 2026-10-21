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

// This header is DEPRECATED. Please include "pw_toolchain/busy_wait_forever.h"
// instead.

#include "pw_toolchain/busy_wait_forever.h"  // IWYU pragma: export

#ifdef __cplusplus

namespace pw {

[[noreturn,
  deprecated("Renamed; call pw::BusyWaitForever() from "
             "pw_toolchain/busy_wait_forever.h instead")]] inline void
InfiniteLoop() {
  BusyWaitForever();
}

}  // namespace pw

#else

#include "pw_preprocessor/compiler.h"

PW_NO_RETURN static inline void pw_InfiniteLoop(void) { pw_BusyWaitForever(); }

#endif  // __cplusplus
