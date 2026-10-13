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

#include "pw_bluetooth_sapphire/internal/host/l2cap/dynamic_channel_registry.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"

namespace bt::l2cap::internal {
// Run return callbacks on the L2CAP thread. LogicalLink takes care of out-of-
// thread dispatch for delivering the pointer to the channel.
void DynamicChannelRegistry::OpenOutbound(Psm psm,
                                          ChannelParameters params,
                                          DynamicChannelCallback open_cb) {
  const ChannelId id = FindAvailableChannelId();
  if (id == kInvalidChannelId) {
    bt_log(ERROR, "l2cap", "No dynamic channel IDs available");
    open_cb(nullptr);
    return;
  }

  auto iter = channels_.emplace(id, MakeOutbound(psm, id, params)).first;
  ActivateChannel(iter->second.get(), std::move(open_cb), /*pass_failed=*/true);
}

void DynamicChannelRegistry::CloseChannel(ChannelId local_cid,
                                          fit::closure close_callback) {
  DynamicChannel* channel = FindChannelByLocalId(local_cid);
  if (!channel) {
    close_callback();
    return;
  }

  PW_DCHECK(channel->IsConnected());
  auto disconn_done_cb =
      [self = GetWeakPtr(), close_cb = std::move(close_callback), channel] {
        if (!self.is_alive()) {
          close_cb();
          return;
        }
        self->RemoveChannel(channel);
        close_cb();
      };
  channel->Disconnect(std::move(disconn_done_cb));
}

DynamicChannelRegistry::DynamicChannelRegistry(
    uint16_t max_num_channels,
    DynamicChannelCallback close_cb,
    ServiceRequestCallback service_request_cb,
    bool random_channel_ids)
    : WeakSelf(this),
      max_num_channels_(max_num_channels),
      close_cb_(std::move(close_cb)),
      service_request_cb_(std::move(service_request_cb)),
      random_channel_ids_(random_channel_ids) {
  PW_DCHECK(max_num_channels > 0);
  PW_DCHECK(max_num_channels < 65473);
  PW_DCHECK(close_cb_);
  PW_DCHECK(service_request_cb_);
}

DynamicChannel* DynamicChannelRegistry::RequestService(Psm psm,
                                                       ChannelId local_cid,
                                                       ChannelId remote_cid) {
  PW_DCHECK(local_cid != kInvalidChannelId);

  auto service_info = service_request_cb_(psm);
  if (!service_info) {
    bt_log(WARN,
           "l2cap",
           "No service found for PSM %#.4x from %#.4x",
           psm,
           remote_cid);
    return nullptr;
  }

  auto iter =
      channels_
          .emplace(
              local_cid,
              MakeInbound(
                  psm, local_cid, remote_cid, service_info->channel_params))
          .first;
  ActivateChannel(iter->second.get(),
                  std::move(service_info->channel_cb),
                  /*pass_failed=*/false);
  return iter->second.get();
}

ChannelId DynamicChannelRegistry::FindAvailableChannelId() {
  uint16_t offset = 0;
  if (random_channel_ids_) {
    random_generator()->GetInt(offset, max_num_channels_);
  }
  for (uint16_t i = 0; i < max_num_channels_; i++) {
    ChannelId id = kFirstDynamicChannelId + ((offset + i) % max_num_channels_);
    if (channels_.count(id) == 0) {
      return id;
    }
  }

  return kInvalidChannelId;
}

size_t DynamicChannelRegistry::AliveChannelCount() const {
  return channels_.size();
}

DynamicChannel* DynamicChannelRegistry::FindChannelByLocalId(
    ChannelId local_cid) const {
  auto iter = channels_.find(local_cid);
  if (iter == channels_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

DynamicChannel* DynamicChannelRegistry::FindChannelByRemoteId(
    ChannelId remote_cid) const {
  for (auto& [id, channel_ptr] : channels_) {
    if (channel_ptr->remote_cid() == remote_cid) {
      return channel_ptr.get();
    }
  }
  return nullptr;
}

void DynamicChannelRegistry::ForEach(
    fit::function<void(DynamicChannel*)> f) const {
  for (auto iter = channels_.begin(); iter != channels_.end();) {
    // f() may remove the channel from the registry, so get next iterator to
    // avoid invalidation. Only the erased iterator is invalidated.
    auto next = std::next(iter);
    f(iter->second.get());
    iter = next;
  }
}

void DynamicChannelRegistry::ActivateChannel(
    DynamicChannel* channel,
    DynamicChannelCallback open_callback,
    bool pass_failed) {
  // It's safe to capture |this| here because the callback will be owned by the
  // DynamicChannel, which this registry owns.
  auto return_chan = [this,
                      channel,
                      open_cb = std::move(open_callback),
                      pass_failed]() mutable {
    if (channel->IsOpen()) {
      channel->set_opened();
      open_cb(channel);
      return;
    }

    bt_log(DEBUG,
           "l2cap",
           "Failed to open dynamic channel %#.4x (remote %#.4x) for PSM %#.4x",
           channel->local_cid(),
           channel->remote_cid(),
           channel->psm());

    // TODO(fxbug.dev/42057179): Maybe negotiate channel parameters here?
    // For now, just disconnect the channel. Move the callback to the stack
    // to prepare for channel destruction.
    auto pass_failure = [cb = std::move(open_cb), pass_failed] {
      if (pass_failed) {
        cb(nullptr);
      }
    };

    // This lambda is owned by the channel, so captures are no longer valid
    // after this call.
    auto disconn_done_cb = [self = GetWeakPtr(), channel] {
      if (!self.is_alive()) {
        return;
      }
      self->RemoveChannel(channel);
    };
    channel->Disconnect(std::move(disconn_done_cb));

    pass_failure();
  };

  channel->Open(std::move(return_chan));
}

void DynamicChannelRegistry::OnChannelDisconnected(DynamicChannel* channel) {
  if (channel->opened()) {
    close_cb_(channel);
  }
  RemoveChannel(channel);
}

void DynamicChannelRegistry::RemoveChannel(DynamicChannel* channel) {
  PW_DCHECK(channel);
  PW_DCHECK(!channel->IsConnected());

  auto iter = channels_.find(channel->local_cid());
  if (iter == channels_.end()) {
    return;
  }

  if (channel != iter->second.get()) {
    return;
  }

  channels_.erase(iter);
}

}  // namespace bt::l2cap::internal
