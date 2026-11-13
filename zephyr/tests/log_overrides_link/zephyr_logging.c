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

// Zephyr-style C code that logs with Zephyr's log macros. It doesn't depend on
// Pigweed directly; CONFIG_PIGWEED_LOG_STRING routes the macros to Pigweed.

#include <stdint.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_zephyr_logging, LOG_LEVEL_INF);

void app_zephyr_logging_run(void) {
  static const uint8_t kData[] = {0x01, 0x23, 0x45, 0x67};
  LOG_INF("Logging from Zephyr code");
  LOG_HEXDUMP_INF(kData, sizeof(kData), "Hex dump from Zephyr code");
}
