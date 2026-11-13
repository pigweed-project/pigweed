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

#include <cstdarg>

extern "C" void app_zephyr_logging_run(void);

// The pw_log_string.handler backend required by CONFIG_PIGWEED_LOG_STRING.
extern "C" void pw_log_string_HandleMessageVaList(
    int, unsigned int, const char*, const char*, int, const char*, va_list) {}

int main() {
  app_zephyr_logging_run();
  return 0;
}
