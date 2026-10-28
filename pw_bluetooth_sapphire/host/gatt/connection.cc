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

#include "pw_bluetooth_sapphire/internal/host/gatt/connection.h"

#include <pw_assert/check.h>

#include <numeric>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/client.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/server.h"

namespace bt::gatt::internal {

Connection::Connection(std::unique_ptr<Client> client,
                       std::unique_ptr<Server> server,
                       RemoteServiceWatcher svc_watcher)
    : server_(std::move(server)), weak_self_(this) {
  PW_CHECK(svc_watcher);

  remote_service_manager_ =
      std::make_unique<RemoteServiceManager>(std::move(client));
  remote_service_manager_->set_service_watcher(std::move(svc_watcher));
  remote_service_manager_->set_fatal_error_callback(
      [self = weak_self_.GetWeakPtr()](att::Error error) {
        if (!self.is_alive()) {
          return;
        }
        bt_log(WARN,
               "gatt",
               "client error after setup (%s); shutting down",
               bt_str(error));
        self->ShutDown();
      });
}

void Connection::Initialize(
    std::vector<UUID> service_uuids,
    fit::callback<void(uint16_t)> mtu_cb,
    fit::callback<void(const bt::att::Error&)> on_error) {
  PW_CHECK(remote_service_manager_);

  auto uuids_count = service_uuids.size();
  // status_cb must not capture att_ in order to prevent reference cycle.
  auto status_cb = [self = weak_self_.GetWeakPtr(),
                    uuids_count,
                    on_error_cb =
                        std::move(on_error)](att::Result<> status) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (bt_is_error(status, ERROR, "gatt", "client setup failed")) {
      // Signal a link error.
      on_error_cb(status.error_value());
      self->ShutDown();
    } else if (uuids_count > 0) {
      bt_log(DEBUG,
             "gatt",
             "primary service discovery complete for (%zu) service uuids",
             uuids_count);
    } else {
      bt_log(DEBUG, "gatt", "primary service discovery complete");
    }
  };

  remote_service_manager_->Initialize(
      std::move(status_cb), std::move(mtu_cb), std::move(service_uuids));
}

void Connection::ShutDown() {
  // We shut down the connection from the server not for any technical reason,
  // but just because it was simpler to expose the att::Bearer's ShutDown
  // behavior from the server.
  server_->ShutDown();
}
}  // namespace bt::gatt::internal
