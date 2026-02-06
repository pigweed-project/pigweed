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

#include <optional>

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/acl_data_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

L2capChannelManager::L2capChannelManager(AclDataChannel& acl_data_channel,
                                         Allocator& allocator)
    : acl_data_channel_(acl_data_channel), impl_(*this, allocator) {}

namespace internal {

L2capChannelManagerImpl::L2capChannelManagerImpl(L2capChannelManager& manager,
                                                 Allocator& allocator)
    : manager_(manager),
      allocator_(allocator),
      drain_task_(*this),
      lrd_channel_(manager.channels_by_local_cid_.end()),
      round_robin_terminus_(manager.channels_by_local_cid_.end()) {}

Status L2capChannelManagerImpl::Init(async2::Dispatcher& dispatcher) {
  if (dispatcher_ != nullptr) {
    return Status::FailedPrecondition();
  }

  auto result = async2::CreateMpscChannel<bool>(allocator_, 1);
  if (!result.has_value()) {
    return Status::ResourceExhausted();
  }

  std::tie(credit_handle_, credit_receiver_) = std::move(*result);
  credit_sender_ = credit_handle_.CreateSender();

  dispatcher_ = &dispatcher;
  dispatcher_thread_id_ = this_thread::get_id();
  dispatcher_->Post(drain_task_);

  return OkStatus();
}

async2::Sender<bool> L2capChannelManagerImpl::CreateCreditSender() {
  return credit_handle_.CreateSender();
}

bool L2capChannelManagerImpl::IsRunningOnDispatcherThread() const {
  return this_thread::get_id() == dispatcher_thread_id_;
}

void L2capChannelManagerImpl::OnRegister() {
  if (lrd_channel_ == manager_.channels_by_local_cid_.end()) {
    lrd_channel_ = manager_.channels_by_local_cid_.begin();
  }
  waker_.Wake();
}

// See L2capChannelManager::ReportNewTxPacketsOrCredits
void L2capChannelManagerImpl::ReportNewTxPacketsOrCredits() {
  credit_sender_.TrySend(true).IgnoreError();
}

void L2capChannelManagerImpl::DrainChannelQueuesIfNewTx() {
  // Actual work is done by the drain task and `DoDrainChannelQueuesIfNewTx`
  waker_.Wake();
}

async2::Poll<> L2capChannelManagerImpl::DoDrainChannelQueuesIfNewTx(
    async2::Context& context) {
  // No-op that satisifes lock annotations.
  std::lock_guard lock(channels_mutex_);

  if (round_robin_terminus_ == manager_.channels_by_local_cid_.end()) {
    round_robin_terminus_ = lrd_channel_;
  }

  while (true) {
    if (!credit_handle_.is_open()) {
      // ProxyHost has been reset; end this task.
      return async2::Ready();
    }

    if (!credit_future_.is_pendable()) {
      credit_future_ = credit_receiver_.Receive();
      round_robin_terminus_ = lrd_channel_;
    }

    if (credit_future_.Pend(context).IsReady()) {
      // We received a notification of additional tx credits. The future is no
      // longer pendable, so start a fresh iteration.
      continue;
    }

    // Container is empty, nothing to do.
    if (manager_.channels_by_local_cid_.empty()) {
      break;
    }

    L2capChannel::Handle& handle = *lrd_channel_;
    L2capChannel& channel = *handle;
    manager_.Advance(lrd_channel_);

    // If we can reserve a send credit for the channel's type, attempt to
    // dequeue a packet from channel. The credit is RAII, and will be returned
    // if not used to send a packet.
    //
    // If we have made it all the way around without dequeueing a packet or
    // receiving an notification of new credits, we are done for now.
    std::optional<AclDataChannel::SendCredit> credit =
        manager_.acl_data_channel_.ReserveSendCredit(handle->transport());
    if (!credit.has_value()) {
      if (lrd_channel_ == round_robin_terminus_) {
        break;
      }
      continue;
    }

    async2::Poll<std::optional<H4PacketWithH4>> packet_poll =
        channel.impl().DequeuePacket(context);
    if (packet_poll.IsReady() && packet_poll->has_value()) {
      // If we have a packet, send it and reset the terminus so round robin will
      // continue until we have done a full loop with no packets dequeued.
      PW_CHECK_OK(manager_.acl_data_channel_.SendAcl(std::move(**packet_poll),
                                                     std::move(*credit)));
      round_robin_terminus_ = lrd_channel_;
      continue;
    }

    // If no packet was returned and the channel is stale, no further packets
    // will be received and the channel can be removed.
    if (channel.IsStale()) {
      manager_.DeregisterChannelLocked(channel);
      channel.Close();
      allocator_.Delete(&channel);
    }

    // Done if we have made it all the way around.
    if (lrd_channel_ == round_robin_terminus_) {
      break;
    }
  }

  // We completed a full round robin without dequeuing any packets. This task
  // may be awoken by wakers set by `L2capChannelImpl::DequeuePacket`. It also
  // needs to be awoken if new channels are registered, so store that waker now.
  PW_ASYNC_STORE_WAKER(
      context, waker_, "Waiting for channels to be registered");
  return async2::Pending();
}

async2::Poll<> L2capChannelManagerImpl::DrainTask::DoPend(
    async2::Context& context) {
  return impl_.DoDrainChannelQueuesIfNewTx(context);
}

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
