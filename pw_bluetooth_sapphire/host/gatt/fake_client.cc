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

#include "pw_bluetooth_sapphire/internal/host/gatt/fake_client.h"

#include <unordered_set>

#include "pw_bluetooth_sapphire/internal/host/gatt/client.h"

namespace bt::gatt::testing {

FakeClient::FakeClient(pw::async::Dispatcher& pw_dispatcher)
    : heap_dispatcher_(pw_dispatcher), weak_self_(this), weak_fake_(this) {}

uint16_t FakeClient::mtu() const {
  // TODO(armansito): Return a configurable value.
  return att::kLEMinMTU;
}

void FakeClient::ExchangeMTU(MTUCallback callback) {
  (void)heap_dispatcher_.Post(
      [mtu_status = exchange_mtu_status_,
       mtu = server_mtu_,
       callback = std::move(callback)](pw::async::Context /*ctx*/,
                                       pw::Status status) mutable {
        if (!status.ok()) {
          return;
        }

        if (mtu_status.is_error()) {
          callback(fit::error(mtu_status.error_value()));
        } else {
          callback(fit::ok(mtu));
        }
      });
}

void FakeClient::DiscoverServices(ServiceKind kind,
                                  ServiceCallback svc_callback,
                                  att::ResultFunction<> status_callback) {
  DiscoverServicesInRange(kind,
                          /*start=*/att::kHandleMin,
                          /*end=*/att::kHandleMax,
                          std::move(svc_callback),
                          std::move(status_callback));
}

void FakeClient::DiscoverServicesInRange(
    ServiceKind kind,
    att::Handle start,
    att::Handle end,
    ServiceCallback svc_callback,
    att::ResultFunction<> status_callback) {
  DiscoverServicesWithUuidsInRange(kind,
                                   start,
                                   end,
                                   std::move(svc_callback),
                                   std::move(status_callback),
                                   /*uuids=*/{});
}

void FakeClient::DiscoverServicesWithUuids(
    ServiceKind kind,
    ServiceCallback svc_callback,
    att::ResultFunction<> status_callback,
    std::vector<UUID> uuids) {
  DiscoverServicesWithUuidsInRange(kind,
                                   /*start=*/att::kHandleMin,
                                   /*end=*/att::kHandleMax,
                                   std::move(svc_callback),
                                   std::move(status_callback),
                                   std::move(uuids));
}

void FakeClient::DiscoverServicesWithUuidsInRange(
    ServiceKind kind,
    att::Handle start,
    att::Handle end,
    ServiceCallback svc_callback,
    att::ResultFunction<> status_callback,
    std::vector<UUID> uuids) {
  att::Result<> callback_status = fit::ok();
  if (discover_services_callback_) {
    callback_status = discover_services_callback_(kind);
  }

  std::unordered_set<UUID> uuids_set(uuids.cbegin(), uuids.cend());

  if (callback_status.is_ok()) {
    for (const ServiceData& svc : services_) {
      bool uuid_matches =
          uuids.empty() || uuids_set.find(svc.type) != uuids_set.end();
      if (svc.kind == kind && uuid_matches && svc.range_start >= start &&
          svc.range_start <= end) {
        (void)heap_dispatcher_.Post(
            [svc, cb = svc_callback.share()](pw::async::Context /*ctx*/,
                                             pw::Status status) {
              if (status.ok()) {
                cb(svc);
              }
            });
      }
    }
  }

  (void)heap_dispatcher_.Post(
      [callback_status, cb = std::move(status_callback)](
          pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          cb(callback_status);
        }
      });
}

void FakeClient::DiscoverCharacteristics(
    att::Handle range_start,
    att::Handle range_end,
    CharacteristicCallback chrc_callback,
    att::ResultFunction<> status_callback) {
  last_chrc_discovery_start_handle_ = range_start;
  last_chrc_discovery_end_handle_ = range_end;
  chrc_discovery_count_++;

  (void)heap_dispatcher_.Post(
      [this,
       range_start,
       range_end,
       chrc_callback = std::move(chrc_callback),
       status_callback = std::move(status_callback)](pw::async::Context /*ctx*/,
                                                     pw::Status status) {
        if (!status.ok()) {
          return;
        }
        for (const auto& chrc : chrcs_) {
          if (chrc.handle >= range_start && chrc.handle <= range_end) {
            chrc_callback(chrc);
          }
        }
        status_callback(chrc_discovery_status_);
      });
}

void FakeClient::DiscoverDescriptors(att::Handle range_start,
                                     att::Handle range_end,
                                     DescriptorCallback desc_callback,
                                     att::ResultFunction<> status_callback) {
  if (discover_descriptors_callback_) {
    discover_descriptors_callback_(range_start,
                                   range_end,
                                   std::move(desc_callback),
                                   std::move(status_callback));
    return;
  }

  last_desc_discovery_start_handle_ = range_start;
  last_desc_discovery_end_handle_ = range_end;
  desc_discovery_count_++;

  att::Result<> discovery_status = fit::ok();
  if (!desc_discovery_status_target_ ||
      desc_discovery_count_ == desc_discovery_status_target_) {
    discovery_status = desc_discovery_status_;
  }

  (void)heap_dispatcher_.Post(
      [this,
       discovery_status,
       range_start,
       range_end,
       desc_callback = std::move(desc_callback),
       status_callback = std::move(status_callback)](pw::async::Context /*ctx*/,
                                                     pw::Status status) {
        if (!status.ok()) {
          return;
        }
        for (const auto& desc : descs_) {
          if (desc.handle >= range_start && desc.handle <= range_end) {
            desc_callback(desc);
          }
        }
        status_callback(discovery_status);
      });
}

void FakeClient::ReadRequest(att::Handle handle, ReadCallback callback) {
  if (read_request_callback_) {
    read_request_callback_(handle, std::move(callback));
  }
}

void FakeClient::ReadByTypeRequest(const UUID& type,
                                   att::Handle start_handle,
                                   att::Handle end_handle,
                                   ReadByTypeCallback callback) {
  if (read_by_type_request_callback_) {
    read_by_type_request_callback_(
        type, start_handle, end_handle, std::move(callback));
  }
}

void FakeClient::ReadBlobRequest(att::Handle handle,
                                 uint16_t offset,
                                 ReadCallback callback) {
  if (read_blob_request_callback_) {
    read_blob_request_callback_(handle, offset, std::move(callback));
  }
}

void FakeClient::WriteRequest(att::Handle handle,
                              const ByteBuffer& value,
                              att::ResultFunction<> callback) {
  if (write_request_callback_) {
    write_request_callback_(handle, value, std::move(callback));
  }
}

void FakeClient::ExecutePrepareWrites(att::PrepareWriteQueue write_queue,
                                      ReliableMode reliable_mode,
                                      att::ResultFunction<> callback) {
  if (execute_prepare_writes_callback_) {
    execute_prepare_writes_callback_(
        std::move(write_queue), reliable_mode, std::move(callback));
  }
}

void FakeClient::PrepareWriteRequest(att::Handle handle,
                                     uint16_t offset,
                                     const ByteBuffer& part_value,
                                     PrepareCallback callback) {
  if (prepare_write_request_callback_) {
    prepare_write_request_callback_(
        handle, offset, part_value, std::move(callback));
  }
}
void FakeClient::ExecuteWriteRequest(att::ExecuteWriteFlag flag,
                                     att::ResultFunction<> callback) {
  if (execute_write_request_callback_) {
    execute_write_request_callback_(flag, std::move(callback));
  }
}

void FakeClient::WriteWithoutResponse(att::Handle handle,
                                      const ByteBuffer& value,
                                      att::ResultFunction<> callback) {
  if (write_without_rsp_callback_) {
    write_without_rsp_callback_(handle, value, std::move(callback));
  }
}

void FakeClient::SendNotification(bool indicate,
                                  att::Handle handle,
                                  const ByteBuffer& value,
                                  bool maybe_truncated) {
  if (notification_callback_) {
    notification_callback_(indicate, handle, value, maybe_truncated);
  }
}

void FakeClient::SetNotificationHandler(NotificationCallback callback) {
  notification_callback_ = std::move(callback);
}

}  // namespace bt::gatt::testing
