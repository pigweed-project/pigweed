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

#include <pw_log/levels.h>
#include <pw_log_tokenized/base64.h>
#include <pw_log_tokenized/config.h>
#include <pw_log_tokenized/handler.h>
#include <pw_log_tokenized/metadata.h>
#include <pw_span/span.h>
#include <pw_sync/interrupt_spin_lock.h>
#include <pw_tokenizer/base64.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_ctrl.h>

namespace pw::log_zephyr {
namespace {

// The Zephyr console may output raw text along with Base64 tokenized messages,
// which could interfere with detokenization. Output a character to mark the end
// of a Base64 message.
constexpr char kEndDelimiter = '#';

sync::InterruptSpinLock log_encode_lock;

}  // namespace

extern "C" void pw_log_tokenized_HandleLog(uint32_t metadata,
                                           const uint8_t log_buffer[],
                                           size_t size_bytes) {
  pw::log_tokenized::Metadata meta(metadata);

  // Encode the tokenized message as Base64.
  const InlineBasicString base64_string =
      log_tokenized::PrefixedBase64Encode(log_buffer, size_bytes);

  if (!base64_string.empty()) {
    // TODO: https://github.com/zephyrproject-rtos/zephyr/issues/59454 - Zephyr
    // frontend should protect messages from getting corrupted from multiple
    // threads.
    log_encode_lock.lock();
    // _is_raw is set to 0 here because the print string is required to be a
    // string literal if _is_raw is set to 1.
    Z_LOG_PRINTK(/*_is_raw=*/0, "%s%c", base64_string.c_str(), kEndDelimiter);
    log_encode_lock.unlock();
  }

  if (meta.level() == PW_LOG_LEVEL_FATAL) {
    LOG_PANIC();
    k_panic();
  }
}

}  // namespace pw::log_zephyr
