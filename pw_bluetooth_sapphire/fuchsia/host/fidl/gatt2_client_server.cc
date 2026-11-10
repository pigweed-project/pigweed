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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt2_client_server.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"

namespace fb = fuchsia::bluetooth;
namespace fbg = fuchsia::bluetooth::gatt2;

namespace bthost {

namespace {

fbg::ServiceInfo RemoteServiceToFidlServiceInfo(
    const bt::gatt::RemoteService::WeakPtr& svc) {
  fbg::ServiceInfo out;
  out.set_handle(fbg::ServiceHandle{svc->handle()});
  auto kind = svc->info().kind == bt::gatt::ServiceKind::PRIMARY
                  ? fbg::ServiceKind::PRIMARY
                  : fbg::ServiceKind::SECONDARY;
  out.set_kind(kind);
  out.set_type(fb::Uuid{svc->uuid().value()});
  return out;
}

}  // namespace

Gatt2ClientServer::Gatt2ClientServer(
    bt::gatt::PeerId peer_id,
    bt::gatt::GATT::WeakPtr weak_gatt,
    fidl::InterfaceRequest<fbg::Client> request,
    fit::callback<void()> error_cb)
    : GattServerBase(std::move(weak_gatt), /*impl=*/this, std::move(request)),
      peer_id_(peer_id),
      server_error_cb_(std::move(error_cb)),
      weak_self_(this) {
  set_error_handler([this](zx_status_t) {
    if (server_error_cb_) {
      server_error_cb_();
    }
  });

  // It is safe to bind |this| to the callback because the service watcher is
  // unregistered in the destructor.
  service_watcher_id_ = gatt()->RegisterRemoteServiceWatcherForPeer(
      peer_id_, [this](auto removed, auto added, auto modified) {
        // Ignore results before the initial call to ListServices() completes to
        // avoid redundant notifications.
        if (!list_services_complete_) {
          bt_log(TRACE,
                 "fidl",
                 "ignoring service watcher update before ListServices() result "
                 "received");
          return;
        }
        OnWatchServicesResult(removed, added, modified);
      });
}

Gatt2ClientServer::~Gatt2ClientServer() {
  PW_CHECK(gatt()->UnregisterRemoteServiceWatcher(service_watcher_id_));
}

void Gatt2ClientServer::OnWatchServicesResult(
    const std::vector<bt::att::Handle>& removed,
    const bt::gatt::ServiceList& added,
    const bt::gatt::ServiceList& modified) {
  // Accumulate all removed services and send in next result.
  if (!next_watch_services_result_.has_value()) {
    next_watch_services_result_.emplace();
  }
  next_watch_services_result_->removed.insert(removed.begin(), removed.end());

  // Remove any stale updated services (to avoid sending an invalid one to the
  // client).
  for (bt::att::Handle handle : removed) {
    next_watch_services_result_->updated.erase(handle);
  }

  // Replace any existing updated services with same handle and add new updates.
  for (const bt::gatt::RemoteService::WeakPtr& svc : added) {
    next_watch_services_result_->updated[svc->handle()] = svc;
  }
  for (const bt::gatt::RemoteService::WeakPtr& svc : modified) {
    next_watch_services_result_->updated[svc->handle()] = svc;
  }

  bt_log(TRACE,
         "fidl",
         "next watch services result: (removed: %zu, updated: %zu) (peer: %s)",
         next_watch_services_result_->removed.size(),
         next_watch_services_result_->updated.size(),
         bt_str(peer_id_));

  TrySendNextWatchServicesResult();
}

void Gatt2ClientServer::TrySendNextWatchServicesResult() {
  if (!watch_services_request_ || !next_watch_services_result_) {
    return;
  }

  std::vector<fbg::Handle> fidl_removed;
  std::transform(
      next_watch_services_result_->removed.begin(),
      next_watch_services_result_->removed.end(),
      std::back_inserter(fidl_removed),
      [](const bt::att::Handle& handle) { return fbg::Handle{handle}; });

  // Don't filter removed services by UUID because we don't know the UUIDs of
  // these services currently.
  // TODO(fxbug.dev/42111895): Filter removed services by UUID.

  std::vector<fbg::ServiceInfo> fidl_updated;
  for (const ServiceMap::value_type& svc_pair :
       next_watch_services_result_->updated) {
    // Filter updated services by UUID.
    // NOTE: If clients change UUIDs they are requesting across requests, they
    // won't receive existing service with the new UUIDs, only new ones
    if (prev_watch_services_uuids_.empty() ||
        prev_watch_services_uuids_.count(svc_pair.second->uuid()) == 1) {
      fidl_updated.push_back(RemoteServiceToFidlServiceInfo(svc_pair.second));
    }
  }

  next_watch_services_result_.reset();

  // Skip sending results that are empty after filtering services by UUID.
  if (fidl_removed.empty() && fidl_updated.empty()) {
    bt_log(TRACE,
           "fidl",
           "skipping service watcher update without matching UUIDs (peer: %s)",
           bt_str(peer_id_));
    return;
  }

  // TODO(fxbug.dev/42165836): Use measure-tape to verify response fits in FIDL
  // channel before sending. This is only an issue for peers with very large
  // databases.
  bt_log(TRACE,
         "fidl",
         "notifying WatchServices() callback (removed: %zu, updated: %zu, "
         "peer: %s)",
         fidl_removed.size(),
         fidl_updated.size(),
         bt_str(peer_id_));
  watch_services_request_.value()(std::move(fidl_updated),
                                  std::move(fidl_removed));
  watch_services_request_.reset();
}

// TODO(fxbug.dev/42165818): Do not send privileged services (e.g. Generic
// Attribute Profile Service) to clients.
void Gatt2ClientServer::WatchServices(std::vector<fb::Uuid> fidl_uuids,
                                      WatchServicesCallback callback) {
  std::unordered_set<bt::UUID> uuids;
  std::transform(fidl_uuids.begin(),
                 fidl_uuids.end(),
                 std::inserter(uuids, uuids.begin()),
                 [](const fb::Uuid& uuid) { return bt::UUID(uuid.value); });

  // If the UUID filter list is changed between requests, perform a fresh
  // ListServices() call to ensure existing services that match the new UUIDs
  // are reported to the client. Dropping the old watch_services_request_ with
  // no new results.
  if (uuids != prev_watch_services_uuids_) {
    bt_log(DEBUG,
           "fidl",
           "WatchServices: UUIDs changed from previous call (peer: %s)",
           bt_str(peer_id_));
    list_services_complete_ = false;
    // Clear old watch service results as we're about to get a fresh list of
    // services.
    next_watch_services_result_.reset();
    prev_watch_services_uuids_ = uuids;
    if (watch_services_request_) {
      watch_services_request_.value()({}, {});
      watch_services_request_.reset();
    }
  }

  // Only allow 1 callback at a time. Close the server if this is violated.
  if (watch_services_request_) {
    bt_log(WARN,
           "fidl",
           "%s: call received while previous call is still pending",
           __FUNCTION__);
    binding()->Close(ZX_ERR_ALREADY_BOUND);
    server_error_cb_();
    return;
  }

  watch_services_request_.emplace(std::move(callback));

  auto self = weak_self_.GetWeakPtr();

  // Return a complete service snapshot on the first call, or on calls that use
  // a new UUID filter list.
  if (!list_services_complete_) {
    std::vector<bt::UUID> uuids_vector(uuids.begin(), uuids.end());
    gatt()->ListServices(
        peer_id_,
        std::move(uuids_vector),
        [self](bt::att::Result<> status,
               const bt::gatt::ServiceList& services) {
          if (!self.is_alive()) {
            return;
          }
          if (bt_is_error(status,
                          INFO,
                          "fidl",
                          "WatchServices: ListServices failed (peer: %s)",
                          bt_str(self->peer_id_))) {
            self->binding()->Close(ZX_ERR_CONNECTION_RESET);
            self->server_error_cb_();
            return;
          }

          bt_log(DEBUG,
                 "fidl",
                 "WatchServices: ListServices complete (peer: %s)",
                 bt_str(self->peer_id_));

          PW_CHECK(self->watch_services_request_);
          self->list_services_complete_ = true;
          self->OnWatchServicesResult(
              /*removed=*/{}, /*added=*/services, /*modified=*/{});
        });
    return;
  }

  TrySendNextWatchServicesResult();
}

void Gatt2ClientServer::ConnectToService(
    fbg::ServiceHandle handle,
    fidl::InterfaceRequest<fbg::RemoteService> request) {
  bt_log(DEBUG, "fidl", "%s: (handle: 0x%lX)", __FUNCTION__, handle.value);

  if (!fidl_helpers::IsFidlGattServiceHandleValid(handle)) {
    request.Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  bt::att::Handle service_handle = static_cast<bt::att::Handle>(handle.value);

  // Only allow clients to have 1 RemoteService per service at a time to prevent
  // race conditions between multiple RemoteService clients modifying a service,
  // and to simplify implementation. A client shouldn't need more than 1
  // RemoteService per service at a time, but if they really need to, they can
  // create multiple Client instances.
  if (services_.count(service_handle) == 1) {
    request.Close(ZX_ERR_ALREADY_EXISTS);
    return;
  }

  bt::gatt::RemoteService::WeakPtr service =
      gatt()->FindService(peer_id_, service_handle);
  if (!service.is_alive()) {
    bt_log(INFO,
           "fidl",
           "service not found (peer: %s, handle: %#.4x)",
           bt_str(peer_id_),
           service_handle);
    request.Close(ZX_ERR_NOT_FOUND);
    return;
  }
  PW_CHECK(service_handle == service->handle());

  // This removed handler may be called long after the service is removed from
  // the service map or this server is destroyed, since removed handlers are not
  // unregistered. If the FIDL client connects->disconnects->connects, it is
  // possible for this handler to be called twice (the second call should then
  // do nothing).
  auto self = weak_self_.GetWeakPtr();
  fit::closure removed_handler = [self, service_handle] {
    if (!self.is_alive()) {
      return;
    }
    bt_log(DEBUG,
           "fidl",
           "service removed (peer: %s, handle: %#.4x)",
           bt_str(self->peer_id_),
           service_handle);
    auto svc_iter = self->services_.find(service_handle);
    if (svc_iter == self->services_.end()) {
      bt_log(TRACE,
             "fidl",
             "ignoring service removed callback for already removed service "
             "(peer: %s, handle: "
             "%#.4x)",
             bt_str(self->peer_id_),
             service_handle);
      return;
    }
    svc_iter->second->Close(ZX_ERR_CONNECTION_RESET);
    self->services_.erase(svc_iter);
  };

  // The only reason RemoteService::AddRemovedHandler() can fail is if the
  // service is already shut down, but that should not be possible in this
  // synchronous callback (the service would not have been returned in the first
  // place).
  PW_CHECK(service->AddRemovedHandler(std::move(removed_handler)),
           "adding service removed handler failed (service may be shut "
           "down) (peer: %s, "
           "handle: %#.4x)",
           bt_str(peer_id_),
           service_handle);

  std::unique_ptr<Gatt2RemoteServiceServer> remote_service_server =
      std::make_unique<Gatt2RemoteServiceServer>(
          std::move(service), gatt(), peer_id_, std::move(request));

  // Even if there is already an error, this handler won't be called until the
  // next yield to the event loop.
  remote_service_server->set_error_handler(
      [self, service_handle](zx_status_t status) {
        bt_log(TRACE,
               "fidl",
               "FIDL channel error (peer: %s, handle: %#.4x)",
               bt_str(self->peer_id_),
               service_handle);
        self->services_.erase(service_handle);
      });

  PW_CHECK(services_.emplace(service_handle, std::move(remote_service_server))
               .second);
}

}  // namespace bthost
