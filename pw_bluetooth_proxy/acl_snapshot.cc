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

#include "pw_bluetooth_proxy/acl_snapshot.h"

namespace pw::bluetooth::proxy {

bool AclConnectionSnapshot::MatchesKey(uint16_t handle) const {
  return connection_handle == handle;
}

Status AclConnectionSnapshot::Update(const AclConnectionSnapshot& update) {
  if (!MatchesKey(update.connection_handle)) {
    return Status::InvalidArgument();
  }
  transport = update.transport;
  num_proxy_pending_packets = update.num_proxy_pending_packets;
  num_host_pending_packets = update.num_host_pending_packets;
  num_queued_host_packets = update.num_queued_host_packets;
  return OkStatus();
}

Status AclSnapshot::ApplyStateUpdate(const AclStateUpdate& update) {
  for (AclConnectionSnapshot& connection : acl_connections) {
    if (connection.MatchesKey(update.connection.connection_handle)) {
      return connection.Update(update.connection);
    }
  }

  if (acl_connections.full()) {
    snapshot_incomplete = true;
    return Status::ResourceExhausted();
  }
  acl_connections.push_back(update.connection);
  return OkStatus();
}

}  // namespace pw::bluetooth::proxy
