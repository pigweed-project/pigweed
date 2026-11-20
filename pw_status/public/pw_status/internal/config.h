// Copyright 2021 The Pigweed Authors
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

/// @module{pw_status}

/// Controls whether to check if `pw::Status` values are used. Unused status
/// values cause compilation warnings or errors. Calling the nop
/// `pw::status::IgnoreError()` function silences these warnings.
#ifndef PW_STATUS_CFG_CHECK_IF_USED
#define PW_STATUS_CFG_CHECK_IF_USED 0
#endif  // PW_STATUS_CFG_CHECK_IF_USED

/// `PW_STATUS_CFG_MESSAGES` value: statuses do not store messages.
/// `PW_STATUS_MSG` arguments are discarded at compile time and statuses are
/// exactly as large as their code.
#define PW_STATUS_CFG_MESSAGES_NONE 0

/// `PW_STATUS_CFG_MESSAGES` value: statuses store a pointer to the message's
/// string literal. Use with `pw_log` backends that do not tokenize logs.
#define PW_STATUS_CFG_MESSAGES_STRING 1

/// `PW_STATUS_CFG_MESSAGES` value: statuses store the message's `pw_tokenizer`
/// token. Use with `pw_log` backends that tokenize logs, such as
/// `pw_log_tokenized`.
#define PW_STATUS_CFG_MESSAGES_TOKENIZED 2

/// Selects how `pw::Status` and other `pw::StatusBase`-derived statuses store
/// optional error messages, which are created with `PW_STATUS_MSG("...")` and
/// logged with `PW_LOG_STATUS`. Set to `PW_STATUS_CFG_MESSAGES_NONE` (the
/// default), `PW_STATUS_CFG_MESSAGES_STRING`, or
/// `PW_STATUS_CFG_MESSAGES_TOKENIZED`.
///
/// Messages are stored in the form the `pw_log` backend emits, so this option
/// must match the backend. `pw_log/status.h` fails to compile if it does not.
///
/// In Bazel, set the `@pigweed//pw_status:messages` flag to `none`, `string`,
/// or `tokenized` instead.
///
/// @warning When messages are stored as tokens, status codes must be valid
/// enumerators. See `pw::StatusBase`.
#ifndef PW_STATUS_CFG_MESSAGES
#define PW_STATUS_CFG_MESSAGES PW_STATUS_CFG_MESSAGES_NONE
#endif  // PW_STATUS_CFG_MESSAGES

#if PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_NONE &&   \
    PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_STRING && \
    PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_TOKENIZED
#error \
    "PW_STATUS_CFG_MESSAGES must be PW_STATUS_CFG_MESSAGES_NONE, \
PW_STATUS_CFG_MESSAGES_STRING, or PW_STATUS_CFG_MESSAGES_TOKENIZED"
#endif  // PW_STATUS_CFG_MESSAGES

/// @}

// Set internal macro that optionally adds the [[nodiscard]] attribute.
#if PW_STATUS_CFG_CHECK_IF_USED
#define _PW_STATUS_NO_DISCARD [[nodiscard]]
#else
#define _PW_STATUS_NO_DISCARD
#endif  // PW_STATUS_CFG_CHECK_IF_USED
