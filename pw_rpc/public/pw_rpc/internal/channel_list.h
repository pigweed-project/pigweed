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
#pragma once

#include <utility>

#include "pw_polyfill/language_feature_macros.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/config.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

// With dynamic allocation enabled, include the specified header and don't
// require the constructor to be constexpr.
#if PW_RPC_DYNAMIC_ALLOCATION

#include PW_RPC_DYNAMIC_CONTAINER_INCLUDE

#define _PW_RPC_CONSTEXPR
#define _PW_RPC_CONSTINIT

#else  // Otherwise, channels are stored in a constexpr constructible span.

#define _PW_RPC_CONSTEXPR constexpr
#define _PW_RPC_CONSTINIT PW_CONSTINIT

#endif  // PW_RPC_DYNAMIC_ALLOCATION

namespace pw::rpc::internal {

class ChannelList {
 public:
  _PW_RPC_CONSTEXPR ChannelList() = default;

  _PW_RPC_CONSTEXPR ChannelList(span<Channel> channels)
  // If dynamic allocation is enabled, channels aren't typically allocated
  // beforehand, though they can be. If they are, push them one-by-one to the
  // vector to avoid requiring a constructor that does that.
#if PW_RPC_DYNAMIC_ALLOCATION
  {
    for (const Channel& channel : channels) {
      channels_.emplace_back(channel);
    }
#else   // Without dynamic allocation, simply initialize the span.
      : channels_(channels) {
#endif  // PW_RPC_DYNAMIC_ALLOCATION
  }

  // Returns the first channel with the matching ID or nullptr if none match.
  // Except for Channel::kUnassignedChannelId, there should be no duplicate
  // channels.
  const Channel* Get(uint32_t channel_id) const;

  Channel* Get(uint32_t channel_id) {
    return const_cast<Channel*>(std::as_const(*this).Get(channel_id));
  }

  // Adds the channel with the requested ID to the list. Returns:
  //
  //   OK - the channel was added
  //   ALREADY_EXISTS - a channel with this ID is already present; remove it
  //       first
  //   RESOURCE_EXHAUSTED - no unassigned channels are available; only possible
  //       if PW_RPC_DYNAMIC_ALLOCATION is disabled
  //
  Status Add(uint32_t channel_id, ChannelOutput& output)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  // Sets the default channel output. Returns:
  //
  //   OK - the channel was added
  //   ALREADY_EXISTS - a default channel output is already present; remove it
  //       first
  //
  Status SetDefaultChannelOutput(ChannelOutput& output)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  // Removes the channel with the requested ID. Returns:
  //
  //   OK - the channel was removed
  //   NOT_FOUND - no channel with the provided ID was found
  //
  Status Remove(uint32_t channel_id) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

#if PW_RPC_DYNAMIC_ALLOCATION
  PW_RPC_DYNAMIC_CONTAINER(Channel) channels_;
#else
  span<Channel> channels_;
#endif  // PW_RPC_DYNAMIC_ALLOCATION

 private:
  Channel default_channel_ = Channel();
};

}  // namespace pw::rpc::internal
