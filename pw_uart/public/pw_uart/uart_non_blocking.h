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

#include <cstdint>

#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_uart/uart_base.h"

namespace pw::uart {

/// @module{pw_uart}

/// Represents an abstract UART interface.
///
/// The `UartNonBlocking` interface provides a basic set of methods for
/// performing Non-Blocking UART communication.

class UartNonBlocking : public UartBase {
 public:
  ~UartNonBlocking() override = default;

  /// Reads exactly `rx_buffer.size()` bytes from the UART into the provided
  /// buffer.
  ///
  /// This function calls `callback` after the entirety of `rx_buffer` is filled
  /// with data. This may be called from interrupt context.
  /// The callback may be called in ISR context.
  /// It is not safe to call any Uart methods from the callback context.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param callback   A callback to invoke when the transaction is completed.
  ///                   @param status  OK: The operation was successful and the
  ///                                  entire buffer has been filled with data.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelRead().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  ///                   @param buffer  buffer.size() returns the number of
  ///                                  bytes successfully read into the buffer.
  ///                                  If `status`=OK, the buffer is identical
  ///                                  to rx_buffer
  ///
  /// @returns
  /// * @OK: The operation was successfully started.
  /// * @UNAVAILABLE: Another `Read()` transaction is currently in progress.
  /// * May return other implementation-specific status codes.
  Status ReadExactly(
      ByteSpan rx_buffer,
      Function<void(Status status, ConstByteSpan buffer)>&& callback) {
    return DoRead(rx_buffer, rx_buffer.size(), std::move(callback));
  }

  /// Reads at least `min_bytes` and at most `rx_buffer.size()` bytes from the
  /// UART into the provided buffer.
  ///
  /// This function calls `callback` after `rx_buffer` is filled with at least
  /// `min_bytes` of data. This may be called from interrupt context.
  /// The callback may be called in ISR context.
  /// It is not safe to call any Uart methods from the callback context.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param min_bytes  Minimum bytes to read.
  /// @param callback   A callback to invoke when the transaction is completed.
  ///                   @param status  OK: The operation was successful and the
  ///                                  buffer has been filled with at least
  ///                                  `min_bytes` with data.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelRead().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  ///                   @param buffer  buffer.size() returns the number of
  ///                                  bytes successfully read into the buffer.
  ///
  /// @returns
  /// * @OK: The operation was successfully started.
  /// * @UNAVAILABLE: Another `Read()` transaction is currently in progress.
  /// * May return other implementation-specific status codes.
  Status ReadAtLeast(
      ByteSpan rx_buffer,
      size_t min_bytes,
      Function<void(Status status, ConstByteSpan buffer)>&& callback) {
    return DoRead(rx_buffer, min_bytes, std::move(callback));
  }

  /// Cancel a current read in progress.
  ///
  /// This function will cancel a read in progress. The read's callback will
  /// be invoked with status=CANCELLED.
  ///
  /// @retval true The operation was successfully canceled a transaction in
  ///     progress and the callback will be invoked with `status=CANCELLED`.
  /// @retval false There were no transactions in progress and nothing was
  ///     cancelled. No callback will be invoked.
  bool CancelRead() { return DoCancelRead(); }

  /// Writes data from a provided buffer to the UART.
  ///
  /// This function calls `callback` after the entirety of `tx_buffer` is
  /// written to the UART. This may be called from interrupt context.
  /// The callback may be called in ISR context.
  /// It is not safe to call any Uart methods from the callback context.
  ///
  /// @param tx_buffer  The buffer to write to the UART.
  /// @param callback   A callback to invoke when the transaction is completed.
  ///                   @param status  status.size() returns the number of bytes
  ///                                  successfully written from `tx_buffer`.
  ///                                  OK: The operation was successful and the
  ///                                  entire buffer has been written.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelWrite().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  ///
  /// @returns
  /// * @OK: The operation was successfully started.
  /// * @UNAVAILABLE: Another Write() transaction is currently in progress.
  /// * May return other implementation-specific status codes.
  Status Write(ConstByteSpan tx_buffer,
               Function<void(StatusWithSize status)>&& callback) {
    return DoWrite(tx_buffer, std::move(callback));
  }

  /// Cancel a current write in progress.
  ///
  /// This function will cancel a write in progress. The write's callback will
  /// be called with status=CANCELLED.
  ///
  /// @retval true The operation was successfully canceled a transaction in
  ///     progress and the callback will be invoked with status=CANCELLED.
  /// @retval false There were no transactions in progress and nothing was
  ///    cancelled. No callback will be invoked.
  bool CancelWrite() { return DoCancelWrite(); }

  /// Ensures all queued data in the UART has been transmitted and the hardware
  /// FIFO is empty.
  ///
  /// This function ensures that all data enqueued before calling this function
  /// has been transmitted. Any data enqueued after this function completes will
  /// be transmitted immediately.
  ///
  /// @param callback   A callback to invoke when the flush is completed.
  ///                   @param status  OK: The operation was successful and the
  ///                                  transmit FIFO is empty.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelFlushOutput().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  ///
  /// @returns
  /// * @OK: The operation was successfully started.
  /// * @UNAVAILABLE: Another `Write()` or `FlushOutput()` operation is
  ///   currently in progress.
  /// * May return other implementation-specific status codes.
  Status FlushOutput(Function<void(Status status)>&& callback) {
    return DoFlushOutput(std::move(callback));
  }

  /// Cancels a pending FlushOutput() operation.
  ///
  /// This function will cancel an output flush in progress. The FlushOutput
  /// callback will be called with status=CANCELLED.
  ///
  /// @returns
  /// * `true`: An in-progress transaction was cancelled and the callback will
  ///   be invoked with status set to @CANCELLED.
  /// * `false`: There were no transactions in progress and nothing was
  ///   cancelled. No callback will be invoked.
  bool CancelFlushOutput() { return DoCancelFlushOutput(); }

 private:
  /// Reads at least `min_bytes` and at most `rx_buffer.size()` bytes from the
  /// UART into the provided buffer.
  ///
  /// This virtual function attempts to read data into the provided byte buffer
  /// (`rx_buffer`).
  /// This virtual function will return immediately. `callback` will be invoked
  /// when the buffer has been filled with at least `min_bytes`, or an error
  /// occurs.
  /// Implementation Notes:
  /// * The callback may be called in ISR context.
  /// * The callback must be moved and stored prior to its invocation.
  /// * Do not hold a lock when invoking the callback.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param min_bytes  Minimum bytes to read.
  /// @param callback   A callback to invoke when the transaction is completed.
  ///                   @param status  OK: The operation was successful and the
  ///                                  buffer has been filled with at least
  ///                                  `min_bytes` with data.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelRead().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  ///                   @param buffer  buffer.size() returns the number of
  ///                                  bytes successfully read into the buffer.
  /// @returns
  /// * @OK: The operation was successfully started.
  /// * @UNAVAILABLE: Another `Read()` transaction is currently in progress.
  /// * May return other implementation-specific status codes.
  virtual Status DoRead(
      ByteSpan rx_buffer,
      size_t min_bytes,
      Function<void(Status status, ConstByteSpan buffer)>&& callback) = 0;

  /// Cancels a current read in progress.
  ///
  /// This virtual function will cancel a read in progress. The read's callback
  /// will be called with status=CANCELLED.
  ///
  /// @returns
  /// * `true`: An in-progress transaction was cancelled and the callback will
  ///   be invoked with status set to @CANCELLED.
  /// * `false`: There were no transactions in progress and nothing was
  ///   cancelled. No callback will be invoked.
  /// * May return other implementation-specific status codes.
  virtual bool DoCancelRead() = 0;

  /// Writes data from a provided buffer to the UART.
  ///
  /// This virtual function attempts to write data from the provided byte
  /// buffer (`tx_buffer`).
  /// This virtual function will return immediately. `callback` will be invoked
  /// when either the buffer fully written, or an error occurs.
  /// Implementation Notes:
  /// * The callback may be called in ISR context.
  /// * The callback must be moved and stored prior to its invocation.
  /// * Do not hold a lock when invoking the callback.
  ///
  /// @param tx_buffer  The buffer to write to the UART.
  /// @param callback   A callback to invoke when the transaction is completed.
  ///                   @param status  status.size() returns the number of bytes
  ///                                  successfully written from `tx_buffer`.
  ///                   @param status  OK: The operation was successful and the
  ///                                  entire buffer has been written.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelWrite().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  /// @returns
  /// * @OK: The operation was successfulyl started.
  /// * @UNAVAILABLE: Another `Write()` transaction is currently in progress.
  /// * May return other implementation-specific status codes.
  virtual Status DoWrite(ConstByteSpan tx_buffer,
                         Function<void(StatusWithSize status)>&& callback) = 0;

  /// Cancel a current write in progress.
  ///
  /// This virtual function will cancel a write in progress. The write's
  /// callback will be invoked with status=CANCELLED.
  ///
  /// @returns
  /// * `true`: An in-progress transaction was cancelled and the callback will
  ///   be invoked with status set to @CANCELLED.
  /// * `false`: There were no transactions in progress and nothing was
  ///   cancelled. No callback will be invoked.
  /// * May return other implementation-specific status codes.
  virtual bool DoCancelWrite() = 0;

  /// Ensures all queued data in the UART has been transmitted and the hardware
  /// FIFO is empty.
  ///
  /// This function ensures that all data enqueued before calling this function
  /// has been transmitted. Any data enqueued after this function completes will
  /// be transmitted immediately.
  ///
  /// @param callback   A callback to invoke when the flush is completed.
  ///                   @param status  OK: The operation was successful and the
  ///                                  transmit FIFO is empty.
  ///                                  CANCELLED: The operation was cancelled
  ///                                  via CancelFlushOutput().
  ///                                  May return other implementation-specific
  ///                                  status codes.
  ///
  /// @returns
  /// * @OK: The operation was successfully started.
  /// * @UNAVAILABLE: Another `Write()` or `FlushOutput()` operation is
  ///   currently in progress.
  /// * May return other implementation-specific status codes.
  virtual Status DoFlushOutput(Function<void(Status status)>&& /*callback*/) {
    return Status::Unimplemented();
  }

  /// Cancels a pending FlushOutput() operation.
  ///
  /// This function will cancel an output flush in progress. The DoFlushOutput
  /// callback will be called with status=CANCELLED.
  ///
  /// @returns
  /// * `true`: An in-progress transaction was cancelled and the callback will
  ///   be invoked with status set to @CANCELLED.
  /// * `false`: There were no transactions in progress and nothing was
  ///   cancelled. No callback will be invoked.
  virtual bool DoCancelFlushOutput() { return false; }
};

}  // namespace pw::uart
