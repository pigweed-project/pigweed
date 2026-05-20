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

#include <stdint.h>

#include "pw_log_tokenized/config.h"
#include "pw_preprocessor/util.h"
#include "pw_tokenizer/tokenize.h"

// TODO(hepler): Remove these includes.
#ifdef __cplusplus
#include "pw_log_tokenized/metadata.h"
#endif  // __cplusplus

/// @module{pw_log_tokenized}

/// This macro implements `PW_LOG` using `pw_tokenizer`. Users must implement
/// `pw_log_tokenized_HandleLog(uint32_t metadata, const uint8_t* buffer, size_t
/// size)`. The log level, module token, and flags are packed into the metadata
/// argument.
///
/// Two strings are tokenized in this macro:
///
///   - The log format string, tokenized in the default tokenizer domain.
///   - Log module name, masked to 16 bits and tokenized in the
///     "pw_log_module_names" tokenizer domain.
///
/// To use this macro, implement `pw_log_tokenized_HandleLog()`, which is
/// defined in `pw_log_tokenized/handler.h`. The log metadata can be accessed
/// using `pw::log_tokenized::Metadata`. For example:
///
/// @code{.cpp}
///   extern "C" void pw_log_tokenized_HandleLog(
///       uint32_t metadata, const uint8_t data[], size_t size) {
///     pw::log_tokenized::Metadata metadata(metadata);
///
///     if (metadata.level() >= kLogLevel && ModuleEnabled(metadata.module())) {
///       EmitLogMessage(data, size, metadata.flags());
///     }
///   }
/// @endcode
#define PW_LOG_TOKENIZED_TO_GLOBAL_HANDLER_WITH_METADATA(                    \
    level, module, flags, message, ...)                                      \
  do {                                                                       \
    _PW_TOKENIZER_CONST uintptr_t _pw_log_tokenized_module_token =           \
        PW_TOKENIZE_STRING_MASK("pw_log_module_names",                       \
                                ((1u << PW_LOG_TOKENIZED_MODULE_BITS) - 1u), \
                                module);                                     \
    const uintptr_t _pw_log_tokenized_level = level;                         \
    PW_LOG_TOKENIZED_ENCODE_MESSAGE(                                         \
        (_PW_LOG_TOKENIZED_LEVEL(_pw_log_tokenized_level) |                  \
         _PW_LOG_TOKENIZED_MODULE(_pw_log_tokenized_module_token) |          \
         _PW_LOG_TOKENIZED_FLAGS(flags) | _PW_LOG_TOKENIZED_LINE(__LINE__)), \
        PW_LOG_TOKENIZED_FORMAT_STRING(module, message),                     \
        __VA_ARGS__);                                                        \
  } while (0)

/// Legacy alias for `PW_LOG_TOKENIZED_TO_GLOBAL_HANDLER_WITH_METADATA`.
#define PW_LOG_TOKENIZED_TO_GLOBAL_HANDLER_WITH_PAYLOAD \
  PW_LOG_TOKENIZED_TO_GLOBAL_HANDLER_WITH_METADATA

/// Encodes a log message and metadata into the tokenized format.
///
/// This macro tokenizes the format string and calls the backend handler with
/// packed metadata.
#define PW_LOG_TOKENIZED_ENCODE_MESSAGE(metadata, format, ...)               \
  do {                                                                       \
    PW_TOKENIZE_FORMAT_STRING(                                               \
        PW_TOKENIZER_DEFAULT_DOMAIN, UINT32_MAX, format, __VA_ARGS__);       \
    _pw_log_tokenized_EncodeTokenizedLog(metadata,                           \
                                         _pw_tokenizer_token,                \
                                         PW_TOKENIZER_ARG_TYPES(__VA_ARGS__) \
                                             PW_COMMA_ARGS(__VA_ARGS__));    \
  } while (0)

/// @endmodule

/// @cond
PW_EXTERN_C_START

void _pw_log_tokenized_EncodeTokenizedLog(uint32_t metadata,
                                          pw_tokenizer_Token token,
                                          pw_tokenizer_ArgTypes types,
                                          ...);

PW_EXTERN_C_END
/// @endcond
