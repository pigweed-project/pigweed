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

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_uart/uart_base.h"

/// Core UART interfaces
namespace pw::uart {

/// @module{pw_uart}

/// Represents an abstract UART interface.
///
/// The `Uart` interface provides a basic set of methods for performing
/// blocking UART communication.

class Uart : public UartBase {
 public:
  ~Uart() override = default;

  /// Reads data from the UART into a provided buffer.
  ///
  /// This function blocks until `min_bytes` have been read into `rx_buffer`.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param min_bytes  The minimum number of bytes to read before returning.
  ///
  /// @returns
  /// * @OK: The operation was successful.
  /// * May return other implementation-specific status codes.
  StatusWithSize ReadAtLeast(ByteSpan rx_buffer, size_t min_bytes) {
    return DoTryReadFor(rx_buffer, min_bytes, std::nullopt);
  }

  /// Reads data from the UART into a provided buffer.
  ///
  /// This function blocks until the entire buffer has been filled.
  ///
  /// @param rx_buffer  The buffer to read data into.
  ///
  /// @returns
  /// * @OK: The operation was successful.
  /// * May return other implementation-specific status codes.
  StatusWithSize ReadExactly(ByteSpan rx_buffer) {
    return DoTryReadFor(rx_buffer, rx_buffer.size(), std::nullopt);
  }

  /// Deprecated: Prefer ReadExactly in new code.
  ///
  /// Reads data from the UART into a provided buffer.
  ///
  /// This function blocks until the entire buffer has been filled.
  ///
  /// @param rx_buffer  The buffer to read data into.
  ///
  /// @returns
  /// * @OK: The operation was successful.
  /// * May return other implementation-specific status codes.
  // TODO: https://pwbug.dev/368149122 - Remove after transition
  Status Read(ByteSpan rx_buffer) {
    return DoTryReadFor(rx_buffer, std::nullopt).status();
  }

  /// Reads data from the UART into a provided buffer.
  ///
  /// This function blocks until either `min_bytes` have been read into buffer
  /// or the specified timeout has elapsed, whichever occurs first.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param min_bytes  The minimum number of bytes to read before returning.
  /// @param timeout    The maximum time to wait for data to be read. If zero,
  ///                   the function will immediately return with at least one
  ///                   hardware read operation attempt.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been filled
  ///   with data.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be filled.
  /// * May return other implementation-specific status codes.
  StatusWithSize TryReadAtLeastFor(ByteSpan rx_buffer,
                                   size_t min_bytes,
                                   chrono::SystemClock::duration timeout) {
    return DoTryReadFor(rx_buffer, min_bytes, timeout);
  }

  /// Reads data from the UART into a provided buffer.
  ///
  /// This function blocks until either `rx_buffer.size()` bytes have been read
  /// into buffer or the specified timeout has elapsed, whichever occurs first.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param timeout    The maximum time to wait for data to be read. If zero,
  ///                   the function will immediately return with at least one
  ///                   hardware read operation attempt.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been filled
  ///   with data.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be filled.
  /// * May return other implementation-specific status codes.
  StatusWithSize TryReadExactlyFor(ByteSpan rx_buffer,
                                   chrono::SystemClock::duration timeout) {
    return DoTryReadFor(rx_buffer, rx_buffer.size(), timeout);
  }

  /// Deprecated: Prefer TryReadExactlyFor in new code.
  /// Reads data from the UART into a provided buffer.
  ///
  /// This function blocks until either `rx_buffer.size()` bytes have been read
  /// into buffer or the specified timeout has elapsed, whichever occurs first.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param timeout    The maximum time to wait for data to be read. If zero,
  ///                   the function will immediately return with at least one
  ///                   hardware read operation attempt.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been filled
  ///   with data.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be filled.
  /// * May return other implementation-specific status codes.
  StatusWithSize TryReadFor(ByteSpan rx_buffer,
                            chrono::SystemClock::duration timeout) {
    // TODO: https://pwbug.dev/368149122 - Remove after transition
    return DoTryReadFor(rx_buffer, timeout);
  }

  /// Writes data from the provided buffer to the UART. The function blocks
  /// until the entire buffer has been written.
  ///
  /// @param tx_buffer - The buffer to write data from.
  ///
  /// @returns
  /// * @OK: The operation was successful.
  /// * May return other implementation-specific status codes.
  Status Write(ConstByteSpan tx_buffer) {
    return DoTryWriteFor(tx_buffer, std::nullopt).status();
  }

  /// Writes data from the provided buffer to the UART. The function blocks
  /// until either the entire buffer has been written or the specified timeout
  /// has elapsed, whichever occurs first.
  ///
  /// @param tx_buffer  The buffer to write data from.
  /// @param timeout    The maximum time to wait for data to be written.
  ///                   If zero, the function will immediately return with at
  ///                   least one hardware write operation attempt.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been
  ///   written.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be written.
  /// * May return other implementation-specific status codes.
  StatusWithSize TryWriteFor(ConstByteSpan tx_buffer,
                             chrono::SystemClock::duration timeout) {
    return DoTryWriteFor(tx_buffer, timeout);
  }

  /// Blocks until all queued data in the UART  has been transmitted and the
  /// FIFO is empty.
  ///
  /// This function ensures that all data enqueued before calling this function
  /// has been transmitted. Any data enqueued after calling this function will
  /// be transmitted immediately.
  ///
  /// @returns
  /// * @OK: The operation was successful.
  /// * May return other implementation-specific status codes.
  Status FlushOutput() { return DoFlushOutput(); }

 private:
  /// Reads data from the UART into a provided buffer with an optional timeout
  /// provided.
  ///
  /// This virtual function attempts to read data into the provided byte buffer
  /// (`rx_buffer`). The operation will continue until either the buffer is
  /// full, an error occurs, or the optional timeout duration expires.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param timeout    An optional timeout duration. If specified, the function
  ///                   will block for no longer than this duration. If zero,
  ///                   the function will immediately return with at least one
  ///                   hardware read operation attempt. If not specified, the
  ///                   function blocks until the buffer is full.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been
  ///   filled with data.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be filled.
  /// * May return other implementation-specific status codes.
  // TODO: https://pwbug.dev/368149122 - Remove after transition.
  virtual StatusWithSize DoTryReadFor(
      ByteSpan rx_buffer,
      std::optional<chrono::SystemClock::duration> timeout) {
    return DoTryReadFor(rx_buffer, rx_buffer.size(), timeout);
  }

  /// Reads data from the UART into a provided buffer with an optional timeout
  /// provided.
  ///
  /// This virtual function attempts to read data into the provided byte buffer
  /// (`rx_buffer`). The operation will continue until either `min_bytes` have
  /// been read into the buffer, an error occurs, or the optional timeout
  /// duration expires.
  ///
  /// @param rx_buffer  The buffer to read data into.
  /// @param min_bytes  The minimum number of bytes to read before returning.
  /// @param timeout    An optional timeout duration. If specified, the function
  ///                   will block for no longer than this duration. If zero,
  ///                   the function will immediately return with at least one
  ///                   hardware read operation attempt. If not specified, the
  ///                   function blocks until the buffer is full.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been
  ///   filled with data.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be filled.
  /// * May return other implementation-specific status codes.
  // TODO: https://pwbug.dev/368149122 - Make pure virtual after transition.
  virtual StatusWithSize DoTryReadFor(
      [[maybe_unused]] ByteSpan rx_buffer,
      [[maybe_unused]] size_t min_bytes,
      [[maybe_unused]] std::optional<chrono::SystemClock::duration> timeout) {
    return StatusWithSize::Unimplemented();
  }

  /// @brief Writes data from a provided buffer to the UART with an optional
  /// timeout.
  ///
  /// This virtual function attempts to write data from the provided byte buffer
  /// (`tx_buffer`) to the UART. The operation will continue until either the
  /// buffer is empty, an error occurs, or the optional timeout duration
  /// expires.
  ///
  /// @param tx_buffer  The buffer containing data to be written.
  /// @param timeout    An optional timeout duration. If specified, the function
  ///                   will block for no longer than this duration. If zero,
  ///                   the function will immediately return after at least one
  ///                   hardware write operation attempt. If not specified, the
  ///                   function blocks until the buffer is empty.
  ///
  /// @returns
  /// * @OK: The operation was successful and the entire buffer has been
  ///   written.
  /// * @DEADLINE_EXCEEDED: The operation timed out before the entire buffer
  ///   could be written.
  /// * May return other implementation-specific status codes.
  virtual StatusWithSize DoTryWriteFor(
      ConstByteSpan tx_buffer,
      std::optional<chrono::SystemClock::duration> timeout) = 0;
  virtual Status DoFlushOutput() = 0;
};

}  // namespace pw::uart
