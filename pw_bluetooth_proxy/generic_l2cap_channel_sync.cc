// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0

#include <mutex>

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/generic_l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager_sync.h"

namespace pw::bluetooth::proxy::internal {

GenericL2capChannelImpl::GenericL2capChannelImpl(L2capChannel& channel)
    : channel_(&channel) {
  std::lock_guard lock(L2capChannelImpl::mutex());
  channel_->impl_.client_ = this;
}

GenericL2capChannelImpl& GenericL2capChannelImpl::operator=(
    GenericL2capChannelImpl&& other) {
  if (this == &other) {
    return *this;
  }

  std::lock_guard lock(L2capChannelImpl::mutex());
  if (channel_ != nullptr) {
    channel_->impl_.client_ = nullptr;
  }
  channel_ = std::exchange(other.channel_, nullptr);
  if (channel_ != nullptr) {
    channel_->impl_.client_ = this;
  }
  return *this;
}

GenericL2capChannelImpl::~GenericL2capChannelImpl() {
  std::lock_guard lock(L2capChannelImpl::mutex());
  if (channel_ != nullptr) {
    channel_->impl_.client_ = nullptr;
  }
}

Status GenericL2capChannelImpl::Init() { return OkStatus(); }

void GenericL2capChannelImpl::Start() {
  auto result = BorrowL2capChannel(L2capChannel::State::kNew);
  if (result.ok()) {
    BorrowedL2capChannel borrowed = std::move(*result);
    borrowed->Start();
  }
}

StatusWithMultiBuf GenericL2capChannelImpl::Write(
    multibuf::MultiBuf&& payload) {
  auto result = BorrowL2capChannel();
  if (!result.ok()) {
    return {result.status(), std::move(payload)};
  }
  BorrowedL2capChannel borrowed = std::move(*result);
  return borrowed->Write(std::move(payload));
}

Status GenericL2capChannelImpl::IsWriteAvailable() {
  PW_TRY_ASSIGN(BorrowedL2capChannel borrowed, BorrowL2capChannel());
  return borrowed->impl().IsWriteAvailable();
}

Status GenericL2capChannelImpl::SendAdditionalRxCredits(
    uint16_t additional_rx_credits) {
  PW_TRY_ASSIGN(BorrowedL2capChannel borrowed, BorrowL2capChannel());
  return borrowed->SendAdditionalRxCredits(additional_rx_credits);
}

void GenericL2capChannelImpl::Stop() {
  auto result = BorrowL2capChannel();
  if (result.ok()) {
    BorrowedL2capChannel borrowed = std::move(*result);
    borrowed->Stop();
  }
}

void GenericL2capChannelImpl::Close() {
  auto result = BorrowL2capChannel();
  if (result.ok()) {
    BorrowedL2capChannel borrowed = std::move(*result);
    borrowed->Close();
  }
}

L2capChannel* GenericL2capChannelImpl::InternalForTesting() const {
  std::lock_guard lock(L2capChannelImpl::mutex());
  return channel_;
}

Result<BorrowedL2capChannel> GenericL2capChannelImpl::BorrowL2capChannel(
    L2capChannel::State expected) const {
  std::lock_guard lock(L2capChannelImpl::mutex());
  if (channel_ == nullptr || channel_->state() != expected) {
    return Status::FailedPrecondition();
  }
  return BorrowedL2capChannel(channel_->impl_);
}

}  // namespace pw::bluetooth::proxy::internal

#endif  // PW_BLUETOOTH_PROXY_ASYNC
