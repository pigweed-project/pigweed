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

#include "pw_i2c_mcuxpresso/i3c_common.h"

namespace pw::i2c {

pw::Status HalStatusToPwStatus(status_t status) {
  switch (status) {
    case kStatus_Success:
      return pw::OkStatus();
    case kStatus_I3C_Timeout:
      return pw::Status::DeadlineExceeded();
    case kStatus_I3C_Nak:
    case kStatus_I3C_Busy:
    case kStatus_I3C_IBIWon:
    case kStatus_I3C_WriteAbort:
      return pw::Status::Unavailable();
    case kStatus_I3C_HdrParityError:
    case kStatus_I3C_CrcError:
    case kStatus_I3C_MsgError:
      return pw::Status::DataLoss();
    default:
      return pw::Status::Unknown();
  }
}

}  // namespace pw::i2c
