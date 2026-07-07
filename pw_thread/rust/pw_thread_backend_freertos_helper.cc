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

#include <cstdint>
#include <type_traits>

#include "FreeRTOS.h"
#include "task.h"

// taskYIELD() is a macro and cannot be called directly from Rust.
// This helper function exposes it as a linkable C function.
extern "C" void pw_thread_freertos_Yield() { taskYIELD(); }

static_assert(std::is_same<TickType_t, uint32_t>::value,
              "FreeRTOS TickType_t must be uint32_t");
