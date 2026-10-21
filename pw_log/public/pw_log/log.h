// Copyright 2020 The Pigweed Authors
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

// This file describes Pigweed's public user-facing logging API.
//
// THIS PUBLIC API IS NOT STABLE OR COMPLETE!
//
// Key functionality is still missing:
//
// - API for controlling verbosity at run time
// - API for querying if logging is enabled for the given level or flags
//
#pragma once

#include "pw_log/config.h"   // IWYU pragma: export
#include "pw_log/levels.h"   // IWYU pragma: export
#include "pw_log/options.h"  // IWYU pragma: export

// log_backend.h must ultimately resolve to a header that implements the macros
// required by the logging facade, as described below.
//
// Inputs: Macros the downstream user provides to control the logging system:
//
//   PW_LOG_MODULE_NAME
//     - The module name the backend should use
//
//   PW_LOG_LEVEL
//     - General log level setting. By default, logs below this level are
//       excluded from the build.
//
// Outputs: Macros log_backend.h is expected to provide:
//
//   PW_LOG(level, verbosity, module, flags, fmt, ...)
//     - Required.
//       Level - An integer level as defined by pw_log/levels.h for this log.
//       Verbosity - An integer level as defined by pw_log/levels.h which is the
//                   minimum level which is enabled.
//       Module - A string literal for the module name.
//       Flags - Arbitrary flags the backend can leverage; user-defined.
//               Example: HAS_PII - A log has personally-identifying data
//               Example: HAS_DII - A log has device-identifying data
//               Example: RELIABLE_DELIVERY - Ask backend to ensure the
//               log is delivered; this may entail blocking other logs.
//               Example: BEST_EFFORT - Don't deliver this log if it
//               would mean blocking or dropping important-flagged logs
//
//   PW_LOG_DEBUG(fmt, ...)
//   PW_LOG_INFO(fmt, ...)
//   PW_LOG_WARN(fmt, ...)
//   PW_LOG_ERROR(fmt, ...)
//   PW_LOG_CRITICAL(fmt, ...)
//     - Optional. If not defined by the backend, the facade's default
//       implementation defines these in terms of PW_LOG().
//
#include "pw_log_backend/log_backend.h"  // IWYU pragma: export

/// @module{pw_log}

/// Accepts the format string and its arguments in a variadic macro. The format
/// string is not listed as a separate argument to avoid adding a comma after
/// the format string when it has no arguments.
#define PW_LOG(                                                            \
    level, verbosity, module, flags, /* format string and arguments */...) \
  do {                                                                     \
    if (PW_LOG_ENABLE_IF(level, verbosity, module, flags)) {               \
      PW_HANDLE_LOG(level, module, flags, __VA_ARGS__);                    \
    }                                                                      \
  } while (0)

/// @}

// For backends that elect to only provide the general PW_LOG() macro and not
// specialized versions, define the standard PW_LOG_<level>() macros in terms
// of the general PW_LOG().
#ifndef PW_LOG_DEBUG
#define PW_LOG_DEBUG(...)    \
  PW_LOG(PW_LOG_LEVEL_DEBUG, \
         PW_LOG_LEVEL,       \
         PW_LOG_MODULE_NAME, \
         PW_LOG_FLAGS,       \
         __VA_ARGS__)
#endif  // PW_LOG_DEBUG

#ifndef PW_LOG_INFO
#define PW_LOG_INFO(...)     \
  PW_LOG(PW_LOG_LEVEL_INFO,  \
         PW_LOG_LEVEL,       \
         PW_LOG_MODULE_NAME, \
         PW_LOG_FLAGS,       \
         __VA_ARGS__)
#endif  // PW_LOG_INFO

#ifndef PW_LOG_WARN
#define PW_LOG_WARN(...)     \
  PW_LOG(PW_LOG_LEVEL_WARN,  \
         PW_LOG_LEVEL,       \
         PW_LOG_MODULE_NAME, \
         PW_LOG_FLAGS,       \
         __VA_ARGS__)
#endif  // PW_LOG_WARN

#ifndef PW_LOG_ERROR
#define PW_LOG_ERROR(...)    \
  PW_LOG(PW_LOG_LEVEL_ERROR, \
         PW_LOG_LEVEL,       \
         PW_LOG_MODULE_NAME, \
         PW_LOG_FLAGS,       \
         __VA_ARGS__)
#endif  // PW_LOG_ERROR

#ifndef PW_LOG_CRITICAL
#define PW_LOG_CRITICAL(...)    \
  PW_LOG(PW_LOG_LEVEL_CRITICAL, \
         PW_LOG_LEVEL,          \
         PW_LOG_MODULE_NAME,    \
         PW_LOG_FLAGS,          \
         __VA_ARGS__)
#endif  // PW_LOG_CRITICAL

#ifndef PW_LOG_EVERY_N
#define PW_LOG_EVERY_N(level, rate, ...)                                       \
  do {                                                                         \
    static uint32_t _pw_log_suppressor##__LINE__ = 0;                          \
    if (_pw_log_suppressor##__LINE__ == 0) {                                   \
      PW_LOG(                                                                  \
          level, PW_LOG_LEVEL, PW_LOG_MODULE_NAME, PW_LOG_FLAGS, __VA_ARGS__); \
      _pw_log_suppressor##__LINE__ = rate;                                     \
    } else {                                                                   \
      _pw_log_suppressor##__LINE__--;                                          \
    }                                                                          \
  } while (0)
#endif  // PW_LOG_EVERY_N

/// @module{pw_log}

/// Default: Number of bits available for the log flags
///
/// All log statements have a flags field, and this define is the number of bits
/// available for the flags. Some backends restrict this for better efficiency.
/// By default, pick a restricted but large enough value to work for most cases.
#ifndef PW_LOG_FLAG_BITS
#define PW_LOG_FLAG_BITS 2
#endif  // PW_LOG_FLAG_BITS

/// @}
