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
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_containers/flat_map.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"

namespace pw::bluetooth::proxy {

L2capChannelManager::L2capChannelManager(AclDataChannel& acl_data_channel,
                                         Allocator& allocator)
    : acl_data_channel_(acl_data_channel), impl_(*this, allocator) {}

namespace internal {

L2capChannelManagerImpl::L2capChannelManagerImpl(L2capChannelManager& manager,
                                                 Allocator& allocator)
    : manager_(manager),
      allocator_(allocator),
      lrd_channel_(manager.channels_by_local_cid_.end()),
      round_robin_terminus_(manager.channels_by_local_cid_.end()) {}

L2capChannelManagerImpl::~L2capChannelManagerImpl() {
  // Should be cleared by the ProxyHost dtor.
  PW_DCHECK(manager_.channels_by_local_cid_.empty());
  PW_DCHECK(manager_.channels_by_remote_cid_.empty());
}

void L2capChannelManagerImpl::OnRegister() {
  if (lrd_channel_ == manager_.channels_by_local_cid_.end()) {
    lrd_channel_ = manager_.channels_by_local_cid_.begin();
  }
}

void L2capChannelManagerImpl::ReportNewTxPacketsOrCredits() {
  std::lock_guard lock(drain_status_mutex_);
  drain_needed_ = true;
}

void L2capChannelManagerImpl::DrainChannelQueuesIfNewTx() {
  {
    std::lock_guard lock(drain_status_mutex_);
    if (drain_running_) {
      // Drain is already in progress
      return;
    }
    if (!drain_needed_) {
      return;
    }
    drain_running_ = true;
    drain_needed_ = false;
  }
  pw::containers::FlatMap<AclTransportType,
                          std::optional<AclDataChannel::SendCredit>,
                          2>
      credits({{{AclTransportType::kBrEdr, {}}, {AclTransportType::kLe, {}}}});
  for (;;) {
    std::optional<H4PacketWithH4> packet;
    std::optional<AclDataChannel::SendCredit> packet_credit{};
    // Attempt to reserve credits. This may be our first pass or we may have
    // used one on last pass.
    // We reserve credits upfront so that acl_data_channel_'s credits mutex lock
    // is not acquired inside the channels_mutex() lock below.
    // SendCredit is RAII object, any held credits will be returned when
    // function exits.
    for (auto& [transport, credit] : credits) {
      if (!credit.has_value()) {
        credits.at(transport) =
            manager_.acl_data_channel_.ReserveSendCredit(transport);
      }
    }
    L2capChannel* channel = nullptr;
    {
      std::lock_guard lock(channels_mutex_);

      // Container is empty, nothing to do.
      if (lrd_channel_ == manager_.channels_by_local_cid_.end()) {
        // No channels, no drain needed.
        std::lock_guard drain_lock(drain_status_mutex_);
        drain_needed_ = false;
        drain_running_ = false;
        return;
      }

      // If terminus is unset, use the least recently drained container.
      if (round_robin_terminus_ == manager_.channels_by_local_cid_.end()) {
        round_robin_terminus_ = lrd_channel_;
      }

      // If we have a credit for the channel's type, attempt to dequeue
      // packet from channel.
      channel = &**lrd_channel_;
      std::optional<AclDataChannel::SendCredit>& current_credit =
          credits.at(channel->transport());
      if (current_credit.has_value()) {
        packet = channel->impl_.DequeuePacket();
      }

      if (packet) {
        // We were able to dequeue a packet. So also take the current credit
        // to use when sending the packet below.
        packet_credit = std::exchange(current_credit, std::nullopt);
        round_robin_terminus_ = manager_.channels_by_local_cid_.end();
      }
    }  // lock_guard: channels_mutex_

    if (packet) {
      // A packet with a credit was found inside the lock. Send while unlocked
      // with that credit.
      // This will trigger another Drain when `packet` is released. This could
      // happen during the SendAcl call, but that is fine because `lrd_channel_`
      // and `round_robin_terminus_` are always adjusted inside the lock. So
      // each Drain frame's loop will just resume where last one left off and
      // continue until that it has found no channels with something to dequeue.
      PW_CHECK_OK(manager_.acl_data_channel_.SendAcl(
          std::move(*packet),
          std::move(std::exchange(packet_credit, std::nullopt).value())));
      continue;
    }
    {
      std::lock_guard channels_lock(channels_mutex_);

      if (channel->impl_.IsStale() && channel->impl_.PayloadQueueEmpty()) {
        // The channel is stale and its queue is empty, so it can be removed.
        manager_.DeregisterChannelLocked(*channel);
        channel->Close();
        allocator_.Delete(channel);
        continue;
      }

      // Always advance so next dequeue is from next channel.
      manager_.Advance(lrd_channel_);

      std::lock_guard drain_lock(drain_status_mutex_);

      if (drain_needed_) {
        // Additional tx packets or resources have arrived, so reset terminus so
        // we attempt to dequeue all the channels again.
        round_robin_terminus_ = manager_.channels_by_local_cid_.end();
        drain_needed_ = false;
        continue;
      }

      if (lrd_channel_ != round_robin_terminus_) {
        // Continue until last drained channel is terminus, meaning we have
        // failed to dequeue from all channels (so nothing left to send).
        continue;
      }

      drain_running_ = false;
      return;
    }  // lock_guard: channels_mutex_, drain_status_mutex_
  }
}

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
