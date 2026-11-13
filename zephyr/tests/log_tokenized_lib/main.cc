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

#include <zephyr/sys/printk.h>

#include <cstddef>
#include <cstdint>

#include "pw_log/log.h"

namespace {

size_t handled_log_count = 0;

}  // namespace

// The application's own handler, as CONFIG_PIGWEED_LOG_TOKENIZED_LIB requires.
extern "C" void pw_log_tokenized_HandleLog(uint32_t, const uint8_t[], size_t) {
  handled_log_count += 1;
}

// Zephyr ignores main()'s return value, so the test prints its result for
// testcase.yaml's console harness to match.
int main() {
  const size_t before = handled_log_count;
  PW_LOG_INFO("Tokenized log handled by the application");
  if (handled_log_count == before + 1) {
    printk("log_tokenized_lib: PASS\n");
  } else {
    printk("log_tokenized_lib: FAIL\n");
  }
  return 0;
}
