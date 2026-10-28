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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt_remote_service_server.h"

#include <pw_assert/check.h>

#include <algorithm>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"

using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Status;
using fuchsia::bluetooth::gatt::Characteristic;
using fuchsia::bluetooth::gatt::CharacteristicPtr;
using fuchsia::bluetooth::gatt::Descriptor;
using fuchsia::bluetooth::gatt::WriteOptions;

using bt::ByteBuffer;
using bt::MutableBufferView;
using bt::gatt::CharacteristicData;
using bt::gatt::CharacteristicHandle;
using bt::gatt::DescriptorData;
using bt::gatt::DescriptorHandle;
using bthost::fidl_helpers::CharacteristicHandleFromFidl;
using bthost::fidl_helpers::DescriptorHandleFromFidl;

namespace bthost {
namespace {

// We mask away the "extended properties" property. We expose extended
// properties in the same bitfield.
constexpr uint8_t kPropertyMask = 0x7F;

Characteristic CharacteristicToFidl(
    const CharacteristicData& characteristic,
    const std::map<DescriptorHandle, DescriptorData>& descriptors) {
  Characteristic fidl_char;
  fidl_char.id = static_cast<uint64_t>(characteristic.value_handle);
  fidl_char.type = characteristic.type.ToString();
  fidl_char.properties =
      static_cast<uint16_t>(characteristic.properties & kPropertyMask);
  fidl_char.descriptors.emplace();  // initialize an empty vector

  // TODO(armansito): Add extended properties.

  for (const auto& [id, descr] : descriptors) {
    Descriptor fidl_descr;
    fidl_descr.id = static_cast<uint64_t>(id.value);
    fidl_descr.type = descr.type.ToString();
    fidl_char.descriptors->push_back(std::move(fidl_descr));
  }

  return fidl_char;
}

void NopStatusCallback(bt::att::Result<>) {}

}  // namespace

GattRemoteServiceServer::GattRemoteServiceServer(
    bt::gatt::RemoteService::WeakPtr service,
    bt::gatt::GATT::WeakPtr gatt,
    bt::PeerId peer_id,
    fidl::InterfaceRequest<fuchsia::bluetooth::gatt::RemoteService> request)
    : GattServerBase(gatt, this, std::move(request)),
      service_(std::move(service)),
      peer_id_(peer_id),
      weak_self_(this) {
  PW_DCHECK(service_.is_alive());
}

GattRemoteServiceServer::~GattRemoteServiceServer() {
  for (const auto& iter : notify_handlers_) {
    if (iter.second != bt::gatt::kInvalidId) {
      service_->DisableNotifications(
          iter.first, iter.second, NopStatusCallback);
    }
  }
}

void GattRemoteServiceServer::DiscoverCharacteristics(
    DiscoverCharacteristicsCallback callback) {
  auto res_cb = [callback = std::move(callback)](bt::att::Result<> status,
                                                 const auto& chrcs) {
    std::vector<Characteristic> fidl_chrcs;
    if (status.is_ok()) {
      for (const auto& [id, chrc] : chrcs) {
        auto& [chr, descs] = chrc;
        fidl_chrcs.push_back(CharacteristicToFidl(chr, descs));
      }
    }

    callback(fidl_helpers::ResultToFidlDeprecated(status, ""),
             std::move(fidl_chrcs));
  };

  service_->DiscoverCharacteristics(std::move(res_cb));
}

void GattRemoteServiceServer::ReadCharacteristic(
    uint64_t id, ReadCharacteristicCallback callback) {
  auto cb = [callback = std::move(callback)](
                bt::att::Result<> status, const bt::ByteBuffer& value, auto) {
    // We always reply with a non-null value.
    std::vector<uint8_t> vec;

    if (status.is_ok() && value.size()) {
      vec.resize(value.size());

      MutableBufferView vec_view(vec.data(), vec.size());
      value.Copy(&vec_view);
    }

    callback(fidl_helpers::ResultToFidlDeprecated(status), std::move(vec));
  };

  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->ReadCharacteristic(CharacteristicHandleFromFidl(id), std::move(cb));
}

void GattRemoteServiceServer::ReadLongCharacteristic(
    uint64_t id,
    uint16_t offset,
    uint16_t max_bytes,
    ReadLongCharacteristicCallback callback) {
  auto cb = [callback = std::move(callback)](
                bt::att::Result<> status, const bt::ByteBuffer& value, auto) {
    // We always reply with a non-null value.
    std::vector<uint8_t> vec;

    if (status.is_ok() && value.size()) {
      vec.resize(value.size());

      MutableBufferView vec_view(vec.data(), vec.size());
      value.Copy(&vec_view);
    }

    callback(fidl_helpers::ResultToFidlDeprecated(status), std::move(vec));
  };

  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->ReadLongCharacteristic(
      CharacteristicHandleFromFidl(id), offset, max_bytes, std::move(cb));
}

void GattRemoteServiceServer::WriteCharacteristic(
    uint64_t id,
    ::std::vector<uint8_t> value,
    WriteCharacteristicCallback callback) {
  auto cb = [callback = std::move(callback)](bt::att::Result<> status) {
    callback(fidl_helpers::ResultToFidlDeprecated(status, ""));
  };

  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->WriteCharacteristic(
      CharacteristicHandleFromFidl(id), std::move(value), std::move(cb));
}

void GattRemoteServiceServer::WriteLongCharacteristic(
    uint64_t id,
    uint16_t offset,
    ::std::vector<uint8_t> value,
    WriteOptions write_options,
    WriteLongCharacteristicCallback callback) {
  auto cb = [callback = std::move(callback)](bt::att::Result<> status) {
    callback(fidl_helpers::ResultToFidlDeprecated(status, ""));
  };

  auto reliable_mode = fidl_helpers::ReliableModeFromFidl(write_options);
  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->WriteLongCharacteristic(CharacteristicHandleFromFidl(id),
                                    offset,
                                    std::move(value),
                                    std::move(reliable_mode),
                                    std::move(cb));
}

void GattRemoteServiceServer::WriteCharacteristicWithoutResponse(
    uint64_t id, ::std::vector<uint8_t> value) {
  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->WriteCharacteristicWithoutResponse(CharacteristicHandleFromFidl(id),
                                               std::move(value),
                                               /*cb=*/[](auto) {});
}

void GattRemoteServiceServer::ReadDescriptor(uint64_t id,
                                             ReadDescriptorCallback callback) {
  auto cb = [callback = std::move(callback)](
                bt::att::Result<> status, const bt::ByteBuffer& value, auto) {
    // We always reply with a non-null value.
    std::vector<uint8_t> vec;

    if (status.is_ok() && value.size()) {
      vec.resize(value.size());

      MutableBufferView vec_view(vec.data(), vec.size());
      value.Copy(&vec_view);
    }

    callback(fidl_helpers::ResultToFidlDeprecated(status), std::move(vec));
  };

  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->ReadDescriptor(DescriptorHandleFromFidl(id), std::move(cb));
}

void GattRemoteServiceServer::ReadLongDescriptor(
    uint64_t id,
    uint16_t offset,
    uint16_t max_bytes,
    ReadLongDescriptorCallback callback) {
  auto cb = [callback = std::move(callback)](
                bt::att::Result<> status, const bt::ByteBuffer& value, auto) {
    // We always reply with a non-null value.
    std::vector<uint8_t> vec;

    if (status.is_ok() && value.size()) {
      vec.resize(value.size());

      MutableBufferView vec_view(vec.data(), vec.size());
      value.Copy(&vec_view);
    }

    callback(fidl_helpers::ResultToFidlDeprecated(status), std::move(vec));
  };

  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->ReadLongDescriptor(
      DescriptorHandleFromFidl(id), offset, max_bytes, std::move(cb));
}

void GattRemoteServiceServer::WriteDescriptor(
    uint64_t id,
    ::std::vector<uint8_t> value,
    WriteDescriptorCallback callback) {
  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->WriteDescriptor(
      DescriptorHandleFromFidl(id),
      std::move(value),
      [callback = std::move(callback)](bt::att::Result<> status) {
        callback(fidl_helpers::ResultToFidlDeprecated(status, ""));
      });
}

void GattRemoteServiceServer::WriteLongDescriptor(
    uint64_t id,
    uint16_t offset,
    ::std::vector<uint8_t> value,
    WriteLongDescriptorCallback callback) {
  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  service_->WriteLongDescriptor(
      DescriptorHandleFromFidl(id),
      offset,
      std::move(value),
      [callback = std::move(callback)](bt::att::Result<> status) {
        callback(fidl_helpers::ResultToFidlDeprecated(status, ""));
      });
}

void GattRemoteServiceServer::ReadByType(fuchsia::bluetooth::Uuid uuid,
                                         ReadByTypeCallback callback) {
  service_->ReadByType(
      fidl_helpers::UuidFromFidl(uuid),
      [self = weak_self_.GetWeakPtr(),
       cb = std::move(callback),
       func = __FUNCTION__](
          bt::att::Result<> status,
          std::vector<bt::gatt::RemoteService::ReadByTypeResult> results) {
        if (!self.is_alive()) {
          return;
        }

        if (status.is_error()) {
          if (status.error_value().is(bt::HostError::kInvalidParameters)) {
            bt_log(WARN,
                   "fidl",
                   "%s: called with invalid parameters, closing FIDL channel "
                   "(peer: %s)",
                   func,
                   bt_str(self->peer_id_));
            self->binding()->Close(ZX_ERR_INVALID_ARGS);
            return;
          }
          if (status.error_value().is(bt::HostError::kTooManyResults)) {
            // RemoteService::ReadByType() fails with kTooManyResults when the
            // peer returns more than RemoteService::kMaxReadByTypeResults
            // results.
            bt_log(INFO,
                   "fidl",
                   "%s: too many results (peer: %s)",
                   func,
                   bt_str(self->peer_id_));
            cb(fpromise::error(
                fuchsia::bluetooth::gatt::Error::TOO_MANY_RESULTS));
            return;
          }
          cb(fpromise::error(
              fidl_helpers::GattErrorToFidl(status.error_value())));
          return;
        }

        if (results.size() >
            fuchsia::bluetooth::gatt::MAX_READ_BY_TYPE_RESULTS) {
          cb(fpromise::error(
              fuchsia::bluetooth::gatt::Error::TOO_MANY_RESULTS));
          return;
        }

        std::vector<fuchsia::bluetooth::gatt::ReadByTypeResult> fidl_results;
        fidl_results.reserve(results.size());

        for (const auto& result : results) {
          fuchsia::bluetooth::gatt::ReadByTypeResult fidl_result;
          fidl_result.set_id(static_cast<uint64_t>(result.handle.value));
          if (result.result.is_ok()) {
            fidl_result.set_value(result.result.value()->ToVector());
          } else {
            fidl_result.set_error(fidl_helpers::GattErrorToFidl(
                bt::att::Error(result.result.error_value())));
          }
          fidl_results.push_back(std::move(fidl_result));
        }

        cb(fpromise::ok(std::move(fidl_results)));
      });
}

void GattRemoteServiceServer::NotifyCharacteristic(
    uint64_t id, bool enable, NotifyCharacteristicCallback callback) {
  // TODO(fxbug.dev/42141942): The 64 bit `id` can overflow the 16 bits of a
  // bt::att:Handle. Fix this.
  auto handle = CharacteristicHandleFromFidl(id);
  if (!enable) {
    auto iter = notify_handlers_.find(handle);
    if (iter == notify_handlers_.end()) {
      callback(fidl_helpers::NewFidlError(ErrorCode::NOT_FOUND,
                                          "characteristic not notifying"));
      return;
    }

    if (iter->second == bt::gatt::kInvalidId) {
      callback(fidl_helpers::NewFidlError(
          ErrorCode::IN_PROGRESS,
          "characteristic notification registration pending"));
      return;
    }

    service_->DisableNotifications(
        handle,
        iter->second,
        [callback = std::move(callback)](bt::att::Result<> status) {
          callback(fidl_helpers::ResultToFidlDeprecated(status, ""));
        });
    notify_handlers_.erase(iter);

    return;
  }

  if (notify_handlers_.count(handle) != 0) {
    callback(fidl_helpers::NewFidlError(ErrorCode::ALREADY,
                                        "characteristic already notifying"));
    return;
  }

  // Prevent any races and leaks by marking a notification is in progress
  notify_handlers_[handle] = bt::gatt::kInvalidId;

  auto self = weak_self_.GetWeakPtr();
  auto value_cb = [self, id](const ByteBuffer& value,
                             bool /*maybe_truncated*/) {
    if (!self.is_alive())
      return;

    self->binding()->events().OnCharacteristicValueUpdated(id,
                                                           value.ToVector());
  };

  auto status_cb =
      [self, svc = service_, handle, callback = std::move(callback)](
          bt::att::Result<> status, HandlerId handler_id) {
        if (!self.is_alive()) {
          if (status.is_ok()) {
            // Disable this handler so it doesn't leak.
            svc->DisableNotifications(handle, handler_id, NopStatusCallback);
          }

          callback(fidl_helpers::NewFidlError(ErrorCode::FAILED, "canceled"));
          return;
        }

        if (status.is_ok()) {
          PW_DCHECK(handler_id != bt::gatt::kInvalidId);
          PW_DCHECK(self->notify_handlers_.count(handle) == 1u);
          PW_DCHECK(self->notify_handlers_[handle] == bt::gatt::kInvalidId);
          self->notify_handlers_[handle] = handler_id;
        } else {
          // Remove our handle holder.
          self->notify_handlers_.erase(handle);
        }

        callback(fidl_helpers::ResultToFidlDeprecated(status, ""));
      };

  service_->EnableNotifications(
      handle, std::move(value_cb), std::move(status_cb));
}

}  // namespace bthost
