// Copyright 2024 The Pigweed Authors
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

#include <zephyr/sys/__assert.h>

#include "pw_log/log.h"
#include "pw_polyfill/static_assert.h"

#undef shell_fprintf
#undef shell_info
#undef shell_print
#undef shell_warn
#undef shell_error

#define shell_fprintf(sh, color, fmt, ...) \
  do {                                     \
    (void)(sh);                            \
    (void)(color);                         \
    PW_LOG_INFO(fmt, ##__VA_ARGS__);       \
  } while (0)

#define shell_info(_sh, _ft, ...)    \
  do {                               \
    (void)(_sh);                     \
    PW_LOG_INFO(_ft, ##__VA_ARGS__); \
  } while (0)

#define shell_print(_sh, _ft, ...)   \
  do {                               \
    (void)(_sh);                     \
    PW_LOG_INFO(_ft, ##__VA_ARGS__); \
  } while (0)

#define shell_warn(_sh, _ft, ...)    \
  do {                               \
    (void)(_sh);                     \
    PW_LOG_WARN(_ft, ##__VA_ARGS__); \
  } while (0)

#define shell_error(_sh, _ft, ...)    \
  do {                                \
    (void)(_sh);                      \
    PW_LOG_ERROR(_ft, ##__VA_ARGS__); \
  } while (0)
