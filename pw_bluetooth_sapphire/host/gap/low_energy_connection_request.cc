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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_request.h"

namespace bt::gap::internal {

LowEnergyConnectionRequest::LowEnergyConnectionRequest(
    PeerId peer_id,
    ConnectionResultCallback first_callback,
    LowEnergyConnectionOptions connection_options,
    Peer::InitializingConnectionToken peer_conn_token)
    : peer_id_(peer_id, MakeToStringInspectConvertFunction()),
      callbacks_(/*convert=*/[](const auto& cbs) {
        return static_cast<int64_t>(cbs.size());
      }),
      connection_options_(connection_options),
      peer_conn_token_(std::move(peer_conn_token)) {
  callbacks_.Mutable()->push_back(std::move(first_callback));
}

void LowEnergyConnectionRequest::NotifyCallbacks(
    fit::result<HostError, RefFunc> result) {
  peer_conn_token_.reset();

  for (const auto& callback : *callbacks_) {
    if (result.is_error()) {
      callback(fit::error(result.error_value()));
      continue;
    }
    auto conn_ref = result.value()();
    callback(fit::ok(std::move(conn_ref)));
  }
}

void LowEnergyConnectionRequest::AttachInspect(inspect::Node& parent,
                                               std::string name) {
  inspect_node_ = parent.CreateChild(name);
  peer_id_.AttachInspect(inspect_node_, "peer_id");
  callbacks_.AttachInspect(inspect_node_, "callbacks");
}

}  // namespace bt::gap::internal
