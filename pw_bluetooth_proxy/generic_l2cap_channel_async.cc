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

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/generic_l2cap_channel_async.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager_async.h"

namespace pw::bluetooth::proxy::internal {

GenericL2capChannelImpl::GenericL2capChannelImpl(L2capChannel& channel)
    : GenericL2capChannelImpl(channel, channel.l2cap_channel_manager_.impl()) {}

GenericL2capChannelImpl::GenericL2capChannelImpl(
    L2capChannel& channel, L2capChannelManagerImpl& manager)
    : channel_(&channel),
      dispatcher_(&manager.dispatcher()),
      dispatcher_thread_id_(manager.dispatcher_thread_id()) {}

GenericL2capChannelImpl& GenericL2capChannelImpl::operator=(
    GenericL2capChannelImpl&& other) {
  if (this == &other) {
    return *this;
  }

  channel_ = std::exchange(other.channel_, nullptr);
  dispatcher_ = std::exchange(other.dispatcher_, nullptr);
  dispatcher_thread_id_ = std::move(other.dispatcher_thread_id_);

  request_handle_ = std::move(other.request_handle_);
  request_sender_ = std::move(other.request_sender_);

  payload_sender_ = std::move(other.payload_sender_);
  send_reservation_ = std::move(other.send_reservation_);

  return *this;
}

GenericL2capChannelImpl::~GenericL2capChannelImpl() {
  if (request_handle_.is_open()) {
    request_handle_.Close();
  }
}

Status GenericL2capChannelImpl::Init() {
  PW_TRY(channel_->impl().Connect(request_handle_, request_sender_));
  channel_->impl().Connect(payload_sender_);
  return OkStatus();
}

void GenericL2capChannelImpl::Start() { channel_->Start(); }

StatusWithMultiBuf GenericL2capChannelImpl::Write(
    multibuf::MultiBuf&& payload) {
  if (auto status = IsWriteAvailable(); !status.ok()) {
    return {status, std::move(payload)};
  }
  send_reservation_->Commit(std::move(payload));
  send_reservation_.reset();
  return {OkStatus(), std::nullopt};
}

Status GenericL2capChannelImpl::IsWriteAvailable() {
  if (!payload_sender_.is_open()) {
    return Status::FailedPrecondition();
  }
  if (send_reservation_.has_value()) {
    return OkStatus();
  }
  auto result = payload_sender_.TryReserveSend();
  if (result.ok()) {
    send_reservation_ = std::move(*result);
    return OkStatus();
  }
  if (result.status() != Status::Unavailable()) {
    return result.status();
  }
  if (this_thread::get_id() == dispatcher_thread_id_) {
    channel_->impl().NotifyOnDequeue();
  } else {
    Request request;
    request.type = Request::Type::kNotifyOnDequeue;
    PW_TRY(Send(std::move(request)));
  }
  return Status::Unavailable();
}

Status GenericL2capChannelImpl::SendAdditionalRxCredits(
    uint16_t additional_rx_credits) {
  if (!request_handle_.is_open()) {
    return Status::FailedPrecondition();
  }
  if (this_thread::get_id() == dispatcher_thread_id_) {
    return channel_->SendAdditionalRxCredits(additional_rx_credits);
  }
  Request request;
  request.type = Request::Type::kSendAdditionalRxCredits;
  request.additional_rx_credits = additional_rx_credits;
  return Send(std::move(request));
}

void GenericL2capChannelImpl::Stop() {
  if (this_thread::get_id() == dispatcher_thread_id_) {
    return channel_->Stop();
  }
  Request request;
  request.type = Request::Type::kStop;
  Send(std::move(request)).IgnoreError();
}

void GenericL2capChannelImpl::Close() {
  if (this_thread::get_id() == dispatcher_thread_id_) {
    return channel_->Close();
  }
  Request request;
  request.type = Request::Type::kClose;
  Send(std::move(request)).IgnoreError();
}

L2capChannel* GenericL2capChannelImpl::InternalForTesting() {
  return request_handle_.is_open() ? channel_ : nullptr;
}

Status GenericL2capChannelImpl::Send(Request&& request) const {
  PW_TRY(request_sender_.BlockingSend(*dispatcher_, std::move(request)));
  return OkStatus();
}

}  // namespace pw::bluetooth::proxy::internal

#endif  // PW_BLUETOOTH_PROXY_ASYNC
