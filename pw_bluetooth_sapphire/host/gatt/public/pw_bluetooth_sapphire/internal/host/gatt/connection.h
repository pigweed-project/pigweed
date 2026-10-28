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

#pragma once
#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service_manager.h"

namespace bt {

namespace l2cap {
class Channel;
}  // namespace l2cap

namespace att {
class Bearer;
}  // namespace att

namespace gatt {

class Server;

namespace internal {

// Represents the GATT data channel between the local adapter and a single
// remote peer. A Connection supports simultaneous GATT client and server
// functionality. An instance of Connection should exist on each ACL logical
// link.
class Connection final {
 public:
  // |client| is the GATT client for this connection, which uses |att_bearer| in
  // production. |server| is the GATT server for this connection, which uses
  // |att_bearer| in production. |svc_watcher| communicates updates about the
  // peer's GATT services to the Connection's owner.
  Connection(std::unique_ptr<Client> client,
             std::unique_ptr<Server> server,
             RemoteServiceWatcher svc_watcher);
  ~Connection() = default;

  Server* server() const { return server_.get(); }
  RemoteServiceManager* remote_service_manager() const {
    return remote_service_manager_.get();
  }

  // Performs MTU exchange, then primary service discovery. Shuts down the
  // connection on failure. If |service_uuids| is non-empty, discovery is only
  // performed for services with the indicated UUIDs. Returns the agreed-upon
  // MTU via |mtu_cb|. The connection is also shut down if the peer later
  // reports service changes that cannot be applied.
  //
  // If an error occurs, the `on_error` callback will be called with the
  // corresponding error code
  void Initialize(std::vector<UUID> service_uuids,
                  fit::callback<void(uint16_t)> mtu_cb,
                  fit::callback<void(const bt::att::Error&)> on_error);

  // Closes the ATT bearer on which the connection operates.
  void ShutDown();

 private:
  std::unique_ptr<Server> server_;
  std::unique_ptr<RemoteServiceManager> remote_service_manager_;

  WeakSelf<Connection> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Connection);
};

}  // namespace internal
}  // namespace gatt
}  // namespace bt
