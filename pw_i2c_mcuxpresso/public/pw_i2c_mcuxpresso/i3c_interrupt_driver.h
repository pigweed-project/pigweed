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
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::i2c {

// inclusive-language: disable

// Interface to the fsl_i3c.c driver interrupt interface
//  - calls I3C_MasterTransferNonBlocking()
class I3cInterruptDriver final : public I3cDriver {
 public:
  I3cInterruptDriver();

 private:
  // I3cDriver Interface
  pw::Status DoInit(I3C_Type* base) override;
  void DoDeInit() override;
  pw::Status DoInitiateTransferUntil(pw::chrono::SystemClock::time_point,
                                     i3c_master_transfer_t*) override;

  // Non-blocking I3C transfer callback.
  static void TransferCompleteCallback(I3C_Type* base,
                                       i3c_master_handle_t* handle,
                                       status_t status,
                                       void* driver_ptr);

  I3C_Type* base_ = 0;
  bool enabled_ = false;

  i3c_master_handle_t handle_;
  i3c_master_transfer_callback_t callbacks_;

  // Transfer completion status for non-blocking I3C transfer.
  sync::TimedThreadNotification callback_complete_notification_;
  std::atomic<status_t> transfer_status_;
};

// inclusive-language: enable

}  // namespace pw::i2c
