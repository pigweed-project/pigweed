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
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::i2c {

// inclusive-language: disable

class I3cDriver {
 public:
  virtual ~I3cDriver() = default;

  Status Init(I3C_Type* base) { return DoInit(base); }
  void DeInit() { return DoDeInit(); }
  Status InitiateTransferUntil(chrono::SystemClock::time_point deadline,
                               i3c_master_transfer_t* transfer) {
    return DoInitiateTransferUntil(deadline, transfer);
  }

 private:
  // Perform initialize of the I3C interface
  // Called when the I3C initiator is Enabled
  virtual Status DoInit(I3C_Type* base) = 0;

  // Perform de-initialization of the I3C interface
  // Called when the I3C initiator is Disabled.
  virtual void DoDeInit() = 0;

  // Should make an I3C_* call to start a transfer.
  // Should call the matching I3C_Abort* function and
  // return an error code if deadline is reached before
  // the transfer is complete.
  virtual Status DoInitiateTransferUntil(
      chrono::SystemClock::time_point deadline,
      i3c_master_transfer_t* transfer) = 0;
};

// inclusive-language: enable

}  // namespace pw::i2c
