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

#include <atomic>

// inclusive-language: disable
#include "fsl_flexio_i2c_master.h"
// inclusive-language: enable
#include "pw_chrono/system_clock.h"
#include "pw_clock_tree/clock_tree.h"
#include "pw_i2c/initiator.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::i2c {

// Initiator interface implementation based on FlexIo I2C driver in NXP
// MCUXpresso SDK. Currently supports only devices with 7 bit addresses.
//
// Do note that NXP FlexIo offers I2C emulation, but not native hardware
// support. This effectively means I2C over FlexIo has several limitations:
// - No Clock Stretching
// - No Arbitration
// - No proper differentiation between overflow and NACK
// - No transfer flag support, i.e. no support for NO_START, REPEATED_START and
//     NO_STOP flags.
class FlexIoMcuxpressoInitiator final : public Initiator {
 public:
  struct Config {
    uint32_t flexio_address;
    clock_name_t clock_name;
    uint32_t baud_rate_bps;
    // FlexIo channel definition, as enumeration from 0 to the max number of
    // FlexIo-supported pins. The specific pins depend on the hardware
    // configuration.
    uint8_t flexio_sda_channel;
    uint8_t flexio_scl_channel;
    // Optional clock tree element.
    pw::clock_tree::Element* clock_tree_element{};
  };

  FlexIoMcuxpressoInitiator(const Config& config)
      : Initiator(Initiator::Feature::kStandard),
        config_(config),
        base_(CreateBase(config)),
        clock_tree_element_(config.clock_tree_element) {}

  // Should be called before attempting any transfers.
  void Enable() PW_LOCKS_EXCLUDED(mutex_);
  void Disable() PW_LOCKS_EXCLUDED(mutex_);

  // Returns a pointer to the underlying MCUXpresso I2C peripheral base address
  // structure. This can be used for direct interaction with the I2C hardware
  // register.
  FLEXIO_I2C_Type* base() { return &base_; }

  ~FlexIoMcuxpressoInitiator() final;

 private:
  // inclusive-language: disable

  // Implements DoWriteReadFor instead of DoTransferFor due to the no
  // transfer-flag support in fsl_flexio_i2c_master, forcing this implementation
  // to only support:
  // - single read, e.g. triggered by ReadFor, or
  // - single write, e.g. triggered by WriteFor, or
  // - read-after-write operation, using the subaddress (max 4 bytes write).
  //   An InvalidArgument status is returned if more than 4 bytes are requested
  //   for the write.
  //
  // Returns
  // - InvalidArgument status if no read and no write is requested.
  // - DeadlineExceeded status if the transfer is not finished within the
  //   provided timeout.
  // - Unavailable if the I2C bus is busy or the device replied with a NACK.
  // - Internal status if the underlying driver returned other errors.
  // - OkStatus otherwise.
  Status DoWriteReadFor(Address device_address,
                        ConstByteSpan tx_buffer,
                        ByteSpan rx_buffer,
                        chrono::SystemClock::duration timeout) override
      PW_LOCKS_EXCLUDED(mutex_);

  // Initiates the provided transfer and waits until TransferCompleteCallback is
  // called or the deadline is exceeded.
  Status InitiateTransferUntil(chrono::SystemClock::time_point deadline,
                               flexio_i2c_master_transfer_t* transfer)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Callback invoked on transfer completion, unblocks InitiateTransferUntil.
  static void TransferCompleteCallback(FLEXIO_I2C_Type* base,
                                       flexio_i2c_master_handle_t* handle,
                                       status_t status,
                                       void* initiator_ptr);
  // inclusive-language: enable

  inline static FLEXIO_I2C_Type CreateBase(const Config& config) {
    return FLEXIO_I2C_Type(
        /*flexioBase=*/reinterpret_cast<FLEXIO_Type*>(config.flexio_address),
        /*SDAPinIndex=*/config.flexio_sda_channel,
        /*SCLPinIndex=*/config.flexio_scl_channel,
        /*shifterIndex=*/{0U, 1U},
        /*timerIndex=*/{0U, 1U, 2U});
  }

  sync::Mutex mutex_;
  const Config config_;
  FLEXIO_I2C_Type base_;
  pw::clock_tree::OptionalElement clock_tree_element_;
  bool enabled_ PW_GUARDED_BY(mutex_) = false;

  // Transfer completion status for non-blocking I2C transfer.
  sync::TimedThreadNotification callback_complete_notification_;
  std::atomic<status_t> transfer_status_;

  // inclusive-language: disable
  flexio_i2c_master_handle_t handle_ PW_GUARDED_BY(mutex_);
  // inclusive-language: enable
};

}  // namespace pw::i2c
