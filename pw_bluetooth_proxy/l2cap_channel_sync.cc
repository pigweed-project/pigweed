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
#include "pw_log/log.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy::internal {

sync::Mutex L2capChannelImpl::static_mutex_;

BorrowedL2capChannel::BorrowedL2capChannel(L2capChannelImpl& impl)
    : impl_(&impl) {
  std::lock_guard lock(impl_->mutex_);
  ++impl_->num_borrows_;
  PW_CHECK_UINT_NE(impl_->num_borrows_, 0);
}

BorrowedL2capChannel& BorrowedL2capChannel::operator=(
    BorrowedL2capChannel&& other) {
  if (this != &other) {
    std::swap(impl_, other.impl_);
  }
  return *this;
}

BorrowedL2capChannel::~BorrowedL2capChannel() {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard lock(impl_->mutex_);
  PW_CHECK_UINT_NE(impl_->num_borrows_, 0);
  if (--impl_->num_borrows_ == 0) {
    impl_->notification_.release();
  }
}

L2capChannel* BorrowedL2capChannel::operator->() { return &impl_->channel_; }

L2capChannel& BorrowedL2capChannel::operator*() { return impl_->channel_; }

void L2capChannelImpl::BlockWhileBorrowed(
    std::unique_lock<internal::Mutex>& lock) PW_NO_LOCK_SAFETY_ANALYSIS {
  while (num_borrows_ != 0) {
    lock.unlock();
    notification_.acquire();
    lock.lock();
  }
}

void L2capChannelImpl::Close() {
  std::lock_guard lock(L2capChannelImpl::mutex());
  if (client_.has_value() && *client_ != nullptr) {
    (*client_)->channel_ = nullptr;
  }
  // Always set to null to mark channel as stale, even if it was not acquired.
  client_ = nullptr;
}

StatusWithMultiBuf L2capChannelImpl::Write(multibuf::MultiBuf&& payload)
    PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(mutex_);
  if (channel_.state_ != L2capChannel::State::kRunning) {
    PW_LOG_WARN(
        "btproxy: L2capChannel::Write called when not running. "
        "local_cid: %#x, remote_cid: %#x, state: %u",
        channel_.local_cid(),
        channel_.remote_cid(),
        cpp23::to_underlying(channel_.state_));
    return {Status::FailedPrecondition(), std::move(payload)};
  }

  if (payload_queue_.full()) {
    notify_on_dequeue_ = true;
    return {Status::Unavailable(), std::move(payload)};
  }
  payload_queue_.push(std::move(payload));
  channel_.ReportNewTxPacketsOrCredits();
  return {OkStatus(), std::nullopt};
}

Status L2capChannelImpl::IsWriteAvailable() PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(mutex_);
  if (channel_.state_ != L2capChannel::State::kRunning) {
    return Status::FailedPrecondition();
  }

  if (payload_queue_.full()) {
    notify_on_dequeue_ = true;
    return Status::Unavailable();
  }

  notify_on_dequeue_ = false;
  return OkStatus();
}

std::optional<H4PacketWithH4> L2capChannelImpl::DequeuePacket()
    PW_NO_LOCK_SAFETY_ANALYSIS {
  std::optional<H4PacketWithH4> packet;
  bool should_notify = false;
  {
    // Get a payload.
    std::lock_guard lock(mutex_);
    if (channel_.state_ != L2capChannel::State::kRunning) {
      payload_queue_.clear();
    }
    if (payload_queue_.empty()) {
      return std::nullopt;
    }
    const multibuf::MultiBuf& payload = payload_queue_.front();

    // Create a packet from the payload.
    bool keep_payload = false;
    packet = channel_.GenerateNextTxPacket(payload, keep_payload);

    // If no additional packets can be created from the payload, remove it.
    if (!keep_payload) {
      payload_queue_.pop();
      should_notify = notify_on_dequeue_;
      notify_on_dequeue_ = false;
    }
  }

  // Notify the client if there is now room in the queue.
  if (should_notify) {
    SendEvent(L2capChannelEvent::kWriteAvailable);
  }

  if (packet.has_value()) {
    channel_.NotifyCreditMutation();
  }

  return packet;
}

void L2capChannelImpl::ReportNewTxPacketsOrCredits() {
  channel_.l2cap_channel_manager_.ReportNewTxPacketsOrCredits();
}

void L2capChannelImpl::ClearQueue() { payload_queue_.clear(); }

bool L2capChannelImpl::IsStale() const {
  std::lock_guard lock(L2capChannelImpl::mutex());
  return client_.has_value() && *client_ == nullptr;
}

bool L2capChannelImpl::PayloadQueueEmpty() const {
  std::lock_guard lock(mutex_);
  return payload_queue_.empty();
}

void L2capChannelImpl::SendEvent(L2capChannelEvent event) {
  {
    std::lock_guard lock(L2capChannelImpl::mutex());
    if (!client_.has_value() || *client_ == nullptr) {
      return;
    }
  }

  // We don't log kWriteAvailable since they happen often. Optimally we would
  // just debug log them also, but one of our downstreams logs all levels.
  if (event != L2capChannelEvent::kWriteAvailable) {
    PW_LOG_INFO(
        "btproxy: L2capChannel::SendEvent -  connection: %#x, "
        "local_cid: %#x, event: %u,",
        channel_.connection_handle(),
        channel_.local_cid(),
        cpp23::to_underlying(event));
  }

  if (channel_.event_fn_) {
    channel_.event_fn_(event);
  }
}

}  // namespace pw::bluetooth::proxy::internal

#endif  // PW_BLUETOOTH_PROXY_ASYNC
