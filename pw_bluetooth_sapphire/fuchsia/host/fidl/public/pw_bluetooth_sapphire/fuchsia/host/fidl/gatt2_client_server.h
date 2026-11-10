// Copyright 2024 The Pigweed Authors
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

#include <fuchsia/bluetooth/gatt2/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt2_remote_service_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/server_base.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"

namespace bthost {
class Gatt2ClientServer
    : public GattServerBase<fuchsia::bluetooth::gatt2::Client> {
 public:
  // |error_cb| will be called if the FIDL client closed the protocol or an
  // error occurs and this server should be destroyed.
  Gatt2ClientServer(
      bt::gatt::PeerId peer_id,
      bt::gatt::GATT::WeakPtr weak_gatt,
      fidl::InterfaceRequest<fuchsia::bluetooth::gatt2::Client> request,
      fit::callback<void()> error_cb);
  ~Gatt2ClientServer() override;

 private:
  using WatchServicesCallbackOnce =
      fit::callback<void(std::vector<::fuchsia::bluetooth::gatt2::ServiceInfo>,
                         std::vector<::fuchsia::bluetooth::gatt2::Handle>)>;
  using WatchServicesRequest = WatchServicesCallbackOnce;

  using ServiceMap =
      std::unordered_map<bt::att::Handle, bt::gatt::RemoteService::WeakPtr>;

  struct WatchServicesResult {
    std::unordered_set<bt::att::Handle> removed;
    ServiceMap updated;
  };

  void OnWatchServicesResult(const std::vector<bt::att::Handle>& removed,
                             const bt::gatt::ServiceList& added,
                             const bt::gatt::ServiceList& modified);

  void TrySendNextWatchServicesResult();

  // fuchsia::bluetooth::gatt2::Client overrides:
  void WatchServices(std::vector<::fuchsia::bluetooth::Uuid> fidl_uuids,
                     WatchServicesCallback callback) override;
  void ConnectToService(
      fuchsia::bluetooth::gatt2::ServiceHandle handle,
      fidl::InterfaceRequest<fuchsia::bluetooth::gatt2::RemoteService> request)
      override;

  // The ID of the peer that this client is attached to.
  bt::gatt::PeerId peer_id_;

  // Callback provided by this server's owner that handles fatal errors (by
  // closing this server).
  fit::callback<void()> server_error_cb_;

  // Servers for connected RemoteServices, keyed by service handle.
  std::unordered_map<bt::att::Handle, std::unique_ptr<Gatt2RemoteServiceServer>>
      services_;

  // False initially, and set to true after GATT::ListServices() completes.
  // Set to false again if WatchServices() is called with a new UUID list.
  bool list_services_complete_ = false;

  // UUIDs of the previous WatchServices() call, if any.
  std::unordered_set<bt::UUID> prev_watch_services_uuids_;
  std::optional<WatchServicesRequest> watch_services_request_;

  // Between clients calls to WatchServices, service watcher results are
  // accumulated here.
  std::optional<WatchServicesResult> next_watch_services_result_;

  bt::gatt::GATT::RemoteServiceWatcherId service_watcher_id_;

  // Must be the last member of this class.
  WeakSelf<Gatt2ClientServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Gatt2ClientServer);
};
}  // namespace bthost
