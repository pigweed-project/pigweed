// Copyright 2023 The Pigweed Authors
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

#include <zephyr/logging/log_core.h>
#include <zephyr/sys/__assert.h>

// If static_assert wasn't defined by zephyr/sys/__assert.h that means it's not
// supported, just ignore it.
#ifndef static_assert
#define static_assert(...)
#endif

#include <pw_log/log.h>

#undef LOG_DBG
#undef LOG_INF
#undef LOG_WRN
#undef LOG_ERR

#define Z_PW_LOG(_level, fn, format, ...)   \
  do {                                      \
    if (!Z_LOG_CONST_LEVEL_CHECK(_level)) { \
      break;                                \
    }                                       \
    fn(format, ##__VA_ARGS__);              \
  } while (false)

#define LOG_DBG(format, ...) \
  Z_PW_LOG(LOG_LEVEL_DBG, PW_LOG_DEBUG, format, ##__VA_ARGS__)
#define LOG_INF(format, ...) \
  Z_PW_LOG(LOG_LEVEL_INF, PW_LOG_INFO, format, ##__VA_ARGS__)
#define LOG_WRN(format, ...) \
  Z_PW_LOG(LOG_LEVEL_WRN, PW_LOG_WARN, format, ##__VA_ARGS__)
#define LOG_ERR(format, ...) \
  Z_PW_LOG(LOG_LEVEL_ERR, PW_LOG_ERROR, format, ##__VA_ARGS__)

#undef LOG_HEXDUMP_DBG
#undef LOG_HEXDUMP_INF
#undef LOG_HEXDUMP_WRN
#undef LOG_HEXDUMP_ERR

#include "pw_hex_dump/log_bytes.h"

#ifdef __cplusplus
#include "pw_span/span.h"

#define Z_PW_LOG_HEXDUMP(                                            \
    _level, pw_level_val, header_fn, data, length, header)           \
  do {                                                               \
    if (!Z_LOG_CONST_LEVEL_CHECK(_level)) {                          \
      break;                                                         \
    }                                                                \
    header_fn(header);                                               \
    pw::dump::LogBytes(                                              \
        pw_level_val,                                                \
        pw::span(reinterpret_cast<const std::byte*>(data), length)); \
  } while (false)
#else
#define Z_PW_LOG_HEXDUMP(                                  \
    _level, pw_level_val, header_fn, data, length, header) \
  do {                                                     \
    if (!Z_LOG_CONST_LEVEL_CHECK(_level)) {                \
      break;                                               \
    }                                                      \
    header_fn(header);                                     \
    pw_dump_LogBytes(pw_level_val, data, length);          \
  } while (false)
#endif  // __cplusplus

// Re-define Zephyr hex dump macros to route through Pigweed's log framework.
#define LOG_HEXDUMP_DBG(data, length, str) \
  Z_PW_LOG_HEXDUMP(                        \
      LOG_LEVEL_DBG, PW_LOG_LEVEL_DEBUG, PW_LOG_DEBUG, data, length, str)
#define LOG_HEXDUMP_INF(data, length, str) \
  Z_PW_LOG_HEXDUMP(                        \
      LOG_LEVEL_INF, PW_LOG_LEVEL_INFO, PW_LOG_INFO, data, length, str)
#define LOG_HEXDUMP_WRN(data, length, str) \
  Z_PW_LOG_HEXDUMP(                        \
      LOG_LEVEL_WRN, PW_LOG_LEVEL_WARN, PW_LOG_WARN, data, length, str)
#define LOG_HEXDUMP_ERR(data, length, str) \
  Z_PW_LOG_HEXDUMP(                        \
      LOG_LEVEL_ERR, PW_LOG_LEVEL_ERROR, PW_LOG_ERROR, data, length, str)
