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

#define PW_LOG_MODULE_NAME "FLEXIO_I2C"

#include "pw_i2c_mcuxpresso/flexio_initiator.h"

#include <mutex>

// inclusive-language: disable
#include "fsl_flexio_i2c_master.h"
#include "pw_assert/check.h"
#include "pw_chrono/system_clock.h"
#include "pw_function/scope_guard.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::i2c {
namespace {

Status HalStatusToPwStatus(status_t status) {
  switch (status) {
    case kStatus_Success:
      return OkStatus();
    case kStatus_FLEXIO_I2C_Busy:
    case kStatus_FLEXIO_I2C_Nak:
      return Status::Unavailable();
    case kStatus_InvalidArgument:
      return Status::Internal();
    case kStatus_FLEXIO_I2C_Timeout:
      return Status::DeadlineExceeded();
    default:
      return Status::Internal();
  }
}
}  // namespace

void FlexIoMcuxpressoInitiator::Enable() {
  std::lock_guard lock(mutex_);

  // Acquire the clock_tree element. Note that this function only requires the
  // IP clock and not the functional clock. However, ClockMcuxpressoClockIp
  // only provides the combined element, so that's what we use here.
  // Make sure it's released on any function exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  // Initiate FlexIO peripheral.
  flexio_config_t flexio_config;
  FLEXIO_GetDefaultConfig(&flexio_config);
  FLEXIO_Init(base_.flexioBase, &flexio_config);

  flexio_i2c_master_config_t masterConfig;
  FLEXIO_I2C_MasterGetDefaultConfig(&masterConfig);
  masterConfig.baudRate_Bps = config_.baud_rate_bps;

  // Initiate FlexIo I2C Master.
  status_t status = FLEXIO_I2C_MasterInit(
      &base_, &masterConfig, CLOCK_GetFreq(config_.clock_name));
  if (status != kStatus_Success) {
    PW_LOG_ERROR("FLEXIO_I2C_MasterInit failed: %d", status);
    FLEXIO_Deinit(base_.flexioBase);
    return;
  }

  // Create the handle for the non-blocking transfer and register callback.
  status = FLEXIO_I2C_MasterTransferCreateHandle(
      &base_,
      &handle_,
      FlexIoMcuxpressoInitiator::TransferCompleteCallback,
      this);
  if (status != kStatus_Success) {
    PW_LOG_ERROR("FLEXIO_I2C_MasterTransferCreateHandle failed: %d", status);
    FLEXIO_I2C_MasterDeinit(&base_);
    FLEXIO_Deinit(base_.flexioBase);
    return;
  }

  enabled_ = true;
}

void FlexIoMcuxpressoInitiator::Disable() {
  std::lock_guard lock(mutex_);

  // Acquire the clock_tree element. Note that this function only requires the
  // IP clock and not the functional clock. However, ClockMcuxpressoClockIp
  // only provides the combined element, so that's what we use here.
  // Make sure it's released on any function exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  FLEXIO_I2C_MasterDeinit(&base_);
  FLEXIO_Deinit(base_.flexioBase);
  enabled_ = false;
}

FlexIoMcuxpressoInitiator::~FlexIoMcuxpressoInitiator() { Disable(); }

void FlexIoMcuxpressoInitiator::TransferCompleteCallback(
    FLEXIO_I2C_Type* /*&base*/,
    flexio_i2c_master_handle_t* /*handle*/,
    status_t status,
    void* initiator_ptr) {
  FlexIoMcuxpressoInitiator* initiator =
      static_cast<FlexIoMcuxpressoInitiator*>(initiator_ptr);
  initiator->transfer_status_ = status;

  // We cannot release clock_tree_element_ here since we are in an ISR.
  // It is released where callback_complete_notification_ is waited on.
  initiator->callback_complete_notification_.release();
}

Status FlexIoMcuxpressoInitiator::InitiateTransferUntil(
    chrono::SystemClock::time_point deadline,
    flexio_i2c_master_transfer_t* transfer) {
  // Acquire the clock_tree_element. Use a scoped guard so it's released from
  // any function return.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  if (const auto status =
          FLEXIO_I2C_MasterTransferNonBlocking(&base_, &handle_, transfer);
      status != kStatus_Success) {
    PW_LOG_DEBUG("Failed to initiate FLEXIO I2C transfer: %d", status);
    return HalStatusToPwStatus(status);
  }

  if (!callback_complete_notification_.try_acquire_until(deadline)) {
    FLEXIO_I2C_MasterTransferAbort(&base_, &handle_);
    return Status::DeadlineExceeded();
  }

  const status_t transfer_status = transfer_status_;
  const Status pw_status = HalStatusToPwStatus(transfer_status);
  if (!pw_status.ok()) {
    PW_LOG_DEBUG("FLEXIO I2C transfer failed with status: %d", transfer_status);
  }
  return pw_status;
}

// Performs blocking I2C write, read and read-after-write depending on the tx
// and rx buffer states.
Status FlexIoMcuxpressoInitiator::DoWriteReadFor(
    Address device_address,
    ConstByteSpan tx_buffer,
    ByteSpan rx_buffer,
    chrono::SystemClock::duration timeout) {
  chrono::SystemClock::time_point deadline =
      chrono::SystemClock::TimePointAfterAtLeast(timeout);

  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return Status::FailedPrecondition();
  }

  auto transfer = flexio_i2c_master_transfer_t{
      .flags = 0,  // Transfer flag is reserved for FlexIO I2C.
      .slaveAddress = device_address.GetSevenBit(),  // Will CHECK if >7 bits.
      .subaddress = 0,
      .subaddressSize = 0,
  };

  if (!tx_buffer.empty() && rx_buffer.empty()) {
    // Write
    transfer.direction = kFLEXIO_I2C_Write;
    transfer.data = const_cast<uint8_t*>(
        reinterpret_cast<const uint8_t*>(tx_buffer.data()));
    transfer.dataSize = tx_buffer.size();
  } else if (tx_buffer.empty() && !rx_buffer.empty()) {
    // Read
    transfer.direction = kFLEXIO_I2C_Read;
    transfer.data = reinterpret_cast<uint8_t*>(rx_buffer.data());
    transfer.dataSize = rx_buffer.size();
  } else if (!tx_buffer.empty() && !rx_buffer.empty()) {
    // Read-after-write can be implemented as a FlexIo Read transfer with
    // subaddress.

    if (tx_buffer.size() > 4) {
      PW_LOG_WARN(
          "FLEXIO I2C read-after-write supports a max of 4 bytes write.");
      return Status::InvalidArgument();
    }

    uint32_t subaddress = 0;
    for (size_t i = 0; i < tx_buffer.size(); ++i) {
      subaddress =
          (subaddress << 8) | static_cast<uint8_t>(tx_buffer.data()[i]);
    }

    transfer.direction = kFLEXIO_I2C_Read;
    transfer.subaddress = subaddress;
    transfer.subaddressSize = static_cast<uint8_t>(tx_buffer.size());
    transfer.data = reinterpret_cast<uint8_t*>(rx_buffer.data());
    transfer.dataSize = rx_buffer.size();
  } else {
    PW_LOG_WARN(
        "FLEXIO I2C transfer called with both tx and rx buffers empty.");
    return Status::InvalidArgument();
  }

  return InitiateTransferUntil(deadline, &transfer);
}
// inclusive-language: enable

}  // namespace pw::i2c
