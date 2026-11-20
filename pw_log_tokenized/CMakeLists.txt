# Copyright 2020 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
include($ENV{PW_ROOT}/pw_enum/pw_cc_enum_library.cmake)
include($ENV{PW_ROOT}/pw_log_tokenized/backend.cmake)

pw_add_module_config(pw_log_tokenized_CONFIG)

pw_add_library(pw_log_tokenized.config INTERFACE
  HEADERS
    public/pw_log_tokenized/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log.facade
    pw_polyfill
    pw_tokenizer.config
    ${pw_log_tokenized_CONFIG}
)

pw_add_library(pw_log_tokenized.tokenized_backend_indicator INTERFACE
  HEADERS
    public_overrides/pw_log_backend/log_backend_uses_pw_tokenizer.h
  PUBLIC_INCLUDES
    public_overrides
)

pw_add_library(pw_log_tokenized STATIC
  HEADERS
    metadata_public_overrides/pw_log_backend/log_backend.h
  PUBLIC_INCLUDES
    metadata_public_overrides
  PUBLIC_DEPS
    pw_log_tokenized.handler
    pw_log_tokenized._headers
    pw_log_tokenized.tokenized_backend_indicator
  SOURCES
    log_tokenized.cc
)

pw_add_library(pw_log_tokenized.light STATIC
  HEADERS
    light_public_overrides/pw_log_backend/log_backend.h
  PUBLIC_INCLUDES
    light_public_overrides
  PUBLIC_DEPS
    pw_log_tokenized.light_handler
    pw_log_tokenized._headers
    pw_log_tokenized.tokenized_backend_indicator
  SOURCES
    log_tokenized_light.cc
)

pw_add_library(pw_log_tokenized._headers INTERFACE
  HEADERS
    public/pw_log_tokenized/log_tokenized.h
    public/pw_log_tokenized/log_tokenized_light.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log_tokenized.config
    pw_log_tokenized.metadata
    pw_preprocessor
    pw_tokenizer
)

pw_add_library(pw_log_tokenized.gcc_partially_tokenized STATIC
  HEADERS
    public/pw_log_tokenized/gcc_partially_tokenized.h
    gcc_partially_tokenized_public_overrides/pw_log_backend/log_backend.h
  PUBLIC_INCLUDES
    public
    gcc_partially_tokenized_public_overrides
  PUBLIC_DEPS
    pw_log_string.handler
    pw_log_tokenized.handler
    pw_log_tokenized._headers
  SOURCES
    log_tokenized.cc
)

pw_add_library(pw_log_tokenized.metadata INTERFACE
  HEADERS
    public/pw_log_tokenized/metadata.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log_tokenized.config
)

pw_add_library(pw_log_tokenized.fields INTERFACE
  HEADERS
    public/pw_log_tokenized/fields.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log_tokenized.config
    pw_status
)

pw_add_facade(pw_log_tokenized.handler INTERFACE
  BACKEND
    pw_log_tokenized.handler_BACKEND
  HEADERS
    public/pw_log_tokenized/handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_preprocessor
)

pw_add_facade(pw_log_tokenized.light_handler INTERFACE
  BACKEND
    pw_log_tokenized.light_handler_BACKEND
  HEADERS
    public/pw_log_tokenized/light_handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_preprocessor
)

pw_add_library(pw_log_tokenized.base64 INTERFACE
  HEADERS
    public/pw_log_tokenized/base64.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log_tokenized.config
    pw_tokenizer.base64
)

# This target provides a backend for pw_tokenizer that encodes tokenized logs as
# Base64, encodes them into HDLC frames, and writes them over sys_io.
pw_add_library(pw_log_tokenized.base64_over_hdlc STATIC
  SOURCES
    base64_over_hdlc.cc
  PRIVATE_DEPS
    pw_hdlc.encoder
    pw_log_tokenized.base64
    pw_log_tokenized.handler
    pw_log_tokenized.light_handler
    pw_span
    pw_stream.sys_io_stream
)

pw_add_test(pw_log_tokenized.log_tokenized_test
  SOURCES
    log_tokenized_test.cc
    log_tokenized_test_c.c
    pw_log_tokenized_private/test_utils.h
  PRIVATE_INCLUDES
    .
  PRIVATE_DEPS
    pw_log_tokenized.base64
    pw_log_tokenized._headers
    pw_preprocessor
  GROUPS
    modules
    pw_log_tokenized
)

pw_add_test(pw_log_tokenized.metadata_test
  SOURCES
    metadata_test.cc
  PRIVATE_DEPS
    pw_log_tokenized.metadata
  GROUPS
    modules
    pw_log_tokenized
)

pw_add_test(pw_log_tokenized.fields_test
  SOURCES
    fields_test.cc
  PRIVATE_DEPS
    pw_log_tokenized.fields
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_log_tokenized
)

pw_add_enum_library(pw_log_tokenized.tokenized_args_test_enum
  ENUM_HEADERS
    test_enums/tokenized_args_test_enum.h
  PUBLIC_INCLUDES
    test_enums
)

pw_add_test(pw_log_tokenized.tokenized_args_test
  SOURCES
    tokenized_args_test.cc
  PRIVATE_DEPS
    pw_log.args
    pw_log.status
    pw_log_tokenized.tokenized_args_test_enum
    pw_log_tokenized.tokenized_backend_indicator
    pw_result
    pw_span
    pw_status
    pw_status.message
    pw_tokenizer
    pw_tokenizer.decoder
    pw_unit_test.constexpr
  GROUPS
    modules
    pw_log_tokenized
)
