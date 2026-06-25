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

#include <zephyr/kernel.h>

#include <cstdint>

#include "pw_log/log.h"

extern "C" void pw_boot_rust_entry(void*);

// A temporary wrapper to allow a re-usable application function.
// Remove once we have the OSAL.
extern "C" void vTaskDelay(uint32_t ticks) {
  // loop_example uses FreeRTOS ticks. For Pico ports, configTICK_RATE_HZ is
  // typically 1000, which means 1 tick = 1 ms.
  k_msleep(ticks);
}

int main() {
  PW_LOG_INFO("Pigweed RP2350 Target Board booting Zephyr");
  pw_boot_rust_entry(nullptr);
  return 0;
}
