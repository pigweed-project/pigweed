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
#pragma once

#include <stdint.h>

#include "pw_log_tokenized/config.h"
#include "pw_preprocessor/compiler.h"
#include "pw_preprocessor/util.h"
#include "pw_tokenizer/tokenize.h"

/// @module{pw_log_tokenized}

/// This macro implements `PW_LOG` using `pw_tokenizer` without any metadata.
/// Users must implement `pw_log_tokenized_HandleLogWithoutMetadata(const
/// uint8_t* buffer, size_t size)`.
#define PW_LOG_TOKENIZED_TO_GLOBAL_HANDLER(level, module, flags, message, ...) \
  do {                                                                         \
    (void)level;                                                               \
    (void)flags;                                                               \
    PW_LOG_TOKENIZED_ENCODE_MESSAGE_LIGHT(                                     \
        PW_LOG_TOKENIZED_FORMAT_STRING(module, message), __VA_ARGS__);         \
  } while (0)

/// Encodes a log message into the tokenized format without metadata.
///
/// This macro tokenizes the format string and calls the backend handler
/// `_pw_log_tokenized_EncodeTokenizedLogWithoutMetadata`.
#define PW_LOG_TOKENIZED_ENCODE_MESSAGE_LIGHT(format, ...)               \
  do {                                                                   \
    PW_TOKENIZE_FORMAT_STRING(                                           \
        PW_TOKENIZER_DEFAULT_DOMAIN, UINT32_MAX, format, __VA_ARGS__);   \
    _pw_log_tokenized_EncodeTokenizedLogWithoutMetadata(                 \
        _pw_tokenizer_token,                                             \
        PW_TOKENIZER_ARG_TYPES(__VA_ARGS__) PW_COMMA_ARGS(__VA_ARGS__)); \
  } while (0)

/// @endmodule

/// @cond
PW_EXTERN_C_START

void _pw_log_tokenized_EncodeTokenizedLogWithoutMetadata(
    pw_tokenizer_Token token, pw_tokenizer_ArgTypes types, ...);

PW_EXTERN_C_END
/// @endcond
