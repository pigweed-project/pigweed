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

#include "pw_bluetooth_proxy/rfcomm/rfcomm_channel_manager_interface.h"

namespace pw::bluetooth::proxy::rfcomm {

RfcommChannel::RfcommChannel(RfcommChannel&& other) noexcept
    : connection_handle_(other.connection_handle_),
      channel_number_(other.channel_number_),
      manager_(other.manager_) {
  other.manager_ = nullptr;
}

RfcommChannel& RfcommChannel::operator=(RfcommChannel&& other) noexcept {
  if (this != &other) {
    Reset();
    connection_handle_ = other.connection_handle_;
    channel_number_ = other.channel_number_;
    manager_ = other.manager_;
    other.manager_ = nullptr;
  }
  return *this;
}

RfcommChannel::~RfcommChannel() { Reset(); }

StatusWithMultiBuf RfcommChannel::Write(FlatConstMultiBuf&& payload) {
  if (!manager_) {
    return {Status::FailedPrecondition(), std::move(payload)};
  }
  return manager_->Write(
      connection_handle_, channel_number_, std::move(payload));
}

RfcommChannel::RfcommChannel(ConnectionHandle connection_handle,
                             uint8_t channel_number,
                             RfcommChannelManagerInterface* manager)
    : connection_handle_(connection_handle),
      channel_number_(channel_number),
      manager_(manager) {}

void RfcommChannel::Reset() {
  if (manager_) {
    static_cast<void>(
        manager_->ReleaseRfcommChannel(connection_handle_, channel_number_));
  }
  manager_ = nullptr;
}

}  // namespace pw::bluetooth::proxy::rfcomm
