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

#include <cerrno>
#include <cstring>  // strerror()

#include "pw_log/log.h"

// TODO(b/328262654): Move this to a more appropriate module.

// A format string for logging an errno plus its string representation.
// Use with the provided ERRNO_FORMAT_ARGS function in logging functions.
// This can be used directly with PW_LOG_*, PW_CHECK_*, and PW_CRASH.
//
// For example:
//
//   PW_CHECK_INT_EQ(size, expected, "Error reading foo: " ERRNO_FORMAT_STRING,
//                   ERRNO_FORMAT_ARGS(errno));
//
// Prefer to use one of the helper macros below for common use cases.
//
#define ERRNO_FORMAT_STRING "errno=%d (%s)"
#define ERRNO_FORMAT_ARGS(e) e, std::strerror(e)

// Macros used to log errno plus its string representation. This ensures
// consistent formatting and always remembering to include the string.
//
// Note that we accept the extra memory cost of logging the string directly
// instead of attempting to tokenize it. These logs should only be used in
// Linux platform-specific code and generally only for logging errors.
// This could be updated to use nested tokenization:
// https://pigweed.dev/seed/0105-pw_tokenizer-pw_log-nested-tokens.html
//
// Example:
//
//   LOG_ERROR_WITH_ERRNO("Error calling foo:", errno);
//
#define LOG_DEBUG_WITH_ERRNO(message, e, ...) \
  PW_LOG_DEBUG(                               \
      message " " ERRNO_FORMAT_STRING, ##__VA_ARGS__, ERRNO_FORMAT_ARGS(e));

#define LOG_INFO_WITH_ERRNO(message, e, ...) \
  PW_LOG_INFO(                               \
      message " " ERRNO_FORMAT_STRING, ##__VA_ARGS__, ERRNO_FORMAT_ARGS(e));

#define LOG_WARN_WITH_ERRNO(message, e, ...) \
  PW_LOG_WARN(                               \
      message " " ERRNO_FORMAT_STRING, ##__VA_ARGS__, ERRNO_FORMAT_ARGS(e));

#define LOG_ERROR_WITH_ERRNO(message, e, ...) \
  PW_LOG_ERROR(                               \
      message " " ERRNO_FORMAT_STRING, ##__VA_ARGS__, ERRNO_FORMAT_ARGS(e));

#define LOG_CRITICAL_WITH_ERRNO(message, e, ...) \
  PW_LOG_CRITICAL(                               \
      message " " ERRNO_FORMAT_STRING, ##__VA_ARGS__, ERRNO_FORMAT_ARGS(e));
