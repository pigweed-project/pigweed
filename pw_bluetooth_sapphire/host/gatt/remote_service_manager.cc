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

#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service_manager.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/att/error.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"

namespace bt::gatt::internal {

RemoteServiceManager::ServiceListRequest::ServiceListRequest(
    ServiceListCallback callback, const std::vector<UUID>& uuids)
    : callback_(std::move(callback)), uuids_(uuids) {
  PW_DCHECK(callback_);
}

void RemoteServiceManager::ServiceListRequest::Complete(
    att::Result<> status, const ServiceMap& services) {
  TRACE_DURATION("bluetooth",
                 "gatt::RemoteServiceManager::ServiceListRequest::Complete");

  ServiceList result;

  if (status.is_error() || services.empty()) {
    callback_(status, std::move(result));
    return;
  }

  for (const auto& iter : services) {
    auto& svc = iter.second;
    auto pred = [&svc](const UUID& uuid) { return svc->uuid() == uuid; };
    if (uuids_.empty() ||
        std::find_if(uuids_.begin(), uuids_.end(), pred) != uuids_.end()) {
      result.push_back(iter.second->GetWeakPtr());
    }
  }

  callback_(status, std::move(result));
}

RemoteServiceManager::RemoteServiceManager(std::unique_ptr<Client> client)
    : client_(std::move(client)), initialized_(false), weak_self_(this) {
  PW_DCHECK(client_);

  client_->SetNotificationHandler(
      fit::bind_member<&RemoteServiceManager::OnNotification>(this));
}

RemoteServiceManager::~RemoteServiceManager() {
  client_->SetNotificationHandler({});
  services_.clear();

  // Resolve all pending requests with an error.
  att::Result<> status = ToResult(HostError::kFailed);

  auto pending = std::move(pending_list_services_requests_);
  while (!pending.empty()) {
    // This copies |services|.
    pending.front().Complete(status, services_);
    pending.pop();
  }
}

void RemoteServiceManager::Initialize(
    att::ResultFunction<> result_callback,
    fit::callback<void(uint16_t)> mtu_callback,
    std::vector<UUID> services_to_discover) {
  auto self = weak_self_.GetWeakPtr();

  auto wrapped_result_cb = [self, user_result_cb = std::move(result_callback)](
                               att::Result<> status) mutable {
    TRACE_DURATION("bluetooth",
                   "gatt::RemoteServiceManager::Initialize::init_cb");
    bt_log(DEBUG, "gatt", "RemoteServiceManager initialization complete");

    // The Client's Bearer may outlive this object.
    if (!self.is_alive()) {
      return;
    }

    self->initialized_ = true;

    if (status.is_ok() && self->svc_watcher_) {
      // Notify all discovered services here.
      TRACE_DURATION("bluetooth", "gatt::RemoteServiceManager::svc_watcher_");
      ServiceList added;
      std::transform(
          self->services_.begin(),
          self->services_.end(),
          std::back_inserter(added),
          [](ServiceMap::value_type& svc) { return svc.second->GetWeakPtr(); });
      self->svc_watcher_(/*removed=*/{}, std::move(added), /*modified=*/{});
    }

    // Notify pending ListService() requests.
    while (!self->pending_list_services_requests_.empty()) {
      self->pending_list_services_requests_.front().Complete(status,
                                                             self->services_);
      self->pending_list_services_requests_.pop();
    }

    user_result_cb(status);
  };

  client_->ExchangeMTU([self,
                        result_cb = std::move(wrapped_result_cb),
                        mtu_cb = std::move(mtu_callback),
                        discovery_services = std::move(services_to_discover)](
                           att::Result<uint16_t> mtu_result) mutable {
    // The Client's Bearer may outlive this object.
    if (!self.is_alive()) {
      return;
    }

    // Support for the MTU exchange is optional, so if the peer indicated they
    // don't support it, we continue with initialization.
    if (mtu_result.is_ok() ||
        mtu_result.error_value().is(att::ErrorCode::kRequestNotSupported)) {
      bt_is_error(mtu_result, DEBUG, "gatt", "MTU exchange not supported");
      mtu_cb(mtu_result.value_or(att::kLEMinMTU));
    } else {
      bt_log(INFO,
             "gatt",
             "MTU exchange failed: %s",
             bt_str(mtu_result.error_value()));
      result_cb(fit::error(mtu_result.error_value()));
      return;
    }

    self->InitializeGattProfileService([self,
                                        init_cb = std::move(result_cb),
                                        services =
                                            std::move(discovery_services)](
                                           att::Result<> init_status) mutable {
      if (init_status == ToResult(HostError::kNotFound)) {
        // The GATT Profile service's Service Changed characteristic is
        // optional. Its absence implies that the set of GATT services on
        // the server is fixed, so the kNotFound error can be safely
        // ignored.
        bt_log(DEBUG,
               "gatt",
               "GATT Profile service not found. Assuming services are fixed.");
      } else if (init_status.is_error()) {
        init_cb(init_status);
        return;
      }

      self->DiscoverServices(
          std::move(services),
          [self, discover_cb = std::move(init_cb)](
              att::Result<> discovery_status) mutable {
            if (discovery_status.is_error()) {
              discover_cb(discovery_status);
              return;
            }

            // Handle Service Changed notifications received during service
            // discovery. Skip notifying the service watcher callback as it
            // will be notified in the init_cb callback. We handle Service
            // Changed notifications before notifying the service watcher
            // and init_cb in order to reduce the likelihood that returned
            // services are instantly invalidated by Service Changed
            // notifications. It is likely that bonded peers will send a
            // Service Changed notification upon connection to indicate that
            // services changed since the last connection, and such
            // notifications will probably be received before service
            // discovery completes. (Core Spec v5.3, Vol 3, Part G,
            // Sec 2.5.2)
            self->MaybeHandleNextServiceChangedNotification(
                [self, cb = std::move(discover_cb)]() mutable {
                  cb(fit::ok());
                });
          });
    });
  });
}

RemoteService* RemoteServiceManager::GattProfileService() {
  auto service_iter =
      std::find_if(services_.begin(), services_.end(), [](auto& s) {
        return s.second->uuid() == types::kGenericAttributeService;
      });
  return service_iter == services_.end() ? nullptr : service_iter->second.get();
}

void RemoteServiceManager::ConfigureServiceChangedNotifications(
    RemoteService* gatt_profile_service,
    att::ResultFunction<> result_callback) {
  auto self = weak_self_.GetWeakPtr();
  gatt_profile_service->DiscoverCharacteristics(
      [self, callback = std::move(result_callback)](
          att::Result<> status,
          const CharacteristicMap& characteristics) mutable {
        // The Client's Bearer may outlive this object.
        if (!self.is_alive()) {
          return;
        }

        if (bt_is_error(
                status,
                WARN,
                "gatt",
                "Error discovering GATT Profile service characteristics")) {
          callback(status);
          return;
        }

        RemoteService* profile_service = self->GattProfileService();
        PW_CHECK(profile_service);

        auto svc_changed_char_iter = std::find_if(
            characteristics.begin(),
            characteristics.end(),
            [](CharacteristicMap::const_reference c) {
              const CharacteristicData& data = c.second.first;
              return data.type == types::kServiceChangedCharacteristic;
            });

        // The Service Changed characteristic is optional, and its absence
        // implies that the set of GATT services on the server is fixed.
        if (svc_changed_char_iter == characteristics.end()) {
          callback(ToResult(HostError::kNotFound));
          return;
        }

        const bt::gatt::CharacteristicHandle svc_changed_char_handle =
            svc_changed_char_iter->first;

        auto notification_cb = [self](const ByteBuffer& value,
                                      bool /*maybe_truncated*/) {
          // The Client's Bearer may outlive this object.
          if (self.is_alive()) {
            self->OnServiceChangedNotification(value);
          }
        };

        // Don't save handler_id as notifications never need to be disabled.
        auto status_cb = [self, cb = std::move(callback)](
                             att::Result<> enable_status,
                             IdType /*handler_id*/) {
          // The Client's Bearer may outlive this object.
          if (!self.is_alive()) {
            return;
          }

          // If the Service Changed characteristic exists, notification support
          // is mandatory (Core Spec v5.2, Vol 3, Part G, Sec 7.1).
          if (bt_is_error(enable_status,
                          WARN,
                          "gatt",
                          "Enabling notifications of Service Changed "
                          "characteristic failed")) {
            cb(enable_status);
            return;
          }

          cb(fit::ok());
        };

        profile_service->EnableNotifications(svc_changed_char_handle,
                                             std::move(notification_cb),
                                             std::move(status_cb));
      });
}

void RemoteServiceManager::InitializeGattProfileService(
    att::ResultFunction<> init_callback) {
  auto self = weak_self_.GetWeakPtr();
  DiscoverGattProfileService([self, callback = std::move(init_callback)](
                                 att::Result<> discovery_status) mutable {
    // The Client's Bearer may outlive this object.
    if (!self.is_alive()) {
      return;
    }

    if (discovery_status.is_error()) {
      callback(discovery_status);
      return;
    }

    RemoteService* gatt_svc = self->GattProfileService();
    PW_CHECK(gatt_svc);
    self->ConfigureServiceChangedNotifications(
        gatt_svc,
        [self, cb = std::move(callback)](att::Result<> config_status) {
          // The Client's Bearer may outlive this object.
          if (!self.is_alive()) {
            return;
          }

          cb(config_status);
        });
  });
}

void RemoteServiceManager::DiscoverGattProfileService(
    att::ResultFunction<> callback) {
  auto self = weak_self_.GetWeakPtr();
  auto status_cb = [self, cb = std::move(callback)](att::Result<> status) {
    if (!self.is_alive()) {
      return;
    }

    if (bt_is_error(
            status, WARN, "gatt", "Error discovering GATT Profile service")) {
      cb(status);
      return;
    }

    // The GATT Profile service is optional, and its absence implies that the
    // set of GATT services on the server is fixed.
    if (self->services_.empty()) {
      cb(ToResult(HostError::kNotFound));
      return;
    }

    // At most one instance of the GATT Profile service may exist (Core Spec
    // v5.2, Vol 3, Part G, Sec 7).
    if (self->services_.size() > 1) {
      bt_log(WARN,
             "gatt",
             "Discovered (%zu) GATT Profile services, expected 1",
             self->services_.size());
      cb(ToResult(HostError::kFailed));
      return;
    }

    UUID uuid = self->services_.begin()->second->uuid();
    // The service UUID is filled in by Client based on the service discovery
    // request, so it should be the same as the requested UUID.
    PW_CHECK(uuid == types::kGenericAttributeService);

    cb(fit::ok());
  };
  DiscoverServicesOfKind(ServiceKind::PRIMARY,
                         {types::kGenericAttributeService},
                         std::move(status_cb));
}

void RemoteServiceManager::AddService(const ServiceData& service_data) {
  att::Handle handle = service_data.range_start;
  auto iter = services_.find(handle);
  if (iter != services_.end()) {
    // The GATT Profile service is discovered before general service discovery,
    // so it may be discovered twice.
    if (iter->second->uuid() != types::kGenericAttributeService) {
      bt_log(WARN,
             "gatt",
             "found duplicate service attribute handle! (%#.4x)",
             handle);
    }
    return;
  }

  auto svc =
      std::make_unique<RemoteService>(service_data, client_->GetWeakPtr());
  if (!svc) {
    bt_log(DEBUG, "gatt", "failed to allocate RemoteService");
    return;
  }

  services_[handle] = std::move(svc);
}

void RemoteServiceManager::DiscoverServicesOfKind(
    ServiceKind kind,
    std::vector<UUID> service_uuids,
    att::ResultFunction<> status_cb) {
  auto self = weak_self_.GetWeakPtr();
  ServiceCallback svc_cb = [self](const ServiceData& service_data) {
    // The Client's Bearer may outlive this object.
    if (self.is_alive()) {
      self->AddService(service_data);
    }
  };

  if (!service_uuids.empty()) {
    client_->DiscoverServicesWithUuids(kind,
                                       std::move(svc_cb),
                                       std::move(status_cb),
                                       std::move(service_uuids));
  } else {
    client_->DiscoverServices(kind, std::move(svc_cb), std::move(status_cb));
  }
}

void RemoteServiceManager::DiscoverServices(
    std::vector<UUID> service_uuids, att::ResultFunction<> status_callback) {
  auto self = weak_self_.GetWeakPtr();
  auto status_cb_wrapper =
      [self, status_cb = std::move(status_callback)](att::Result<> status) {
        TRACE_DURATION(
            "bluetooth",
            "gatt::RemoteServiceManager::DiscoverServices::status_cb_wrapper");

        // The Client's Bearer may outlive this object.
        if (!self.is_alive()) {
          status_cb(ToResult(HostError::kFailed));
          return;
        }

        // Service discovery support is mandatory for servers (v5.0, Vol 3, Part
        // G, 4.2).
        if (bt_is_error(status, TRACE, "gatt", "failed to discover services")) {
          self->services_.clear();
        }

        status_cb(status);
      };

  ServiceCallback svc_cb = [self](const ServiceData& service_data) {
    // The Client's Bearer may outlive this object.
    if (self.is_alive()) {
      self->AddService(service_data);
    }
  };

  DiscoverPrimaryAndSecondaryServicesInRange(std::move(service_uuids),
                                             att::kHandleMin,
                                             att::kHandleMax,
                                             std::move(svc_cb),
                                             std::move(status_cb_wrapper));
}

void RemoteServiceManager::DiscoverPrimaryAndSecondaryServicesInRange(
    std::vector<UUID> service_uuids,
    att::Handle start,
    att::Handle end,
    ServiceCallback service_cb,
    att::ResultFunction<> status_callback) {
  auto self = weak_self_.GetWeakPtr();
  auto primary_discov_cb = [self,
                            service_uuids,
                            start,
                            end,
                            svc_cb = service_cb.share(),
                            status_cb = std::move(status_callback)](
                               att::Result<> primary_status) mutable {
    if (!self.is_alive() || primary_status.is_error()) {
      status_cb(primary_status);
      return;
    }

    auto secondary_discov_cb = [cb = std::move(status_cb)](
                                   att::Result<> secondary_status) mutable {
      // Not all GATT servers support the "secondary service" group type. We
      // suppress the "Unsupported Group Type" error code and simply report no
      // services instead of treating it as a fatal condition (errors propagated
      // up the stack from here will cause the connection to be terminated).
      if (secondary_status == ToResult(att::ErrorCode::kUnsupportedGroupType)) {
        bt_log(DEBUG,
               "gatt",
               "peer does not support secondary services; ignoring ATT error");
        secondary_status = fit::ok();
      }

      cb(secondary_status);
    };

    if (!service_uuids.empty()) {
      self->client_->DiscoverServicesWithUuidsInRange(
          ServiceKind::SECONDARY,
          start,
          end,
          std::move(svc_cb),
          std::move(secondary_discov_cb),
          std::move(service_uuids));
    } else {
      self->client_->DiscoverServicesInRange(ServiceKind::SECONDARY,
                                             start,
                                             end,
                                             std::move(svc_cb),
                                             std::move(secondary_discov_cb));
    }
  };

  if (!service_uuids.empty()) {
    client_->DiscoverServicesWithUuidsInRange(ServiceKind::PRIMARY,
                                              start,
                                              end,
                                              std::move(service_cb),
                                              std::move(primary_discov_cb),
                                              std::move(service_uuids));
  } else {
    client_->DiscoverServicesInRange(ServiceKind::PRIMARY,
                                     start,
                                     end,
                                     std::move(service_cb),
                                     std::move(primary_discov_cb));
  }
}

void RemoteServiceManager::ListServices(const std::vector<UUID>& uuids,
                                        ServiceListCallback callback) {
  ServiceListRequest request(std::move(callback), uuids);
  if (initialized_) {
    request.Complete(fit::ok(), services_);
  } else {
    pending_list_services_requests_.push(std::move(request));
  }
}

RemoteService::WeakPtr RemoteServiceManager::FindService(att::Handle handle) {
  auto iter = services_.find(handle);
  return iter == services_.end() ? RemoteService::WeakPtr()
                                 : iter->second->GetWeakPtr();
}

void RemoteServiceManager::OnNotification(bool /*indication*/,
                                          att::Handle value_handle,
                                          const ByteBuffer& value,
                                          bool maybe_truncated) {
  if (services_.empty()) {
    bt_log(DEBUG, "gatt", "ignoring notification from unknown service");
    return;
  }

  // Find the service that |value_handle| belongs to.
  auto iter = services_.upper_bound(value_handle);
  if (iter != services_.begin())
    --iter;

  // If |value_handle| is within the previous service then we found it.
  auto& svc = iter->second;
  PW_DCHECK(value_handle >= svc->handle());

  if (svc->info().range_end >= value_handle) {
    svc->HandleNotification(value_handle, value, maybe_truncated);
  }
}

void RemoteServiceManager::OnServiceChangedNotification(
    const ByteBuffer& buffer) {
  bt_log(DEBUG, "gatt", "received service changed notification");

  if (buffer.size() != sizeof(ServiceChangedCharacteristicValue)) {
    bt_log(WARN,
           "gatt",
           "service changed notification value malformed; ignoring (size: %zu)",
           buffer.size());
    return;
  }

  ServiceChangedCharacteristicValue value;
  value.range_start_handle = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little,
      buffer.ReadMember<
          &ServiceChangedCharacteristicValue::range_start_handle>());
  value.range_end_handle = pw::bytes::ConvertOrderFrom(
      cpp20::endian::little,
      buffer
          .ReadMember<&ServiceChangedCharacteristicValue::range_end_handle>());
  if (value.range_start_handle > value.range_end_handle) {
    bt_log(
        WARN,
        "gatt",
        "service changed notification value malformed; ignoring (start > end)");
    return;
  }

  queued_service_changes_.push(value);

  // Bonded devices may send service changed notifications upon connection if
  // services changed while the device was disconnected (Core Spec v5.3, Vol 3,
  // Part G, Sec 7.1). These notifications may be received during the initial
  // service discovery procedure. Queue the service changes and process them as
  // the last step of initialization.
  if (!initialized_) {
    bt_log(DEBUG,
           "gatt",
           "Received service changed notification before RemoteServiceManager "
           "initialization "
           "complete; queueing.");
    return;
  }

  MaybeHandleNextServiceChangedNotification();
}

void RemoteServiceManager::MaybeHandleNextServiceChangedNotification(
    fit::callback<void()> on_complete) {
  if (on_complete) {
    service_changes_complete_callbacks_.push_back(std::move(on_complete));
  }

  if (current_service_change_.has_value()) {
    return;
  }

  if (queued_service_changes_.empty()) {
    for (auto& cb : service_changes_complete_callbacks_) {
      cb();
    }
    service_changes_complete_callbacks_.clear();
    return;
  }

  bt_log(DEBUG, "gatt", "handling next Service Changed notification");

  current_service_change_ = ServiceChangedState{
      .value = queued_service_changes_.front(), .services = {}};
  queued_service_changes_.pop();

  auto self = weak_self_.GetWeakPtr();
  ServiceCallback svc_cb = [self](const ServiceData& service_data) {
    if (self.is_alive()) {
      PW_CHECK(self->current_service_change_.has_value());
      // gatt::Client verifies that service discovery results are in the
      // requested range.
      PW_CHECK(service_data.range_start >=
               self->current_service_change_->value.range_start_handle);
      PW_CHECK(service_data.range_start <=
               self->current_service_change_->value.range_end_handle);
      self->current_service_change_->services.emplace(service_data.range_start,
                                                      service_data);
    }
  };

  att::ResultFunction<> status_cb = [self](att::Result<> status) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (!bt_is_error(
            status,
            WARN,
            "gatt",
            "service discovery for service changed notification failed")) {
      PW_CHECK(self->current_service_change_.has_value());
      self->ProcessServiceChangedDiscoveryResults(
          self->current_service_change_.value());
    }

    if (!self.is_alive()) {
      return;
    }

    self->current_service_change_.reset();
    self->MaybeHandleNextServiceChangedNotification();
  };

  DiscoverPrimaryAndSecondaryServicesInRange(
      /*service_uuids=*/{},
      self->current_service_change_->value.range_start_handle,
      self->current_service_change_->value.range_end_handle,
      std::move(svc_cb),
      std::move(status_cb));
}

void RemoteServiceManager::ProcessServiceChangedDiscoveryResults(
    const ServiceChangedState& service_changed) {
  std::vector<ServiceMap::iterator> removed_iters;
  std::vector<ServiceData> added_data;
  std::vector<std::pair<ServiceMap::iterator, ServiceData>>
      modified_iters_and_data;
  CalculateServiceChanges(
      service_changed, removed_iters, added_data, modified_iters_and_data);

  bt_log(INFO,
         "gatt",
         "service changed notification added %zu, removed %zu, and modified "
         "%zu services",
         added_data.size(),
         removed_iters.size(),
         modified_iters_and_data.size());

  auto self = weak_self_.GetWeakPtr();

  std::vector<att::Handle> removed_service_handles;
  for (ServiceMap::iterator& service_iter : removed_iters) {
    removed_service_handles.push_back(service_iter->first);
    service_iter->second->set_service_changed(true);
    services_.erase(service_iter);
    if (!self.is_alive()) {
      return;
    }
  }

  ServiceList modified_services;
  modified_services.reserve(modified_iters_and_data.size());
  for (auto& [service_iter, new_service_data] : modified_iters_and_data) {
    if (service_iter->second->uuid() == types::kGenericAttributeService) {
      // The specification is ambiguous about what to do if the GATT Profile
      // Service changes, but it implies that it means the Database Hash or
      // Server Supported Features values have changed. At the very least, the
      // Service Changed Characteristic is not supposed to change if the server
      // is bonded with any client. We don't want to reset the service and
      // potentially miss notifications until characteristics have been
      // rediscovered. See Core Spec v5.3, Vol 3, Part G, Sec 7.1.
      bt_log(INFO,
             "gatt",
             "GATT Profile Service changed; assuming same characteristics "
             "(server values probably "
             "changed)");
      modified_services.push_back(service_iter->second->GetWeakPtr());
      continue;
    }

    // Destroy the old service and replace with a new service in order to easily
    // cancel ongoing procedures and ensure clients handle service change.
    service_iter->second->set_service_changed(true);
    service_iter->second.reset();
    if (!self.is_alive()) {
      return;
    }

    auto new_service = std::make_unique<RemoteService>(new_service_data,
                                                       client_->GetWeakPtr());
    PW_CHECK(new_service->handle() == service_iter->first);
    modified_services.push_back(new_service->GetWeakPtr());
    service_iter->second = std::move(new_service);
  }

  ServiceList added_services;
  added_services.reserve(added_data.size());
  for (ServiceData service_data : added_data) {
    auto service =
        std::make_unique<RemoteService>(service_data, client_->GetWeakPtr());
    added_services.push_back(service->GetWeakPtr());
    auto [_, inserted] =
        services_.try_emplace(service->handle(), std::move(service));
    PW_CHECK(inserted,
             "service with handle (%#.4x) already exists",
             service->handle());
  }

  // Skip notifying the service watcher callback during initialization as it
  // will be notified in the init_cb callback.
  if (initialized_) {
    svc_watcher_(std::move(removed_service_handles),
                 std::move(added_services),
                 std::move(modified_services));
  }
}

void RemoteServiceManager::CalculateServiceChanges(
    const ServiceChangedState& service_changed,
    std::vector<ServiceMap::iterator>& removed_services,
    std::vector<ServiceData>& added_services,
    std::vector<std::pair<ServiceMap::iterator, ServiceData>>&
        modified_services) {
  // iterator to first service greater than or equal to the start of the
  // affected range.
  auto services_iter =
      services_.lower_bound(service_changed.value.range_start_handle);
  // iterator to first service greater than the end of the affected range.
  auto services_end =
      services_.upper_bound(service_changed.value.range_end_handle);
  auto new_services_iter = service_changed.services.begin();
  auto new_services_end = service_changed.services.end();

  // Iterate through the lists of services and calculate the difference.  Both
  // the old and new services are stored in ordered maps, so we can iterate
  // through both linearly in one pass.
  while (services_iter != services_end &&
         new_services_iter != new_services_end) {
    if (services_iter->first < new_services_iter->first) {
      removed_services.push_back(services_iter);
      services_iter++;
    } else if (services_iter->first == new_services_iter->first) {
      if (services_iter->second->uuid() == new_services_iter->second.type) {
        // Assume service with same handle & UUID has been modified, since all
        // services in the Service Change range must be affected, by definition:
        // "The Service Changed Characteristic Value [...] indicates the
        // beginning and ending Attribute Handles affected by an addition,
        // removal, or modification to a GATT-based service on the server" (Core
        // Spec v5.3, Vol 3, Part G, Sec 7.1).
        modified_services.emplace_back(services_iter,
                                       new_services_iter->second);
      } else {
        // A new service has been added with the same handle but different type.
        removed_services.push_back(services_iter);
        added_services.push_back(new_services_iter->second);
      }
      services_iter++;
      new_services_iter++;
    } else {
      added_services.push_back(new_services_iter->second);
      new_services_iter++;
    }
  }

  // Remaining old services must have been removed.
  while (services_iter != services_end) {
    removed_services.push_back(services_iter);
    services_iter++;
  }

  // Remaining new services must have been added.
  if (new_services_iter != new_services_end) {
    std::transform(
        new_services_iter,
        new_services_end,
        std::back_inserter(added_services),
        [](const std::map<att::Handle, ServiceData>::value_type& value) {
          return value.second;
        });
  }
}

}  // namespace bt::gatt::internal
