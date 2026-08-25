# Copyright 2021 The Pigweed Authors
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

pw_add_library(pw_log_zephyr STATIC
  HEADERS
    public/pw_log_zephyr/log_zephyr.h
    public/pw_log_zephyr/config.h
    public_overrides/pw_log_backend/log_backend.h
  PUBLIC_INCLUDES
    public
    public_overrides
  PUBLIC_DEPS
    pw_log.facade
  SOURCES
    log_zephyr.cc
  PRIVATE_DEPS
    pw_preprocessor
)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_LOG_ZEPHYR
    pw_log.facade
    pw_log_zephyr
)

pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_LOG_STRING
    pw_log.facade
    pw_log_string
)

pw_add_library(pw_log_zephyr.pigweed_overrides INTERFACE
  HEADERS
    zephyr_public_overrides/zephyr_custom_log.h
    zephyr_public_overrides/zephyr_custom_shell.h
  PUBLIC_INCLUDES
    zephyr_public_overrides
  PUBLIC_DEPS
    pw_log.facade
    pw_hex_dump.log_bytes
)

pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_LOG_PIGWEED_OVERRIDES
    pw_log_zephyr.pigweed_overrides
)

pw_add_library(pw_log_zephyr.tokenized_lib INTERFACE
  PUBLIC_DEPS
    pw_log_tokenized
    pw_log_tokenized.base64
    pw_log_zephyr.pigweed_overrides
    pw_tokenizer
)

pw_add_library(pw_log_zephyr.tokenized_handler STATIC
  SOURCES
    pw_log_zephyr_tokenized_handler.cc
  PUBLIC_DEPS
    pw_log_tokenized.handler
    pw_log_zephyr.tokenized_lib
)

pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED
    pw_log.facade
    pw_log_tokenized
    pw_log_zephyr.pigweed_overrides
    pw_log_zephyr.tokenized_handler
)
pw_add_library(pw_log_zephyr.tokenized_rpc_handler INTERFACE
  PUBLIC_DEPS
    pw_checksum
    pw_sys_io
    pw_system.init
    pw_log.facade
    pw_log_zephyr.pigweed_overrides
    pw_metric.global
    pw_system.log_backend
)

pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_LOG_TOKENIZED_RPC
    pw_log.facade
    pw_log_tokenized
    pw_log_zephyr.pigweed_overrides
    pw_log_zephyr.tokenized_rpc_handler
)

# Map tokenized configuration options
pw_set_config_from_zephyr(CONFIG_PIGWEED_LOG_TOKENIZED_LEVEL_BITS PW_LOG_TOKENIZED_LEVEL_BITS)
pw_set_config_from_zephyr(CONFIG_PIGWEED_LOG_TOKENIZED_LINE_BITS PW_LOG_TOKENIZED_LINE_BITS)
pw_set_config_from_zephyr(CONFIG_PIGWEED_LOG_TOKENIZED_FLAG_BITS PW_LOG_TOKENIZED_FLAG_BITS)
pw_set_config_from_zephyr(CONFIG_PIGWEED_LOG_TOKENIZED_MODULE_BITS PW_LOG_TOKENIZED_MODULE_BITS)
