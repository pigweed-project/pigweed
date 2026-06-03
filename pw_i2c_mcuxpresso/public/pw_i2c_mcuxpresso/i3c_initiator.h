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

#include <atomic>
#include <optional>

#include "config.h"
#include "fsl_clock.h"
#include "fsl_i3c.h"
#include "pw_bytes/span.h"
#include "pw_clock_tree/clock_tree.h"
#include "pw_containers/vector.h"
#include "pw_i2c/initiator.h"
#include "pw_i2c_mcuxpresso/i3c_ccc.h"
#include "pw_i2c_mcuxpresso/i3c_driver.h"
#include "pw_i2c_mcuxpresso/i3c_interrupt_driver.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::i2c {

// I2C initiator interface implementation fsl_i3c driver in NXP MCUXpresso SDK.
class I3cMcuxpressoInitiator final : public pw::i2c::Initiator {
 public:
  struct Config {
    uint32_t base_address;              // I3C peripheral base address.
    uint32_t i2c_baud_rate;             // I2C baud rate in Hz.
    uint32_t i3c_open_drain_baud_rate;  // I3C open drain baud rate in Hz.
    uint32_t i3c_push_pull_baud_rate;   // I3C push pull baud rate in Hz.
    bool enable_open_drain_stop;  // Whether to emit open-drain speed STOP.
    bool enable_open_drain_high;  // Enable Open-Drain High to be 1 PPBAUD count
                                  // for I3C messages, or 1 ODBAUD.
  };

  /// Construct I3c Initiator with a config, a clock tree element,
  /// and a custom interface driver to the fsl_i3c layer.
  I3cMcuxpressoInitiator(const Config& config,
                         pw::clock_tree::Element& clock_tree_element,
                         I3cDriver& i3c_driver)
      : Initiator(Initiator::Feature::kStandard),
        config_(config),
        base_(reinterpret_cast<I3C_Type*>(config.base_address)),
        clock_tree_element_(clock_tree_element),
        driver_(i3c_driver) {}

  /// Construct I3c Initiator with a config, a clock tree element
  I3cMcuxpressoInitiator(const Config& config,
                         pw::clock_tree::Element& clock_tree_element)
      : Initiator(Initiator::Feature::kStandard),
        config_(config),
        base_(reinterpret_cast<I3C_Type*>(config.base_address)),
        clock_tree_element_(clock_tree_element),
        default_driver_(std::in_place),
        driver_(default_driver_.value()) {}

  /// Construct I3c Initiator with a config
  I3cMcuxpressoInitiator(const Config& config)
      : Initiator(Initiator::Feature::kStandard),
        config_(config),
        base_(reinterpret_cast<I3C_Type*>(config.base_address)),
        default_driver_(std::in_place),
        driver_(default_driver_.value()) {}

  ~I3cMcuxpressoInitiator() final;

  // Initializes the I3C controller peripheral as configured in the constructor.
  void Enable() PW_LOCKS_EXCLUDED(mutex_);

  // Deinitializes the I3C controller peripheral.
  void Disable() PW_LOCKS_EXCLUDED(mutex_);

  // Set dynamic address list that will be used to assign dynamic addresses to
  // I3C devices on the bus during bus initialization step.
  //
  // Subsequent transfers using this initiator to these addresses will
  // use the i3c protocol.
  //
  // If this value is not set, or is an empty span, no dynamic address
  // initialization will occur on the bus.
  //
  // Calling this function again will overwrite the previous list and be used
  // if Disable(), Enable(), and Initialize() are called again.
  pw::Status SetDynamicAddressList(
      pw::span<const Address> dynamic_address_list);

  // Set dynamic address list that will be used to assign dynamic addresses to
  // I3C devices on the bus during bus initialization step.
  //
  // Subsequent transfers using this initiator to these addresses will
  // use the i3c protocol.
  //
  // If this value is not set, or is an empty span, no dynamic address
  // initialization will occur on the bus.
  //
  // Calling this function again will overwrite the previous list and be used
  // if Disable(), Enable(), and Initialize() are called again.
  [[deprecated("Use SetDynamicAddressList(span<Address>)")]]
  pw::Status SetDynamicAddressList(
      pw::span<const uint8_t> dynamic_address_list);

  // Set the static address list. All addresses on this list will be sent an
  // i3c SETDASA command to convert their static i2c address to a dynamic i3c
  // address during initialization. The SETDASA will be sent before any
  // dynamic address initialization.
  //
  // Note: Subsequent transfers from this initiator to these addresses will use
  // the i3c protocol.
  //
  // Note: I3C refers to all i3c addresses as "dynamic addresses", even if they
  // are assigned based on the static i2c address of the target device using
  // SETDASA.
  //
  // Calling this function again will overwrite the previous list and be used
  // if Disable(), Enable(), and Initialize() are called again.
  pw::Status SetStaticAddressList(pw::span<const Address> static_address_list);

  // Initialize the I3C bus (Static and Dynamic address assignment)
  //
  // If dynamic address assignment is desired, a call to SetDynamicAddressList()
  // is required before calling this method.
  //
  // If static address assignment is desired, a call to SetStaticAddressList()
  // is required before calling this method.
  pw::Status Initialize() PW_LOCKS_EXCLUDED(mutex_);

  // Request that a target use its i2c static address as its i3c dynamic
  // address. This method can be used when a single device (for example
  // recently powered on) needs to have its i3c address set for communication
  // on the i3c bus.
  //
  // SETDASA is the i3c command "Set Dynamic Address from Static Address".
  //
  // Enable() needs to be called before this method.
  pw::Status SetDasa(pw::i2c::Address static_addr) PW_LOCKS_EXCLUDED(mutex_);

  // Forget an address that was previously assigned.
  //
  // This is helpful when a device has been powered off and has lots its i3c
  // address. After calling this, any transfers to this device will again be
  // in i2c mode.
  void ForgetAssignedAddress(pw::i2c::Address address)
      PW_LOCKS_EXCLUDED(mutex_);

  // Broadcast the i3c control command RSTDAA (Reset Dynamic Addressing).
  // This will cause all i3c targets to drop their i3c address and revert
  // to their uninitialized, i2c-only state.
  //
  // This command is useful when the i3c initiator is going to shutdown and
  // the bus should be returned its original state.
  //
  // After calling this, you will need to call Initialize() again to assign
  // i3c target dynamic addresses to communicate over i3c.
  pw::Status ResetAddressing() PW_LOCKS_EXCLUDED(mutex_);

  // Set the target's maximum read length by sending an i3c SETMRL message.
  // The target i3c device must have `address` assigned as its i3c address.
  pw::Status SetMaxReadLength(pw::i2c::Address address,
                              uint16_t max_read_length)
      PW_LOCKS_EXCLUDED(mutex_);

  // Get the target's maximum read length by sending an i3c GETMRL message.
  // The target i3c device must have `address` assigned as its i3c address.
  pw::Result<uint16_t> GetMaxReadLength(pw::i2c::Address address)
      PW_LOCKS_EXCLUDED(mutex_);

  // Set the target's maximum write length by sending an i3c SETMWL message.
  // The target i3c device must have `address` assigned as its i3c address.
  pw::Status SetMaxWriteLength(pw::i2c::Address address,
                               uint16_t max_write_length)
      PW_LOCKS_EXCLUDED(mutex_);

  // Get the target's maximum write length by sending an i3c GETMWL message.
  // The target i3c device must have `address` assigned as its i3c address.
  pw::Result<uint16_t> GetMaxWriteLength(pw::i2c::Address address)
      PW_LOCKS_EXCLUDED(mutex_);

 private:
  pw::Status DoTransferCcc(I3cCccAction rnw,
                           I3cCcc ccc_id,
                           pw::i2c::Address address,
                           pw::ByteSpan buffer)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  pw::Status DoTransferFor(span<const Message> messages,
                           chrono::SystemClock::duration timeout) override
      PW_LOCKS_EXCLUDED(mutex_);

  pw::Result<i3c_bus_type_t> ValidateAndDetermineProtocol(
      span<const Message> messages) const;

  // Record an address as being dynamically assigned and in i3c mode.
  pw::Status AddAssignedI3cAddress(pw::i2c::Address address);

  // inclusive-language: disable
  Status InitiateTransferUntil(chrono::SystemClock::time_point deadline,
                               i3c_master_transfer_t* transfer);
  // inclusive-language: enable

  // Request that a target use its i2c static address as its i3c dynamic
  // address.
  // SETDASA is the i3c command "Set Dynamic Address from Static Address".
  pw::Status DoSetDasa(pw::i2c::Address static_addr)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  pw::Status DoResetAddressing() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  const Config& config_;
  I3C_Type* base_;
  pw::clock_tree::OptionalElement clock_tree_element_;
  i3c_device_info_t* device_list_ = nullptr;
  uint8_t device_count_ = 0;
  bool enabled_ PW_GUARDED_BY(mutex_) = false;
  pw::sync::Mutex mutex_;
  std::optional<pw::Vector<Address, I3C_MAX_DEVCNT>> i3c_dynamic_address_list_;
  std::optional<pw::Vector<Address, I3C_MAX_DEVCNT>> i3c_static_address_list_;
  pw::Vector<Address, I3C_MAX_DEVCNT> i3c_assigned_addresses_;

  // Default fsl_i3c interrupt-based driver
  std::optional<I3cInterruptDriver> default_driver_;

  // Driver for interfacing with fsl_i3c transfer subsystems
  I3cDriver& driver_;
};

}  // namespace pw::i2c
