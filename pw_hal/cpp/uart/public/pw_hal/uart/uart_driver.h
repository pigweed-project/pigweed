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

#include <cstddef>

#include "pw_buf/buf.h"
#include "pw_function/function.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::hal::uart {

/// @submodule{pw_hal,uart}

class BufferedUartDriver {
 public:
  /// Default virtual constructor.
  virtual ~BufferedUartDriver() = default;

  /// Enable the UART device/driver.
  ///
  /// Initializes the UART hardware and prepares the driver for transmitting and
  /// receiving data. Calling Enable() when the driver is already enabled is a
  /// no-op and returns `OkStatus()`.
  ///
  /// @returns
  /// * @OK: The UART driver was successfully enabled.
  /// * @FAILED_PRECONDITION: The driver is not initialized or configured.
  /// * @INTERNAL: An internal error occurred while enabling the hardware.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual Status Enable() = 0;

  /// Disable the UART device/driver.
  ///
  /// Shuts down UART communication and disables the hardware and associated
  /// interrupts. Calling Disable() when the driver is already disabled is a
  /// no-op and returns `OkStatus()`.
  ///
  /// @returns
  /// * @OK: The UART driver was successfully disabled.
  /// * @INTERNAL: An internal error occurred while disabling the hardware.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual Status Disable() = 0;

  // TX API ===================================================================

  /// Get the maximum internal buffer capacity of the TX device/driver.
  ///
  /// The specific definition of "maximum internal buffer capacity" depends on
  /// the implementation of the driver and hardware, but typically it will
  /// either be the hardware FIFO size for a raw driver or the internal
  /// circular buffer size for a buffer-backed driver.
  ///
  /// @returns The maximum internal buffer capacity of the TX device/driver.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  size_t tx_buffer_capacity() const { return tx_buffer_capacity_; }

  /// Get the current available bytes in the internal buffer.
  ///
  /// If non-zero, then the next Send() call will accept at least that many
  /// bytes.
  ///
  /// @returns The number of bytes available in the internal buffer (or FIFO).
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{no}
  virtual size_t tx_bytes_available() const = 0;

  /// Set the level at which the _space available_ callback is called.
  ///
  /// When this level is reached, it is guaranteed that the next call to
  /// Send() will be able to accept _at least_ `bytes` number of bytes (or
  /// the maximum buffer capacity, whichever is lower).
  ///
  /// @note The notification level does not guarantee that multiple calls to
  ///       Send() will be able to accept that many bytes. For example, if
  ///       the level is set to 16 bytes, it is not guaranteed that two
  ///       Send() calls of 8 bytes will succeed. Only that the first call
  ///       will succeed. The second call may or may not depending on the
  ///       specific driver implementation.
  ///
  /// @note The notification will be called when _at least_ that many bytes are
  ///       available to be sent. The buffer may have additional space
  ///       available beyond that.
  ///
  /// @param bytes The new level at which to notify the user of space available
  ///        in the buffer.
  /// @returns
  /// * @OK: Level was set successfully.
  /// * @INVALID_ARGUMENT: The specified level was not valid.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual Status SetTxReadyNotifyLevel(size_t bytes) = 0;

  /// Set the notification callback used to notify the user of available space
  /// in the TX buffer.
  ///
  /// @warning The `on_ready` callback may be called from ISR context. It should
  ///          follow all best practices for [ISR-safe
  ///          functions](https://pigweed.dev/embedded_cpp_guide.html#isr-safety).
  ///
  /// @param on_ready The callback function to call when the TX buffer
  ///        notification level is reached. The size_t parameter in the
  ///        callback is the number of bytes currently available to send.
  /// @returns
  /// * @OK: Successfully set notification callback.
  /// * @UNAVAILABLE: Driver cannot change the notifier at this time.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual Status SetTxReadyNotifier(pw::Function<void(size_t)> on_ready) = 0;

  /// Send the given array of bytes.
  ///
  /// "Consumes" the bytes in the span from lowest to highest index. This
  /// function is considered "fire and forget", meaning the bytes may not have
  /// actually been sent across the wire when the function returns. The buffer
  /// may be stored internally for later transmission.
  ///
  /// @note This function is guaranteed not to block.
  ///
  /// @param tx_buffer The buffer of bytes to send.
  /// @return A Status along with the number of bytes accepted.
  /// * @OK: All bytes were accepted. Included size is equal to the size of the
  ///   provided span.
  /// * @RESOURCE_EXHAUSTED: Unable to accept all bytes. Some bytes may have
  ///   been accepted, check the included size to see how many bytes were
  ///   accepted.
  /// * @UNAVAILABLE: The driver is not available for sending at this time.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual StatusWithSize Send(pw::span<const std::byte> tx_buffer) = 0;

  // RX API ===================================================================

  /// Get the maximum internal buffer capacity of the RX device/driver.
  ///
  /// The specific definition of "maximum internal buffer capacity" depends on
  /// the implementation of the driver and hardware, but typically it will
  /// either be the hardware FIFO size for a raw driver or the internal
  /// circular buffer size for a buffer-backed driver.
  ///
  /// @returns The maximum internal buffer capacity of the RX device/driver.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  size_t rx_buffer_capacity() const { return rx_buffer_capacity_; }

  /// Get the current available bytes in the internal buffer.
  ///
  /// If non-zero, then the next Receive() call will provide at least that many
  /// bytes.
  ///
  /// @note There are _at least_ this many bytes available, there may be more.
  ///
  /// @returns The number of bytes available in the internal buffer (or FIFO).
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{no}
  virtual size_t rx_bytes_available() const = 0;

  /// Set the buffer level at which the notify callback is called.
  ///
  /// When this level is reached, it is guaranteed that the next call to
  /// Receive() will be able to provide _at least_ `bytes` number of bytes.
  ///
  /// @note The notification will be called when _at least_ that many bytes are
  ///       available. The buffer may have additional bytes beyond that.
  ///
  /// @param bytes The new level at which to notify the user of bytes available
  ///        in the buffer.
  /// @returns
  /// * @OK: Level was set successfully.
  /// * @INVALID_ARGUMENT: The specified level was not valid.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual Status SetRxReadyNotifyLevel(size_t bytes) = 0;

  /// Set the notification callback used to notify the user of available bytes
  /// in the RX buffer.
  ///
  /// @warning The `on_ready` callback may be called from ISR context. It should
  ///          follow all best practices for [ISR-safe
  ///          functions](https://pigweed.dev/embedded_cpp_guide.html#isr-safety).
  ///
  /// @param on_ready The callback function to call when the RX buffer
  ///        notification level is reached. The size_t parameter in the
  ///        callback is the number of bytes currently available to receive.
  /// @returns
  /// * @OK: Successfully set notification callback.
  /// * @UNAVAILABLE: Driver cannot change the notifier at this time.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual Status SetRxReadyNotifier(pw::Function<void(size_t)> on_ready) = 0;

  /// Receive up to the given array size of bytes.
  ///
  /// "Provides" bytes to the span from lowest to highest index. This
  /// function will not wait to fill the buffer, it will provide any bytes
  /// available at call-time (up to the size of the span) then return to the
  /// caller.
  ///
  /// @note This function is guaranteed not to block.
  ///
  /// @param rx_buffer The buffer to write bytes into.
  /// @return A Status along with the number of bytes accepted.
  /// * @OK: Buffer was filled. Included size is equal to the size of the
  ///   provided span.
  /// * @RESOURCE_EXHAUSTED: Unable to fill buffer. Not enough bytes were
  ///   available, check the included size to see how many bytes were provided.
  /// * @UNAVAILABLE: The driver is not available for receiving at this time.
  ///
  /// @threadsafe{no}
  /// @isrsafe{no}
  /// @nmisafe{no}
  virtual StatusWithSize Receive(pw::span<std::byte> rx_buffer) = 0;

 protected:
  constexpr BufferedUartDriver(size_t tx_buffer_capacity,
                               size_t rx_buffer_capacity)
      : tx_buffer_capacity_(tx_buffer_capacity),
        rx_buffer_capacity_(rx_buffer_capacity) {}

 private:
  size_t tx_buffer_capacity_;
  size_t rx_buffer_capacity_;
};

/// @endsubmodule

}  // namespace pw::hal::uart
