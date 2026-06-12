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
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pw_log/log.h"
#include "pw_preprocessor/compiler.h"
#include "task.h"

extern "C" void pw_boot_rust_entry(void*);

constexpr size_t kEntryStackSize = configMINIMAL_STACK_SIZE;

StackType_t entry_task_stack[kEntryStackSize];
StaticTask_t entry_task_buffer;

int main() {
  stdio_init_all();

  PW_LOG_INFO("Pigweed RP2350 Target Board booting FreeRTOS");

  xTaskCreateStatic(pw_boot_rust_entry,
                    "entry_task",
                    kEntryStackSize,
                    nullptr,
                    1,
                    entry_task_stack,
                    &entry_task_buffer);
  vTaskStartScheduler();
  PW_UNREACHABLE;
}
