// Copyright 2025 The Pigweed Authors
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

#include "pw_bytes/span.h"
#include "pw_i2c/address.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::i2c {

/// Application handler for i2c events.
///
/// Note: these functions will be called on an interrupt context.
class ResponderEvents {
 public:
  virtual ~ResponderEvents() = default;

  /// Called when a read start condition is seen.
  ///
  /// @returns
  /// * true to send an ACK
  /// * false to NACK
  ///
  /// @isrsafe{yes}
  [[nodiscard]] virtual bool OnStartRead() { return true; }

  /// Called when a write start condition is seen.
  ///
  /// @returns
  /// * true to send an ACK
  /// * false to NACK
  ///
  /// @isrsafe{yes}
  [[nodiscard]] virtual bool OnStartWrite() { return true; }

  /// Called when data is available on the bus.
  ///
  /// Depending on the IC, some buses have hardware caches so they will call
  /// this function once with a few bytes while others will call this once per
  /// byte. Either way, the write is not considered complete until the stop
  /// condition is called.
  /// @arg data The data written to the responder
  /// @returns
  /// * true to send an ACK
  /// * false to NACK
  ///
  /// @isrsafe{yes}
  [[nodiscard]] virtual bool OnWrite(ConstByteSpan data) {
    // Provide a name for argument to be used in doxygen
    static_cast<void>(data);
    return false;
  }

  /// Called when data is needed from the bus.
  ///
  /// The responder implementation is responsible for handling hardware that
  /// doesn't support a hardware cache. In those cases follow-up interrupts for
  /// reading the next byte should consume the next byte from the original span
  /// until all the bytes have been consumed. At which point this function will
  /// be called again.
  /// @returns @Result{ConstByteSpan}
  /// * @OK along with a byte span used to send to the initiator.
  ///   An ACK will be sent for each valid byte.
  /// * Any error status to NACK.
  ///
  /// @isrsafe{yes}
  [[nodiscard]] virtual Result<ConstByteSpan> OnRead() {
    return Status::Unimplemented();
  }

  /// Called when the stop condition is received.
  ///
  /// @returns
  /// * true to send an ACK
  /// * false to NACK
  [[nodiscard]] virtual bool OnStop() { return true; }
};

/// The Responder class provides an abstract interface for an I2C device
/// operating in responder (target) mode. It handles callbacks for various
/// I2C transaction events.
class Responder {
 public:
  constexpr Responder(Address address, ResponderEvents& events)
      : address_(address), events_(events) {}
  virtual ~Responder() = default;

  /// Start listening to the port
  ///
  /// @returns
  /// * @OK: The responder is now listening.
  /// * @UNAVAILABLE: The I2C device is not set up or doesn't exist.
  /// * @INTERNAL: The I2C device incurred an internal error.
  Status Enable() { return DoEnable(); }

  /// Stop listening to the port
  ///
  /// @returns
  /// * @OK: if the responder is no longer listening.
  /// * An error status if the responder failed to disable.
  Status Disable() { return DoDisable(); }

 protected:
  /// Implementation of Enable()
  virtual Status DoEnable() = 0;

  /// Implementation of Disable()
  virtual Status DoDisable() = 0;

  /// Called when the I2C initiator initiates a read operation from this
  /// responder. This indicates that the initiator is expecting data from the
  /// responder. The responder should prepare for subsequent OnRead() calls.
  ///
  /// @returns
  /// * true if the responder is ready to provide data.
  /// * false if the responder cannot handle the read operation.
  bool OnStartRead() { return events_.OnStartRead(); }

  /// Called when the I2C initiator initiates a write operation to this
  /// responder. This indicates that the initiator is about to send data to the
  /// responder. The responder should prepare for subsequent OnWrite() calls.
  ///
  /// @returns
  /// * true if the responder is ready to receive data.
  /// * false if the responder cannot handle the write operation.
  bool OnStartWrite() { return events_.OnStartWrite(); }

  /// Called when the I2C initiator has written data to the responder.
  /// This function may be called multiple times within a single I2C write
  /// transaction if the initiator sends data in chunks.
  ///
  /// @param data A span containing the data written by the initiator.
  /// @returns
  /// * true if the data was processed successfully.
  /// * false if there was an issue processing the data.
  bool OnWrite(ConstByteSpan data) { return events_.OnWrite(data); }

  /// Called when the I2C initiator is attempting to read data from the
  /// responder. The responder should fill the provided `buffer` with data to be
  /// sent to the initiator. This function may be called multiple times within a
  /// single I2C read transaction.
  ///
  /// @returns @Result{ConstByteSpan}
  /// * @OK: the span of bytes to be written to to the initiator.
  /// * An error code on failure when no data was read.
  Result<ConstByteSpan> OnRead() { return events_.OnRead(); }

  /// Called when the I2C initiator issues a STOP condition, signaling the end
  /// of the current transaction. This callback allows the responder to perform
  /// any necessary cleanup or state reset.
  /// @returns
  /// * true on success to respond with ACK
  /// * false on failure to respond with NACK
  bool OnStop() { return events_.OnStop(); }

  /// @returns The address of this responder.
  const Address& address() const { return address_; }

 private:
  const Address address_;
  ResponderEvents& events_;
};

}  // namespace pw::i2c
