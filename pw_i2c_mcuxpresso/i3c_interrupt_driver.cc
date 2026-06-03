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

#include "pw_i2c_mcuxpresso/i3c_interrupt_driver.h"

#include "fsl_clock.h"
#include "fsl_i3c.h"
#include "pw_i2c_mcuxpresso/i3c_common.h"
#include "pw_log/log.h"

namespace pw::i2c {

I3cInterruptDriver::I3cInterruptDriver() {}

// inclusive-language: disable
pw::Status I3cInterruptDriver::DoInit(I3C_Type* base) {
  base_ = base;
  callbacks_ = {
      .slave2Master = nullptr,
      .ibiCallback = nullptr,
      .transferComplete = I3cInterruptDriver::TransferCompleteCallback};

  I3C_MasterTransferCreateHandle(base_, &handle_, &callbacks_, this);
  enabled_ = true;
  return pw::OkStatus();
}

void I3cInterruptDriver::DoDeInit() { I3C_MasterDeinit(base_); }

pw::Status I3cInterruptDriver::DoInitiateTransferUntil(
    pw::chrono::SystemClock::time_point deadline,
    i3c_master_transfer_t* transfer) {
  const status_t status =
      I3C_MasterTransferNonBlocking(base_, &handle_, transfer);
  if (status != kStatus_Success) {
    return HalStatusToPwStatus(status);
  }

  if (!callback_complete_notification_.try_acquire_until(deadline)) {
    I3C_MasterTransferAbort(base_, &handle_);
    return Status::DeadlineExceeded();
  }

  if (transfer_status_ != kStatus_Success) {
    PW_LOG_INFO("NonBlockingTransfer failed transfer status %d",
                transfer_status_.load());
  }
  return HalStatusToPwStatus(transfer_status_);
}

void I3cInterruptDriver::TransferCompleteCallback(I3C_Type* base,
                                                  i3c_master_handle_t* handle,
                                                  status_t status,
                                                  void* driver_ptr) {
  I3cInterruptDriver& driver = *static_cast<I3cInterruptDriver*>(driver_ptr);
  driver.transfer_status_ = status;
  driver.callback_complete_notification_.release();
}
// inclusive-language: enable

}  // namespace pw::i2c
