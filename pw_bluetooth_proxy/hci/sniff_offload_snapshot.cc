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

#include "pw_bluetooth_proxy/hci/sniff_offload_snapshot.h"

namespace pw::bluetooth::proxy::hci {

bool SniffConnectionSnapshot::MatchesKey(uint16_t handle) const {
  return connection_handle == handle;
}

Status SniffConnectionSnapshot::Update(const SniffConnectionSnapshot& update) {
  if (!MatchesKey(update.connection_handle)) {
    return Status::InvalidArgument();
  }
  max_interval = update.max_interval;
  min_interval = update.min_interval;
  attempt = update.attempt;
  timeout = update.timeout;
  link_inactivity_timeout = update.link_inactivity_timeout;
  subrating_max_latency = update.subrating_max_latency;
  subrating_min_remote_timeout = update.subrating_min_remote_timeout;
  subrating_min_local_timeout = update.subrating_min_local_timeout;
  allow_exit_sniff_on_rx = update.allow_exit_sniff_on_rx;
  allow_exit_sniff_on_tx = update.allow_exit_sniff_on_tx;
  return OkStatus();
}

Status SniffSnapshot::Update(const SniffConnectionSnapshot& update) {
  for (SniffConnectionSnapshot& connection : connections) {
    if (connection.MatchesKey(update.connection_handle)) {
      return connection.Update(update);
    }
  }

  if (connections.full()) {
    snapshot_incomplete = true;
    return Status::ResourceExhausted();
  }
  connections.push_back(update);
  return OkStatus();
}

Status SniffSnapshot::ApplyStateUpdate(const SniffStateUpdate& update) {
  if (const SniffSnapshot* global = std::get_if<SniffSnapshot>(&update)) {
    *this = *global;
    return OkStatus();
  }
  if (const SniffConnectionSnapshot* conn =
          std::get_if<SniffConnectionSnapshot>(&update)) {
    return Update(*conn);
  }
  return Status::InvalidArgument();
}

}  // namespace pw::bluetooth::proxy::hci
