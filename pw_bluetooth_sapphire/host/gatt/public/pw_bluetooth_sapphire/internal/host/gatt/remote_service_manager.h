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
#include <map>
#include <memory>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/att/error.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"

namespace bt::gatt {

class Client;

namespace internal {

// Maintains a collection of services that are present on a particular peer
// device. The RemoteServiceManager owns the GATT client data bearer, interacts
// with the profile-defined GATT service, and allows observers to be notified
// as services get discovered.
class RemoteServiceManager final {
 public:
  // The maximum number of services tracked for a peer, which bounds the memory
  // a peer can make us allocate by declaring a large number of services. If a
  // peer reports more services than this, either during initialization or in
  // response to a Service Changed indication, service discovery is stopped and
  // a HostError::kOutOfMemory error is reported (see Initialize() and
  // set_fatal_error_callback()).
  //
  // Each tracked service costs a RemoteService (~92 bytes on 32-bit targets,
  // ~144 bytes on 64-bit hosts), a ServiceMap node (~24 / ~48 bytes) and a
  // WeakPtr reference (~8 / ~16 bytes), i.e. ~125 / ~210 bytes before
  // allocator overhead and before any characteristics are discovered. The
  // worst case is therefore ~31 KiB (32-bit) / ~52 KiB (64-bit) per peer,
  // instead of the several MiB a peer could make us allocate without a limit.
  // Real devices expose far fewer services (typically well under 50), so 256
  // leaves ample headroom.
  static constexpr size_t kMaxRemoteServices = 256;

  RemoteServiceManager(std::unique_ptr<Client> client);
  ~RemoteServiceManager();

  // Adds a handler to be notified when services are removed, added, or
  // modified. NOTE: `removed` services should be handled first because they may
  // share handles with `added` services.
  void set_service_watcher(RemoteServiceWatcher watcher) {
    svc_watcher_ = std::move(watcher);
  }

  // Sets a handler that is called if, after initialization, the peer reports
  // changes to its services that cannot be applied (e.g. a Service Changed
  // indication that would exceed kMaxRemoteServices). The changes are not
  // applied and the owner is expected to close the connection. Errors that
  // occur during initialization are reported to the Initialize() callback
  // instead.
  void set_fatal_error_callback(fit::callback<void(att::Error)> callback) {
    fatal_error_callback_ = std::move(callback);
  }

  // Initiates the Exchange MTU procedure followed by service discovery.
  // |callback| is called to notify the result of the procedure.
  // |mtu_callback| is called when the MTU for the connection is determined,
  // which may occur before initialization completes. If |services| is empty,
  // discover all services. If |services| is not empty, only discover services
  // that match the UUIDs in |services|.
  // TODO(fxbug.dev/42144310): Support initiating multiple service discoveries
  // for different service UUIDs.
  void Initialize(att::ResultFunction<> callback,
                  fit::callback<void(uint16_t)> mtu_callback,
                  std::vector<UUID> services = {});

  // Returns a vector containing discovered services that match any of the given
  // |uuids| via |callback|. All services will be returned if |uuids| is empty.
  //
  // If called while uninitialized, |callback| will be run after initialization.
  void ListServices(const std::vector<UUID>& uuids,
                    ServiceListCallback callback);

  // Returns the RemoteService with the requested range start |handle| or
  // nullptr if it is not recognized. This method may fail if called before or
  // during initialization.
  RemoteService::WeakPtr FindService(att::Handle handle);

 private:
  using ServiceMap = std::map<att::Handle, std::unique_ptr<RemoteService>>;

  // Used to represent a queued ListServices() request.
  class ServiceListRequest {
   public:
    ServiceListRequest(ServiceListCallback callback,
                       const std::vector<UUID>& uuids);

    ServiceListRequest() = default;
    ServiceListRequest(ServiceListRequest&&) = default;

    // Completes this request by using entries from |services| that match any of
    // the requested UUIDs.
    void Complete(att::Result<> status, const ServiceMap& services);

   private:
    ServiceListCallback callback_;
    std::vector<UUID> uuids_;

    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ServiceListRequest);
  };

  // State of the current Service Changed notification procedure.
  struct ServiceChangedState {
    ServiceChangedCharacteristicValue value;
    // The cache of services discovered in the changed range.
    std::map<att::Handle, ServiceData> services;
    // The maximum number of services in the changed range that keep the total
    // number of services within kMaxRemoteServices.
    size_t max_services = 0;
    // Set if more than |max_services| services were discovered in the changed
    // range.
    bool exceeded_max_services = false;
  };

  // If the GATT Profile service has been discovered, returns the service.
  // Returns nullptr otherwise.
  RemoteService* GattProfileService();

  // Attempt to discover the GATT Profile service. This method must complete
  // before discovery of other services. Notifies |callback| with a status of
  // HostError::kNotFound if the GATT Profile service is not found.
  void DiscoverGattProfileService(att::ResultFunction<> callback);

  // Discovers characteristics of |gatt_profile_service| and enables
  // notifications of the Service Changed characteristic. Notifies |callback|
  // with a status of HostError::kNotFound if the GATT Profile service's Service
  // Changed characteristic is not found.
  void ConfigureServiceChangedNotifications(RemoteService* gatt_profile_service,
                                            att::ResultFunction<> callback);

  // Discover the GATT Profile service and configure the Service Changed
  // characteristic therein.
  void InitializeGattProfileService(att::ResultFunction<> callback);

  // Create a RemoteService and insert it into the services map, discarding
  // duplicates. Returns an error, without adding the service, if
  // kMaxRemoteServices services are already tracked.
  att::Result<> AddService(const ServiceData& service_data);

  // Discover services of the indicated |kind|.
  // If |service_uuids| is non-empty, only discovers services with the given
  // UUIDs. |status_cb| will be called when the operation completes.
  using ServiceCallback = Client::ServiceCallback;
  void DiscoverServicesOfKind(ServiceKind kind,
                              std::vector<UUID> service_uuids,
                              att::ResultFunction<> status_cb);

  // Discover primary and secondary services.
  // If |service_uuids| is non-empty, only discovers services with the given
  // UUIDs. |status_callback| will be called when the operation completes.
  void DiscoverServices(std::vector<UUID> service_uuids,
                        att::ResultFunction<> status_callback);
  void DiscoverPrimaryAndSecondaryServicesInRange(
      std::vector<UUID> service_uuids,
      att::Handle start,
      att::Handle end,
      ServiceCallback service_cb,
      att::ResultFunction<> status_callback);

  // Called by |client_| when a notification or indication is received.
  void OnNotification(bool ind,
                      att::Handle value_handle,
                      const ByteBuffer& value,
                      bool maybe_truncated);

  // Handler of notifications from the Service Changed Characteristic
  void OnServiceChangedNotification(const ByteBuffer& buffer);

  // If a service changed notification is queued and none are currently being
  // handled, start processing the next notification. |on_complete| is called
  // when all pending service changed notifications have been processed
  // (immediately if there are none), or with an error if a service change
  // could not be applied.
  void MaybeHandleNextServiceChangedNotification(
      att::ResultCallback<> on_complete = nullptr);

  // Called when the current service change cannot be applied. Drops all
  // pending service changes and reports |error| to the Initialize() callback
  // if initialization is in progress, or to |fatal_error_callback_| otherwise.
  void OnServiceChangedFatalError(att::Error error);

  // Apply service changed discovery results to |services_|. If initialization
  // is complete, notify |svc_watcher_|.
  void ProcessServiceChangedDiscoveryResults(
      const ServiceChangedState& service_changed);

  // Calculate the difference between the current services and the services
  // discovered in the Service Changed notification range. Results are appended
  // to the parameters vectors.
  void CalculateServiceChanges(
      const ServiceChangedState& service_changed,
      std::vector<ServiceMap::iterator>& removed_services,
      std::vector<ServiceData>& added_services,
      std::vector<std::pair<ServiceMap::iterator, ServiceData>>&
          modified_services);

  std::unique_ptr<Client> client_;

  bool initialized_;
  RemoteServiceWatcher svc_watcher_;
  fit::callback<void(att::Error)> fatal_error_callback_;

  // Requests queued during calls ListServices() before initialization.
  std::queue<ServiceListRequest> pending_list_services_requests_;

  // Services are sorted by handle.
  ServiceMap services_;

  // Queue of unprocessed Service Changed notification changes.
  std::queue<ServiceChangedCharacteristicValue> queued_service_changes_;

  // Service Changed notification that is currently being processed by
  // performing service discovery.
  std::optional<ServiceChangedState> current_service_change_;

  // Callbacks passed to MaybeHandleNextServiceChangedNotification. Will be
  // called when all queued service changes have been processed.
  std::vector<att::ResultCallback<>> service_changes_complete_callbacks_;

  WeakSelf<RemoteServiceManager> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(RemoteServiceManager);
};

}  // namespace internal
}  // namespace bt::gatt
