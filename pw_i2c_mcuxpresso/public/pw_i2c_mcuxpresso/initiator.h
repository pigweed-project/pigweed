// Copyright 2022 The Pigweed Authors
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

#include "fsl_clock.h"
#include "fsl_i2c.h"
#include "pw_clock_tree/clock_tree.h"
#include "pw_i2c/initiator.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::i2c {

// Initiator interface implementation based on I2C driver in NXP MCUXpresso SDK.
// Currently supports only devices with 7 bit adresses.
class McuxpressoInitiator final : public Initiator {
 public:
  struct Config {
    uint32_t flexcomm_address;
    clock_name_t clock_name;
    uint32_t baud_rate_bps;

    // Automatically restart the I2C interface when a transaction has timed
    // out. This usually indicates the host interface has become stuck and
    // will require a restart.
    bool auto_restart_interface = false;
  };

  McuxpressoInitiator(const Config& config,
                      pw::clock_tree::Element& clock_tree_element)
      : Initiator(Initiator::Feature::kStandard),
        config_(config),
        base_(reinterpret_cast<I2C_Type*>(config_.flexcomm_address)),
        clock_tree_element_(clock_tree_element) {}

  McuxpressoInitiator(const Config& config)
      : Initiator(Initiator::Feature::kStandard),
        config_(config),
        base_(reinterpret_cast<I2C_Type*>(config_.flexcomm_address)) {}

  // Should be called before attempting any transfers.
  void Enable() PW_LOCKS_EXCLUDED(mutex_);
  void Disable() PW_LOCKS_EXCLUDED(mutex_);

  // Returns a pointer to the underlying MCUXpresso I2C peripheral base address
  // structure. This can be used for direct interaction with the I2C hardware
  // register.
  I2C_Type* base() const { return base_; }

  ~McuxpressoInitiator() final;

 private:
  void EnableLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void DisableLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void ResetLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  Status DoTransferFor(span<const Message> messages,
                       chrono::SystemClock::duration timeout) override
      PW_LOCKS_EXCLUDED(mutex_);

  Status TransferSequenceUntilLocked(span<const Message> messages,
                                     chrono::SystemClock::time_point deadline)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // inclusive-language: disable
  Status InitiateNonBlockingTransferUntil(
      chrono::SystemClock::time_point deadline, i2c_master_transfer_t* transfer)
      PW_LOCKS_EXCLUDED(callback_isl_);

  // Non-blocking I2C transfer callback.
  static void TransferCompleteCallback(I2C_Type* base,
                                       i2c_master_handle_t* handle,
                                       status_t status,
                                       void* initiator_ptr)
      PW_LOCKS_EXCLUDED(callback_isl_);
  // inclusive-language: enable

  sync::Mutex mutex_;
  Config const config_;
  I2C_Type* const base_;
  pw::clock_tree::OptionalElement clock_tree_element_;
  bool enabled_ PW_GUARDED_BY(mutex_);

  // Transfer completion status for non-blocking I2C transfer.
  sync::TimedThreadNotification callback_complete_notification_;
  sync::InterruptSpinLock callback_isl_;
  status_t transfer_status_ PW_GUARDED_BY(callback_isl_);

  // inclusive-language: disable
  i2c_master_handle_t handle_ PW_GUARDED_BY(mutex_);
  // inclusive-language: enable
};

}  // namespace pw::i2c
