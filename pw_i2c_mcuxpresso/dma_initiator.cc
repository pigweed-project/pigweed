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

#define PW_LOG_MODULE_NAME "DMA_I2C"

#include "pw_i2c_mcuxpresso/dma_initiator.h"

#include <mutex>

#include "fsl_i2c_dma.h"
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
    case kStatus_I2C_Nak:
    case kStatus_I2C_Addr_Nak:
      return Status::Unavailable();
    case kStatus_I2C_InvalidParameter:
      return Status::InvalidArgument();
    case kStatus_I2C_Timeout:
      return Status::DeadlineExceeded();
    default:
      return Status::Unknown();
  }
}
}  // namespace

pw::ScopeGuard<pw::Function<void()>>
DmaMcuxpressoInitiator::AcquireScopedClock() {
  // Acquire the clock_tree element. Note that this function only requires the
  // IP clock and not the functional clock. However, ClockMcuxpressoClockIp
  // only provides the combined element, so that's what we use here.
  // Make sure it's released on any function exits through a scoped guard.
  PW_CHECK_OK(clock_tree_element_.Acquire());
  return pw::ScopeGuard(
      [this] { clock_tree_element_.Release().IgnoreError(); });
}

// inclusive-language: disable
void DmaMcuxpressoInitiator::Enable() {
  std::lock_guard lock(mutex_);
  EnableLocked();
}

void DmaMcuxpressoInitiator::EnableLocked() {
  pw::ScopeGuard guard = AcquireScopedClock();

  i2c_master_config_t master_config;
  I2C_MasterGetDefaultConfig(&master_config);
  master_config.baudRate_Bps = config_.baud_rate_bps;
  I2C_MasterInit(base_, &master_config, CLOCK_GetFreq(config_.clock_name));

  // Enable the DMA channel interrupts.
  // These are enabled by default by DMA_CreateHandle(), but re-enable them
  // anyway in case they were disabled for some reason.
  dma_channel_.EnableInterrupts();

  // Create the handle for the non-blocking transfer and register callback.
  I2C_MasterTransferCreateHandleDMA(
      base_,
      &i2c_master_dma_handle_,
      DmaMcuxpressoInitiator::TransferCompleteCallback,
      this,
      dma_channel_.handle());

  enabled_ = true;
}

void DmaMcuxpressoInitiator::Disable() {
  std::lock_guard lock(mutex_);
  DisableLocked();
}

void DmaMcuxpressoInitiator::DisableLocked() {
  pw::ScopeGuard guard = AcquireScopedClock();

  I2C_MasterDeinit(base_);
  enabled_ = false;
}

void DmaMcuxpressoInitiator::ResetLocked() {
  PW_LOG_WARN("Resetting I2C interface");
  DisableLocked();
  EnableLocked();
}

DmaMcuxpressoInitiator::~DmaMcuxpressoInitiator() { Disable(); }

void DmaMcuxpressoInitiator::TransferCompleteCallback(I2C_Type*,
                                                      i2c_master_dma_handle_t*,
                                                      status_t status,
                                                      void* initiator_ptr) {
  DmaMcuxpressoInitiator& initiator =
      *static_cast<DmaMcuxpressoInitiator*>(initiator_ptr);
  initiator.callback_isl_.lock();
  initiator.transfer_status_ = status;
  initiator.callback_isl_.unlock();

  // We cannot release clock_tree_element_ here since we are in an ISR.
  // It is released where callback_complete_notification_ is waited on.
  initiator.callback_complete_notification_.release();
}

Status DmaMcuxpressoInitiator::InitiateDmaTransferUntil(
    chrono::SystemClock::time_point deadline, i2c_master_transfer_t transfer) {
  pw::ScopeGuard guard = AcquireScopedClock();

  const status_t status =
      I2C_MasterTransferDMA(base_, &i2c_master_dma_handle_, &transfer);
  if (status != kStatus_Success) {
    return HalStatusToPwStatus(status);
  }

  if (!callback_complete_notification_.try_acquire_until(deadline)) {
    // If we're going to restart the interface for this bus, ignore trying to
    // abort the transfer. Otherwise, we need to keep things synced, so wait
    // for the transfer to abort.
    if (!config_.auto_restart_interface) {
      // Caveat emptor: this busy-waits for the controller to reset to the
      // idle state, which means potentially this could be an unbounded wait
      // if a peripheral is stuck! See also I2C_RETRY_WAIT.
      I2C_MasterTransferAbortDMA(base_, &i2c_master_dma_handle_);
    }

    return Status::DeadlineExceeded();
  }

  callback_isl_.lock();
  const status_t transfer_status = transfer_status_;
  callback_isl_.unlock();

  return HalStatusToPwStatus(transfer_status);
}

Status DmaMcuxpressoInitiator::DoTransferFor(
    span<const Message> messages, chrono::SystemClock::duration timeout) {
  chrono::SystemClock::time_point deadline =
      chrono::SystemClock::TimePointAfterAtLeast(timeout);

  std::lock_guard lock(mutex_);
  if (!enabled_) {
    return Status::FailedPrecondition();
  }

  for (unsigned int i = 0; i < messages.size(); ++i) {
    const Message& msg = messages[i];

    uint32_t i2c_flags = kI2C_TransferDefaultFlag;

    if (msg.IsWriteContinuation()) {
      i2c_flags |= kI2C_TransferNoStartFlag;
    } else if (i > 0) {
      // Use repeated start flag for all but the first message.
      i2c_flags |= kI2C_TransferRepeatedStartFlag;
    }

    // No stop flag prior to the final message.
    if (i < messages.size() - 1) {
      i2c_flags |= kI2C_TransferNoStopFlag;
    }
    i2c_master_transfer_t transfer{
        .flags = i2c_flags,
        .slaveAddress =
            msg.GetAddress().GetSevenBit(),  // Will CHECK if >7 bits.
        .direction = msg.IsRead() ? kI2C_Read : kI2C_Write,
        .subaddress = 0,
        .subaddressSize = 0,
        // Cast GetData() here because GetMutableData() is for Writes only.
        .data = const_cast<std::byte*>(msg.GetData().data()),
        .dataSize = msg.GetData().size()};
    auto status = InitiateDmaTransferUntil(deadline, transfer);

    if (!status.ok()) {
      if (status.IsDeadlineExceeded()) {
        if (config_.auto_restart_interface) {
          // If we've exceeded our deadline, that means our transaction
          // request never received a callback, indicating a stuck I2C
          // interface (or a device that stretched the clock beyond the
          // deadline).

          // Unfortunately, we have observed on RT595 platforms that the I2C
          // interface can get stuck and fail to transmit at all, with no
          // indication in the status registers that anything is wrong,
          // requiring a full reset.
          ResetLocked();
        }

        return Status::DeadlineExceeded();
      } else {
        // All other error statuses we return to the caller
        return status;
      }
    }
  }

  return pw::OkStatus();
}
// inclusive-language: enable

}  // namespace pw::i2c
