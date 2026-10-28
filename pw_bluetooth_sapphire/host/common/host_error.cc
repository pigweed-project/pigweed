// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"

namespace bt {

std::string HostErrorToString(HostError error) {
  switch (error) {
    case HostError::kNotFound:
      return "not found";
    case HostError::kNotReady:
      return "not ready";
    case HostError::kTimedOut:
      return "timed out";
    case HostError::kInvalidParameters:
      return "invalid parameters";
    case HostError::kParametersRejected:
      return "parameters rejected";
    case HostError::kAdvertisingDataTooLong:
      return "advertising data too long";
    case HostError::kScanResponseTooLong:
      return "scan response too long";
    case HostError::kCanceled:
      return "canceled";
    case HostError::kInProgress:
      return "in progress";
    case HostError::kNotSupported:
      return "not supported";
    case HostError::kPacketMalformed:
      return "packet malformed";
    case HostError::kLinkDisconnected:
      return "link disconnected";
    case HostError::kOutOfMemory:
      return "out of memory";
    case HostError::kInsufficientSecurity:
      return "insufficient security";
    case HostError::kNotReliable:
      return "not reliable";
    case HostError::kTooManyResults:
      return "too many results";
    case HostError::kFailed:
      return "failed";
  }
  return "(unknown)";
}

}  // namespace bt
