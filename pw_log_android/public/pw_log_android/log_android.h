// Copyright 2022 The Pigweed Authors
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

#include <android/log.h>

#include "pw_log/levels.h"

// This backend supports PW_LOG_MODULE_NAME as a fallback for Android logging's
// LOG_TAG if and only if LOG_TAG is not already set. We cannot directly set
// LOG_TAG here because it may be defined after this header is included. We
// use PW_LOG_TAG here instead.
#if defined(LOG_TAG)
#define PW_LOG_TAG LOG_TAG
#elif defined(PW_LOG_MODULE_NAME)
#define PW_LOG_TAG PW_LOG_MODULE_NAME
#else
#error \
    "Cannot set PW_LOG_TAG because LOG_TAG and PW_LOG_MODULE_NAME are not defined."
#endif  // defined(LOG_TAG)

// Converts a PW_LOG_LEVEL_* to an android_LogPriority value.
static inline int _pw_log_android_convert_level(int pw_log_level) {
  switch (pw_log_level) {
    case PW_LOG_LEVEL_DEBUG:
      return ANDROID_LOG_DEBUG;
    case PW_LOG_LEVEL_INFO:
      return ANDROID_LOG_INFO;
    case PW_LOG_LEVEL_WARN:
      return ANDROID_LOG_WARN;
    case PW_LOG_LEVEL_ERROR:
    case PW_LOG_LEVEL_CRITICAL:
      return ANDROID_LOG_ERROR;
    case PW_LOG_LEVEL_FATAL:
      return ANDROID_LOG_FATAL;
    default:
      // This should not happen, since we cover all current PW_LOG_LEVEL_*
      // values above.  Map any unknown levels to WARN to help ensure
      // visibility, just in case.
      return ANDROID_LOG_WARN;
  }
}

#define PW_HANDLE_LOG(level, module, flags, ...)                            \
  do {                                                                      \
    const int _pw_log_level = (level);                                      \
    if (_pw_log_level == PW_LOG_LEVEL_FATAL) {                              \
      /* __android_log_assert() will:                                       \
       * 1. Write to the main log buffer at ANDROID_LOG_FATAL level.        \
       * 2. Write to stderr.                                                \
       * 3. Call abort(). It is marked noreturn.                            \
       */                                                                   \
      __android_log_assert(/*cond=*/NULL, /*tag=*/PW_LOG_TAG, __VA_ARGS__); \
    } else {                                                                \
      __android_log_print(                                                  \
          /*prio=*/_pw_log_android_convert_level(_pw_log_level),            \
          /*tag=*/PW_LOG_TAG,                                               \
          __VA_ARGS__);                                                     \
    }                                                                       \
  } while (0)
