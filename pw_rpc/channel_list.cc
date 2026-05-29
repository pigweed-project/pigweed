// Copyright 2022 The Pigweed Authors
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

#include "pw_rpc/internal/channel_list.h"

#include <cstdint>

#include "pw_rpc/channel.h"
#include "pw_status/status.h"

namespace pw::rpc::internal {
namespace {

// Since `Get` returns a `Channel` object and not a `ChannelBase` object, we
// must associate a channel ID with the default channel output, even though it
// will never be used. Any real channels can have the same ID and it will not
// cause issues.
constexpr uint32_t kDefaultChannelOutputChannelId = 1;
}  // namespace

const Channel* ChannelList::Get(uint32_t channel_id) const {
  for (const Channel& channel : channels_) {
    if (channel.id() == channel_id) {
      return &channel;
    }
  }

  if (default_channel_.assigned()) {
    return &default_channel_;
  }

  return nullptr;
}

Status ChannelList::Add(uint32_t channel_id, ChannelOutput& output) {
  ScopedChannelModificationLock lock;

  if (Get(channel_id) != nullptr) {
    return Status::AlreadyExists();
  }

#if PW_RPC_DYNAMIC_ALLOCATION
  channels_.emplace_back(Channel(channel_id, &output));
#else
  Channel* new_channel = Get(Channel::kUnassignedChannelId);
  if (new_channel == nullptr) {
    return Status::ResourceExhausted();
  }

  new_channel->Configure(channel_id, output);
#endif  // PW_RPC_DYNAMIC_ALLOCATION

  return OkStatus();
}

Status ChannelList::SetDefaultChannelOutput(ChannelOutput& output) {
  ScopedChannelModificationLock lock;

  if (default_channel_.assigned()) {
    return Status::AlreadyExists();
  }

  default_channel_ = Channel::Create<kDefaultChannelOutputChannelId>(&output);

  return OkStatus();
}

Status ChannelList::Remove(uint32_t channel_id) {
  ScopedChannelModificationLock lock;

  Channel* channel = Get(channel_id);

  if (channel == nullptr) {
    return Status::NotFound();
  }
  channel->Close();

#if PW_RPC_DYNAMIC_ALLOCATION
  // Order isn't important, so move the channel to the back then pop it.
  std::swap(*channel, channels_.back());
  channels_.pop_back();
#endif  // PW_RPC_DYNAMIC_ALLOCATION

  return OkStatus();
}

}  // namespace pw::rpc::internal
