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

#include "pw_hal/uart/sync_uart.h"

#include <mutex>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_status/status_with_size.h"

namespace pw::hal::uart {

SyncBufferedUart::SyncBufferedUart(std::unique_ptr<BufferedUartDriver> driver)
    : uart_driver_(std::move(driver)) {
  if (uart_driver_ != nullptr) {
    PW_ASSERT_OK(uart_driver_->Enable());
    PW_ASSERT_OK(uart_driver_->SetTxReadyNotifyLevel(1));
    PW_ASSERT_OK(uart_driver_->SetRxReadyNotifyLevel(1));
    PW_ASSERT_OK(uart_driver_->SetTxReadyNotifier(
        [this](size_t bytes_available) { TxNotifyCallback(bytes_available); }));
    PW_ASSERT_OK(uart_driver_->SetRxReadyNotifier(
        [this](size_t bytes_available) { RxNotifyCallback(bytes_available); }));
  }
}

SyncBufferedUart::~SyncBufferedUart() {
  if (uart_driver_ != nullptr) {
    PW_ASSERT_OK(uart_driver_->SetTxReadyNotifier(nullptr));
    PW_ASSERT_OK(uart_driver_->SetRxReadyNotifier(nullptr));
    PW_ASSERT_OK(uart_driver_->Disable());
  }
}

void SyncBufferedUart::TxNotifyCallback(size_t /*bytes_available*/) {
  tx_completion_.release();
}

void SyncBufferedUart::RxNotifyCallback(size_t /*bytes_available*/) {
  rx_completion_.release();
}

Status SyncBufferedUart::SendFor(ConstByteSpan tx_buffer,
                                 chrono::SystemClock::duration timeout) {
  return SendUntil(tx_buffer,
                   chrono::SystemClock::TimePointAfterAtLeast(timeout));
}

Status SyncBufferedUart::ReceiveFor(ByteSpan rx_buffer,
                                    chrono::SystemClock::duration timeout) {
  return ReceiveUntil(rx_buffer,
                      chrono::SystemClock::TimePointAfterAtLeast(timeout));
}

Status SyncBufferedUart::SendUntil(ConstByteSpan tx_buffer,
                                   chrono::SystemClock::time_point until) {
  if (chrono::SystemClock::now() >= until) {
    return Status::DeadlineExceeded();
  }
  if (!tx_mutex_.try_lock_until(until)) {
    return Status::DeadlineExceeded();
  }
  std::unique_lock lock(tx_mutex_, std::adopt_lock);

  if (uart_driver_ == nullptr) {
    return Status::FailedPrecondition();
  }

  if (tx_buffer.empty()) {
    return OkStatus();
  }

  // If the previous Send call timed out, the thread_notification could be
  // stale. Without this, we could get a spurious wake-up on the first loop.
  static_cast<void>(tx_completion_.try_acquire());
  ConstByteSpan remaining = tx_buffer;
  while (!remaining.empty()) {
    StatusWithSize result = uart_driver_->Send(remaining);
    if (!result.ok() && result.status() != Status::ResourceExhausted()) {
      return result.status();
    }
    remaining = remaining.subspan(result.size());
    if (remaining.empty()) {
      break;
    }
    if (!tx_completion_.try_acquire_until(until)) {
      return Status::DeadlineExceeded();
    }
  }

  return OkStatus();
}

Status SyncBufferedUart::ReceiveUntil(ByteSpan rx_buffer,
                                      chrono::SystemClock::time_point until) {
  if (chrono::SystemClock::now() >= until) {
    return Status::DeadlineExceeded();
  }
  if (!rx_mutex_.try_lock_until(until)) {
    return Status::DeadlineExceeded();
  }
  std::unique_lock lock(rx_mutex_, std::adopt_lock);

  if (uart_driver_ == nullptr) {
    return Status::FailedPrecondition();
  }

  if (rx_buffer.empty()) {
    return OkStatus();
  }

  // If the previous Receive call timed out, the thread_notification could be
  // stale. Without this, we could get a spurious wake-up on the first loop.
  static_cast<void>(rx_completion_.try_acquire());
  ByteSpan remaining = rx_buffer;
  while (!remaining.empty()) {
    StatusWithSize result = uart_driver_->Receive(remaining);
    if (!result.ok() && result.status() != Status::ResourceExhausted()) {
      return result.status();
    }
    remaining = remaining.subspan(result.size());
    if (remaining.empty()) {
      break;
    }
    if (!rx_completion_.try_acquire_until(until)) {
      return Status::DeadlineExceeded();
    }
  }

  return OkStatus();
}

}  // namespace pw::hal::uart
