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

#include "FreeRTOS.h"
#include "task.h"

UBaseType_t freertos_sys_taskENTER_CRITICAL_FROM_ISR(void) {
  return taskENTER_CRITICAL_FROM_ISR();
}

void freertos_sys_taskEXIT_CRITICAL_FROM_ISR(UBaseType_t mask) {
  taskEXIT_CRITICAL_FROM_ISR(mask);
}

void freertos_sys_taskENTER_CRITICAL(void) { taskENTER_CRITICAL(); }

void freertos_sys_taskEXIT_CRITICAL(void) { taskEXIT_CRITICAL(); }

void freertos_sys_taskYIELD(void) { taskYIELD(); }
