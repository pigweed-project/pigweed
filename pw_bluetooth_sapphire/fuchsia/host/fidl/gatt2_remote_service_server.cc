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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt2_remote_service_server.h"

#include <pw_assert/check.h>

#include <utility>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/measure_tape/hlcpp_measure_tape_for_read_by_type_result.h"
#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"

namespace fbg = fuchsia::bluetooth::gatt2;
namespace measure_fbg = measure_tape::fuchsia::bluetooth::gatt2;

namespace bthost {
namespace {

bt::att::ResultFunction<> MakeStatusCallback(
    bt::PeerId peer_id,
    const char* request_name,
    fbg::Handle fidl_handle,
    fit::function<void(fpromise::result<void, fbg::Error>)> callback) {
  return [peer_id, fidl_handle, callback = std::move(callback), request_name](
             bt::att::Result<> status) {
    if (bt_is_error(status,
                    INFO,
                    "fidl",
                    "%s: error (peer: %s, handle: 0x%lX)",
                    request_name,
                    bt_str(peer_id),
                    fidl_handle.value)) {
      callback(fpromise::error(
          fidl_helpers::AttErrorToGattFidlError(status.error_value())));
      return;
    }

    callback(fpromise::ok());
  };
}

fbg::Characteristic CharacteristicToFidl(
    const bt::gatt::CharacteristicData& characteristic,
    const std::map<bt::gatt::DescriptorHandle, bt::gatt::DescriptorData>&
        descriptors) {
  fbg::Characteristic fidl_char;
  fidl_char.set_handle(fbg::Handle{characteristic.value_handle});
  fidl_char.set_type(fuchsia::bluetooth::Uuid{characteristic.type.value()});

  // The FIDL property bitfield combines the properties and extended properties
  // bits. We mask away the kExtendedProperties property.
  constexpr uint8_t kRemoveExtendedPropertiesMask = 0x7F;
  fbg::CharacteristicPropertyBits fidl_properties =
      static_cast<fbg::CharacteristicPropertyBits>(
          characteristic.properties & kRemoveExtendedPropertiesMask);
  if (characteristic.extended_properties) {
    if (*characteristic.extended_properties &
        bt::gatt::ExtendedProperty::kReliableWrite) {
      fidl_properties |= fbg::CharacteristicPropertyBits::RELIABLE_WRITE;
    }
    if (*characteristic.extended_properties &
        bt::gatt::ExtendedProperty::kWritableAuxiliaries) {
      fidl_properties |= fbg::CharacteristicPropertyBits::WRITABLE_AUXILIARIES;
    }
  }
  fidl_char.set_properties(fidl_properties);

  if (!descriptors.empty()) {
    std::vector<fbg::Descriptor> fidl_descriptors;
    for (const auto& [handle, data] : descriptors) {
      fbg::Descriptor fidl_descriptor;
      fidl_descriptor.set_handle(fbg::Handle{handle.value});
      fidl_descriptor.set_type(fuchsia::bluetooth::Uuid{data.type.value()});
      fidl_descriptors.push_back(std::move(fidl_descriptor));
    }
    fidl_char.set_descriptors(std::move(fidl_descriptors));
  }

  return fidl_char;
}

// Returned result is supposed to match Read{Characteristic, Descriptor}Callback
// (result type is converted by FIDL move constructor).
[[nodiscard]] fpromise::result<::fuchsia::bluetooth::gatt2::ReadValue,
                               ::fuchsia::bluetooth::gatt2::Error>
ReadResultToFidl(bt::PeerId peer_id,
                 fbg::Handle handle,
                 bt::att::Result<> status,
                 const bt::ByteBuffer& value,
                 bool maybe_truncated,
                 const char* request) {
  if (bt_is_error(status,
                  INFO,
                  "fidl",
                  "%s: error (peer: %s, handle: 0x%lX)",
                  request,
                  bt_str(peer_id),
                  handle.value)) {
    return fpromise::error(
        fidl_helpers::AttErrorToGattFidlError(status.error_value()));
  }

  fbg::ReadValue fidl_value;
  fidl_value.set_handle(handle);
  fidl_value.set_value(value.ToVector());
  fidl_value.set_maybe_truncated(maybe_truncated);
  return fpromise::ok(std::move(fidl_value));
}

void FillInReadOptionsDefaults(fbg::ReadOptions& options) {
  if (options.is_short_read()) {
    return;
  }
  if (!options.long_read().has_offset()) {
    options.long_read().set_offset(0);
  }
  if (!options.long_read().has_max_bytes()) {
    options.long_read().set_max_bytes(fbg::MAX_VALUE_LENGTH);
  }
}

void FillInDefaultWriteOptions(fbg::WriteOptions& options) {
  if (!options.has_write_mode()) {
    *options.mutable_write_mode() = fbg::WriteMode::DEFAULT;
  }
  if (!options.has_offset()) {
    *options.mutable_offset() = 0;
  }
}

bt::gatt::ReliableMode ReliableModeFromFidl(const fbg::WriteMode& mode) {
  return mode == fbg::WriteMode::RELIABLE ? bt::gatt::ReliableMode::kEnabled
                                          : bt::gatt::ReliableMode::kDisabled;
}

}  // namespace

Gatt2RemoteServiceServer::Gatt2RemoteServiceServer(
    bt::gatt::RemoteService::WeakPtr service,
    bt::gatt::GATT::WeakPtr gatt,
    bt::PeerId peer_id,
    fidl::InterfaceRequest<fuchsia::bluetooth::gatt2::RemoteService> request)
    : GattServerBase(std::move(gatt), this, std::move(request)),
      service_(std::move(service)),
      peer_id_(peer_id),
      weak_self_(this) {}

Gatt2RemoteServiceServer::~Gatt2RemoteServiceServer() {
  // Disable all notifications to prevent leaks.
  for (auto& [_, notifier] : characteristic_notifiers_) {
    service_->DisableNotifications(notifier.characteristic_handle,
                                   notifier.handler_id,
                                   /*status_callback=*/[](auto /*status*/) {});
  }
  characteristic_notifiers_.clear();
}

void Gatt2RemoteServiceServer::Close(zx_status_t status) {
  binding()->Close(status);
}

void Gatt2RemoteServiceServer::DiscoverCharacteristics(
    DiscoverCharacteristicsCallback callback) {
  auto res_cb = [callback = std::move(callback)](
                    bt::att::Result<> status,
                    const bt::gatt::CharacteristicMap& characteristics) {
    if (status.is_error()) {
      callback({});
      return;
    }

    std::vector<fbg::Characteristic> fidl_characteristics;
    for (const auto& [_, characteristic] : characteristics) {
      const auto& [data, descriptors] = characteristic;
      fidl_characteristics.push_back(CharacteristicToFidl(data, descriptors));
    }
    callback(std::move(fidl_characteristics));
  };

  service_->DiscoverCharacteristics(std::move(res_cb));
}

void Gatt2RemoteServiceServer::ReadByType(::fuchsia::bluetooth::Uuid uuid,
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

        if (status == ToResult(bt::HostError::kInvalidParameters)) {
          bt_log(WARN,
                 "fidl",
                 "%s: called with invalid parameters (peer: %s)",
                 func,
                 bt_str(self->peer_id_));
          cb(fpromise::error(fbg::Error::INVALID_PARAMETERS));
          return;
        } else if (status == ToResult(bt::HostError::kTooManyResults)) {
          // RemoteService::ReadByType() fails with kTooManyResults when the
          // peer returns more than RemoteService::kMaxReadByTypeResults
          // results.
          bt_log(INFO,
                 "fidl",
                 "%s: too many results (peer: %s)",
                 func,
                 bt_str(self->peer_id_));
          cb(fpromise::error(fbg::Error::TOO_MANY_RESULTS));
          return;
        } else if (status.is_error()) {
          cb(fpromise::error(fbg::Error::UNLIKELY_ERROR));
          return;
        }

        const size_t kVectorOverhead =
            sizeof(fidl_message_header_t) + sizeof(fidl_vector_t);
        const size_t kMaxBytes = ZX_CHANNEL_MAX_MSG_BYTES - kVectorOverhead;
        size_t bytes_used = 0;

        std::vector<fuchsia::bluetooth::gatt2::ReadByTypeResult> fidl_results;
        fidl_results.reserve(results.size());

        for (const bt::gatt::RemoteService::ReadByTypeResult& result :
             results) {
          fuchsia::bluetooth::gatt2::ReadByTypeResult fidl_result;
          fidl_result.set_handle(fbg::Handle{result.handle.value});
          if (result.result.is_ok()) {
            fbg::ReadValue read_value;
            read_value.set_handle(fbg::Handle{result.handle.value});
            read_value.set_value(result.result.value()->ToVector());
            read_value.set_maybe_truncated(result.maybe_truncated);
            fidl_result.set_value(std::move(read_value));
          } else {
            fidl_result.set_error(fidl_helpers::AttErrorToGattFidlError(
                bt::att::Error(result.result.error_value())));
          }

          measure_fbg::Size result_size = measure_fbg::Measure(fidl_result);
          PW_CHECK(result_size.num_handles == 0);
          bytes_used += result_size.num_bytes;

          if (bytes_used > kMaxBytes) {
            cb(fpromise::error(
                fuchsia::bluetooth::gatt2::Error::TOO_MANY_RESULTS));
            return;
          }

          fidl_results.push_back(std::move(fidl_result));
        }

        cb(fpromise::ok(std::move(fidl_results)));
      });
}

void Gatt2RemoteServiceServer::ReadCharacteristic(
    fbg::Handle fidl_handle,
    fbg::ReadOptions options,
    ReadCharacteristicCallback callback) {
  if (!fidl_helpers::IsFidlGattHandleValid(fidl_handle)) {
    callback(fpromise::error(fbg::Error::INVALID_HANDLE));
    return;
  }
  bt::gatt::CharacteristicHandle handle(
      static_cast<bt::att::Handle>(fidl_handle.value));

  FillInReadOptionsDefaults(options);

  const char* kRequestName = __FUNCTION__;
  bt::gatt::RemoteService::ReadValueCallback read_cb =
      [peer_id = peer_id_,
       fidl_handle,
       kRequestName,
       callback = std::move(callback)](bt::att::Result<> status,
                                       const bt::ByteBuffer& value,
                                       bool maybe_truncated) {
        callback(ReadResultToFidl(peer_id,
                                  fidl_handle,
                                  status,
                                  value,
                                  maybe_truncated,
                                  kRequestName));
      };

  if (options.is_short_read()) {
    service_->ReadCharacteristic(handle, std::move(read_cb));
    return;
  }

  service_->ReadLongCharacteristic(handle,
                                   options.long_read().offset(),
                                   options.long_read().max_bytes(),
                                   std::move(read_cb));
}

void Gatt2RemoteServiceServer::WriteCharacteristic(
    fbg::Handle fidl_handle,
    std::vector<uint8_t> value,
    fbg::WriteOptions options,
    WriteCharacteristicCallback callback) {
  if (!fidl_helpers::IsFidlGattHandleValid(fidl_handle)) {
    callback(fpromise::error(fbg::Error::INVALID_HANDLE));
    return;
  }
  bt::gatt::CharacteristicHandle handle(
      static_cast<bt::att::Handle>(fidl_handle.value));

  FillInDefaultWriteOptions(options);

  bt::att::ResultFunction<> write_cb = MakeStatusCallback(
      peer_id_, __FUNCTION__, fidl_handle, std::move(callback));

  if (options.write_mode() == fbg::WriteMode::WITHOUT_RESPONSE) {
    if (options.offset() != 0) {
      write_cb(bt::ToResult(bt::HostError::kInvalidParameters));
      return;
    }
    service_->WriteCharacteristicWithoutResponse(
        handle, std::move(value), std::move(write_cb));
    return;
  }

  const uint16_t kMaxShortWriteValueLength =
      service_->att_mtu() - sizeof(bt::att::OpCode) -
      sizeof(bt::att::WriteRequestParams);
  if (options.offset() == 0 &&
      options.write_mode() == fbg::WriteMode::DEFAULT &&
      value.size() <= kMaxShortWriteValueLength) {
    service_->WriteCharacteristic(
        handle, std::move(value), std::move(write_cb));
    return;
  }

  service_->WriteLongCharacteristic(handle,
                                    options.offset(),
                                    std::move(value),
                                    ReliableModeFromFidl(options.write_mode()),
                                    std::move(write_cb));
}

void Gatt2RemoteServiceServer::ReadDescriptor(
    ::fuchsia::bluetooth::gatt2::Handle fidl_handle,
    ::fuchsia::bluetooth::gatt2::ReadOptions options,
    ReadDescriptorCallback callback) {
  if (!fidl_helpers::IsFidlGattHandleValid(fidl_handle)) {
    callback(fpromise::error(fbg::Error::INVALID_HANDLE));
    return;
  }
  bt::gatt::DescriptorHandle handle(
      static_cast<bt::att::Handle>(fidl_handle.value));

  FillInReadOptionsDefaults(options);

  const char* kRequestName = __FUNCTION__;
  bt::gatt::RemoteService::ReadValueCallback read_cb =
      [peer_id = peer_id_,
       fidl_handle,
       kRequestName,
       callback = std::move(callback)](bt::att::Result<> status,
                                       const bt::ByteBuffer& value,
                                       bool maybe_truncated) {
        callback(ReadResultToFidl(peer_id,
                                  fidl_handle,
                                  status,
                                  value,
                                  maybe_truncated,
                                  kRequestName));
      };

  if (options.is_short_read()) {
    service_->ReadDescriptor(handle, std::move(read_cb));
    return;
  }

  service_->ReadLongDescriptor(handle,
                               options.long_read().offset(),
                               options.long_read().max_bytes(),
                               std::move(read_cb));
}

void Gatt2RemoteServiceServer::WriteDescriptor(
    fbg::Handle fidl_handle,
    std::vector<uint8_t> value,
    fbg::WriteOptions options,
    WriteDescriptorCallback callback) {
  if (!fidl_helpers::IsFidlGattHandleValid(fidl_handle)) {
    callback(fpromise::error(fbg::Error::INVALID_HANDLE));
    return;
  }
  bt::gatt::DescriptorHandle handle(
      static_cast<bt::att::Handle>(fidl_handle.value));

  FillInDefaultWriteOptions(options);

  bt::att::ResultFunction<> write_cb = MakeStatusCallback(
      peer_id_, __FUNCTION__, fidl_handle, std::move(callback));

  // WITHOUT_RESPONSE and RELIABLE write modes are not supported for
  // descriptors.
  if (options.write_mode() == fbg::WriteMode::WITHOUT_RESPONSE ||
      options.write_mode() == fbg::WriteMode::RELIABLE) {
    write_cb(bt::ToResult(bt::HostError::kInvalidParameters));
    return;
  }

  const uint16_t kMaxShortWriteValueLength =
      service_->att_mtu() - sizeof(bt::att::OpCode) -
      sizeof(bt::att::WriteRequestParams);
  if (options.offset() == 0 && value.size() <= kMaxShortWriteValueLength) {
    service_->WriteDescriptor(handle, std::move(value), std::move(write_cb));
    return;
  }

  service_->WriteLongDescriptor(
      handle, options.offset(), std::move(value), std::move(write_cb));
}

void Gatt2RemoteServiceServer::RegisterCharacteristicNotifier(
    fbg::Handle fidl_handle,
    fidl::InterfaceHandle<fbg::CharacteristicNotifier> notifier_handle,
    RegisterCharacteristicNotifierCallback callback) {
  bt::gatt::CharacteristicHandle char_handle(
      static_cast<bt::att::Handle>(fidl_handle.value));
  NotifierId notifier_id = next_notifier_id_++;
  auto self = weak_self_.GetWeakPtr();

  auto value_cb = [self, notifier_id, fidl_handle](const bt::ByteBuffer& value,
                                                   bool maybe_truncated) {
    if (!self.is_alive()) {
      return;
    }

    auto notifier_iter = self->characteristic_notifiers_.find(notifier_id);
    // The lower layers guarantee that the status callback is always invoked
    // before sending notifications. Notifiers are only removed during
    // destruction (addressed by previous `self` check) and in the
    // `DisableNotifications` completion callback in
    // `OnCharacteristicNotifierError`, so no notifications should be received
    // after removing a notifier.
    PW_CHECK(
        notifier_iter != self->characteristic_notifiers_.end(),
        "characteristic notification value received after notifier unregistered"
        "(peer: %s, characteristic: 0x%lX) ",
        bt_str(self->peer_id_),
        fidl_handle.value);
    CharacteristicNotifier& notifier = notifier_iter->second;

    // The `- 1` is needed because there is one unacked notification that we've
    // already sent to the client aside from the values in the queue.
    if (notifier.queued_values.size() == kMaxPendingNotifierValues - 1) {
      bt_log(WARN,
             "fidl",
             "GATT CharacteristicNotifier pending values limit reached, "
             "closing protocol (peer: "
             "%s, characteristic: %#.2x)",
             bt_str(self->peer_id_),
             notifier.characteristic_handle.value);
      self->OnCharacteristicNotifierError(
          notifier_id, notifier.characteristic_handle, notifier.handler_id);
      return;
    }

    fbg::ReadValue fidl_value;
    fidl_value.set_handle(fidl_handle);
    fidl_value.set_value(value.ToVector());
    fidl_value.set_maybe_truncated(maybe_truncated);

    bt_log(TRACE,
           "fidl",
           "Queueing GATT notification value (characteristic: %#.2x)",
           notifier.characteristic_handle.value);
    notifier.queued_values.push(std::move(fidl_value));

    self->MaybeNotifyNextValue(notifier_id);
  };

  auto status_cb = [self,
                    service = service_,
                    char_handle,
                    notifier_id,
                    notifier_handle = std::move(notifier_handle),
                    callback = std::move(callback)](
                       bt::att::Result<> status,
                       bt::gatt::IdType handler_id) mutable {
    if (!self.is_alive()) {
      if (status.is_ok()) {
        // Disable this handler so it doesn't leak.
        service->DisableNotifications(
            char_handle, handler_id, [](auto /*status*/) {
              // There is no notifier to clean up because the server has been
              // destroyed.
            });
      }
      return;
    }

    if (status.is_error()) {
      callback(fpromise::error(
          fidl_helpers::AttErrorToGattFidlError(status.error_value())));
      return;
    }

    CharacteristicNotifier notifier{.handler_id = handler_id,
                                    .characteristic_handle = char_handle,
                                    .notifier = notifier_handle.Bind()};
    auto [notifier_iter, emplaced] = self->characteristic_notifiers_.emplace(
        notifier_id, std::move(notifier));
    PW_CHECK(emplaced);

    // When the client closes the protocol, unregister the notifier.
    notifier_iter->second.notifier.set_error_handler(
        [self, char_handle, handler_id, notifier_id](auto /*status*/) {
          self->OnCharacteristicNotifierError(
              notifier_id, char_handle, handler_id);
        });

    callback(fpromise::ok());
  };

  service_->EnableNotifications(
      char_handle, std::move(value_cb), std::move(status_cb));
}

void Gatt2RemoteServiceServer::MaybeNotifyNextValue(NotifierId notifier_id) {
  auto notifier_iter = characteristic_notifiers_.find(notifier_id);
  if (notifier_iter == characteristic_notifiers_.end()) {
    return;
  }
  CharacteristicNotifier& notifier = notifier_iter->second;

  if (notifier.queued_values.empty()) {
    return;
  }

  if (!notifier.last_value_ack) {
    return;
  }
  notifier.last_value_ack = false;

  fbg::ReadValue value = std::move(notifier.queued_values.front());
  notifier.queued_values.pop();

  bt_log(DEBUG,
         "fidl",
         "Sending GATT notification value (handle: 0x%lX)",
         value.handle().value);
  auto self = weak_self_.GetWeakPtr();
  notifier.notifier->OnNotification(std::move(value), [self, notifier_id]() {
    if (!self.is_alive()) {
      return;
    }

    auto notifier_iter = self->characteristic_notifiers_.find(notifier_id);
    if (notifier_iter == self->characteristic_notifiers_.end()) {
      return;
    }
    notifier_iter->second.last_value_ack = true;
    self->MaybeNotifyNextValue(notifier_id);
  });
}

void Gatt2RemoteServiceServer::OnCharacteristicNotifierError(
    NotifierId notifier_id,
    bt::gatt::CharacteristicHandle char_handle,
    bt::gatt::IdType handler_id) {
  auto self = weak_self_.GetWeakPtr();
  service_->DisableNotifications(
      char_handle, handler_id, [self, notifier_id](auto /*status*/) {
        if (!self.is_alive()) {
          return;
        }
        // Clear the notifier regardless of status. Wait until this callback is
        // called in order to prevent the value callback from being called for
        // an erased notifier.
        self->characteristic_notifiers_.erase(notifier_id);
      });
}

}  // namespace bthost
