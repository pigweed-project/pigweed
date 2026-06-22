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

// This helper file provides concrete C-linkage wrappers around FreeRTOS API
// macros and inline functions. Because Rust FFI cannot directly interface with
// C preprocessor macros or inline functions, these wrapper functions expose
// them as standard C symbols that the Rust backend can link against.

#include "FreeRTOS.h"
#include "pw_interrupt/context.h"
#include "pw_sync_freertos/config.h"
#include "task.h"

extern "C" {

bool pw_sync_spinlock_freertos_UseSchedulerLock(void) {
  return PW_SYNC_FREERTOS_INTERRUPT_SPIN_LOCK_USES_SCHEDULER_LOCK;
}

uint32_t pw_sync_spinlock_freertos_EnterCriticalFromISR(void) {
  return taskENTER_CRITICAL_FROM_ISR();
}

void pw_sync_spinlock_freertos_ExitCriticalFromISR(uint32_t mask) {
  taskEXIT_CRITICAL_FROM_ISR(mask);
}

void pw_sync_spinlock_freertos_EnterCritical(void) { taskENTER_CRITICAL(); }

void pw_sync_spinlock_freertos_ExitCritical(void) { taskEXIT_CRITICAL(); }

bool pw_sync_spinlock_freertos_InInterruptContext(void) {
  return pw::interrupt::InInterruptContext();
}

void pw_sync_spinlock_freertos_SuspendAll(void) { vTaskSuspendAll(); }

void pw_sync_spinlock_freertos_ResumeAll(void) {
  static_cast<void>(xTaskResumeAll());
}

int32_t pw_sync_spinlock_freertos_GetSchedulerState(void) {
  return xTaskGetSchedulerState();
}
}
