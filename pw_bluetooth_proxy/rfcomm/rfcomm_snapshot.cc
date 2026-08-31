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

#include "pw_bluetooth_proxy/rfcomm/rfcomm_snapshot.h"

namespace pw::bluetooth::proxy::rfcomm {

bool RfcommChannelSnapshot::MatchesKey(uint16_t handle,
                                       uint8_t channel,
                                       RfcommDirection dir) const {
  return connection_handle == handle && channel_number == channel &&
         direction == dir;
}

bool RfcommChannelSnapshot::MatchesKey(
    const RfcommChannelRemoved& removal) const {
  return MatchesKey(
      removal.connection_handle, removal.channel_number, removal.direction);
}

Status RfcommChannelSnapshot::Update(const RfcommChannelSnapshot& update) {
  if (!MatchesKey(
          update.connection_handle, update.channel_number, update.direction)) {
    return Status::InvalidArgument();
  }
  local_cid = update.local_cid;
  remote_cid = update.remote_cid;
  mux_initiator = update.mux_initiator;
  tx_credits = update.tx_credits;
  rx_credits = update.rx_credits;
  rx_total_credits = update.rx_total_credits;
  max_frame_size = update.max_frame_size;
  return OkStatus();
}

Status RfcommSnapshot::ApplyStateUpdate(const RfcommStateUpdate& update) {
  return std::visit(
      [this](const auto& arg) -> Status {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, RfcommChannelSnapshot>) {
          for (RfcommChannelSnapshot& channel : rfcomm_channels) {
            if (channel.MatchesKey(
                    arg.connection_handle, arg.channel_number, arg.direction)) {
              return channel.Update(arg);
            }
          }

          if (rfcomm_channels.full()) {
            snapshot_incomplete = true;
            return Status::ResourceExhausted();
          }
          rfcomm_channels.push_back(arg);
          return OkStatus();
        } else if constexpr (std::is_same_v<T, RfcommChannelRemoved>) {
          for (auto it = rfcomm_channels.begin(); it != rfcomm_channels.end();
               ++it) {
            if (it->MatchesKey(arg)) {
              rfcomm_channels.erase(it);
              return OkStatus();
            }
          }
          return OkStatus();
        }
      },
      update);
}

}  // namespace pw::bluetooth::proxy::rfcomm
