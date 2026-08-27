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

#include "pw_bluetooth_proxy/l2cap_snapshot.h"

namespace pw::bluetooth::proxy {

bool L2capSignalingStateSnapshot::MatchesKey(uint16_t handle) const {
  return connection_handle == handle;
}

Status L2capSignalingStateSnapshot::Update(
    const L2capSignalingStateSnapshot& update) {
  if (!MatchesKey(update.connection_handle)) {
    return Status::InvalidArgument();
  }
  transport = update.transport;
  next_identifier = update.next_identifier;
  return OkStatus();
}

bool L2capChannelSnapshot::MatchesKey(uint16_t handle, uint16_t cid) const {
  return connection_handle == handle && local_cid == cid;
}

bool L2capChannelSnapshot::MatchesKey(
    const L2capChannelRemoved& removal) const {
  return MatchesKey(removal.connection_handle, removal.local_cid);
}

Status L2capChannelSnapshot::Update(const L2capChannelSnapshot& update) {
  if (!MatchesKey(update.connection_handle, update.local_cid)) {
    return Status::InvalidArgument();
  }
  remote_cid = update.remote_cid;
  transport = update.transport;
  mode = update.mode;
  acl_recombination_in_progress = update.acl_recombination_in_progress;
  allow_data_loss = update.allow_data_loss;
  rx_engine = update.rx_engine;
  tx_engine = update.tx_engine;
  return OkStatus();
}

Status L2capSnapshot::ApplyStateUpdate(const L2capStateUpdate& update) {
  return std::visit(
      [this](const auto& arg) -> Status {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, L2capSignalingStateSnapshot>) {
          for (L2capSignalingStateSnapshot& state : l2cap_signaling_states) {
            if (state.MatchesKey(arg.connection_handle)) {
              return state.Update(arg);
            }
          }

          if (l2cap_signaling_states.full()) {
            snapshot_incomplete = true;
            return Status::ResourceExhausted();
          }
          l2cap_signaling_states.push_back(arg);
          return OkStatus();
        } else if constexpr (std::is_same_v<T, L2capChannelSnapshot>) {
          for (L2capChannelSnapshot& channel : l2cap_channels) {
            if (channel.MatchesKey(arg.connection_handle, arg.local_cid)) {
              return channel.Update(arg);
            }
          }

          if (l2cap_channels.full()) {
            snapshot_incomplete = true;
            return Status::ResourceExhausted();
          }
          l2cap_channels.push_back(arg);
          return OkStatus();
        } else if constexpr (std::is_same_v<T, L2capChannelRemoved>) {
          for (auto it = l2cap_channels.begin(); it != l2cap_channels.end();
               ++it) {
            if (it->MatchesKey(arg)) {
              l2cap_channels.erase(it);
              return OkStatus();
            }
          }
          // If the channel was not found, nothing needs to be done.
          return OkStatus();
        }
      },
      update);
}

}  // namespace pw::bluetooth::proxy
