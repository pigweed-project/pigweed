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

#include "FreeRTOS.h"
#include "task.h"

// Define a constant for configTICK_RATE_HZ. Since it is defined as a macro
// with a cast, bindgen cannot translate it directly.
static const TickType_t __configTICK_RATE_HZ = configTICK_RATE_HZ;
#undef configTICK_RATE_HZ
static const TickType_t configTICK_RATE_HZ = __configTICK_RATE_HZ;

// FreeRTOS implements critical section entry/exit as preprocessor macros
// or inline functions, which cannot be directly translated to Rust FFI by
// bindgen. These wrapper functions expose them as standard C symbols.
UBaseType_t freertos_sys_taskENTER_CRITICAL_FROM_ISR(void);
void freertos_sys_taskEXIT_CRITICAL_FROM_ISR(UBaseType_t mask);
void freertos_sys_taskENTER_CRITICAL(void);
void freertos_sys_taskEXIT_CRITICAL(void);
void freertos_sys_taskYIELD(void);
