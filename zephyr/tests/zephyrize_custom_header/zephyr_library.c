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

// Compiled as a Zephyr library (zephyr_library() in CMakeLists.txt), as
// Zephyr's own subsystems are, rather than as part of the application. The
// check is at compile time, so nothing calls this file's function.
#include <zephyr/logging/log.h>

#ifndef APP_CUSTOM_LOG_HEADER_INCLUDED
#error "A Zephyr library didn't include the application's zephyr_custom_log.h"
#endif  // APP_CUSTOM_LOG_HEADER_INCLUDED

LOG_MODULE_REGISTER(zephyr_library, LOG_LEVEL_INF);

void zephyr_library_log(void) { LOG_INF("Hello from a Zephyr library"); }
