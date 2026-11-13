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

#include <zephyr/logging/log.h>

#ifndef APP_CUSTOM_LOG_HEADER_INCLUDED
#error "The application's zephyr_custom_log.h was not included"
#endif  // APP_CUSTOM_LOG_HEADER_INCLUDED

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

int main(void) {
  LOG_INF("Hello from the application");
  return 0;
}
