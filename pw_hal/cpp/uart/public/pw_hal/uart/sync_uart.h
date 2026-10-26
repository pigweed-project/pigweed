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

#include <memory>
#include <utility>

#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_hal/uart/uart_driver.h"
#include "pw_status/status.h"
#include "pw_sync/timed_mutex.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::hal::uart {

class SyncBufferedUart {
 public:
  explicit SyncBufferedUart(std::unique_ptr<BufferedUartDriver> driver);
  ~SyncBufferedUart();

  SyncBufferedUart(const SyncBufferedUart&) = delete;
  SyncBufferedUart& operator=(const SyncBufferedUart&) = delete;

  SyncBufferedUart(SyncBufferedUart&&) = delete;
  SyncBufferedUart& operator=(SyncBufferedUart&&) = delete;

  Status SendFor(ConstByteSpan tx_buffer,
                 chrono::SystemClock::duration timeout);
  Status ReceiveFor(ByteSpan rx_buffer, chrono::SystemClock::duration timeout);

  Status SendUntil(ConstByteSpan tx_buffer,
                   chrono::SystemClock::time_point until)
      PW_NO_LOCK_SAFETY_ANALYSIS;
  Status ReceiveUntil(ByteSpan rx_buffer, chrono::SystemClock::time_point until)
      PW_NO_LOCK_SAFETY_ANALYSIS;

 private:
  void TxNotifyCallback(size_t bytes_available);
  void RxNotifyCallback(size_t bytes_available);

  pw::sync::TimedMutex tx_mutex_;
  pw::sync::TimedMutex rx_mutex_;
  pw::sync::TimedThreadNotification tx_completion_;
  pw::sync::TimedThreadNotification rx_completion_;
  std::unique_ptr<BufferedUartDriver> uart_driver_;
};

}  // namespace pw::hal::uart
