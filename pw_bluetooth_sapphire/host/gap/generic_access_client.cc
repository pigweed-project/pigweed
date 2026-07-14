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

#include "pw_bluetooth_sapphire/internal/host/gap/generic_access_client.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"

namespace bt::gap::internal {

GenericAccessClient::GenericAccessClient(PeerId peer_id,
                                         gatt::RemoteService::WeakPtr service)
    : WeakSelf(this), service_(std::move(service)), peer_id_(peer_id) {
  PW_CHECK(service_.is_alive());
  PW_CHECK(service_->uuid() == kGenericAccessService);
}

void GenericAccessClient::ReadDeviceName(DeviceNameCallback callback) {
  if (!service_.is_alive()) {
    callback(ToResult(HostError::kFailed).take_error());
    return;
  }

  service_->DiscoverCharacteristics(
      [self = GetWeakPtr(), cb = std::move(callback)](
          att::Result<> result, const gatt::CharacteristicMap& chars) mutable {
        if (!self.is_alive()) {
          return;
        }

        if (result.is_error()) {
          cb(result.take_error());
          return;
        }

        std::optional<gatt::CharacteristicHandle> device_name_value_handle;
        for (auto& [handle, chr] : chars) {
          auto& data = chr.first;
          if (data.type == kDeviceNameCharacteristic) {
            device_name_value_handle.emplace(data.value_handle);
            break;
          }
        }

        if (!device_name_value_handle) {
          bt_log(DEBUG,
                 "gap-le",
                 "GAP service does not have device name characteristic "
                 "(peer: %s)",
                 bt_str(self->peer_id_));
          cb(ToResult(HostError::kNotFound).take_error());
          return;
        }

        if (!self->service_.is_alive()) {
          cb(ToResult(HostError::kFailed).take_error());
          return;
        }

        // according to Core Spec v5.3, Vol 3, Part C, 12.1: "0 to 248 octets in
        // length"
        self->service_->ReadLongCharacteristic(
            *device_name_value_handle,
            /*offset=*/0,
            att::kMaxAttributeValueLength,
            [self, device_name_cb = std::move(cb)](
                att::Result<> discover_result,
                const ByteBuffer& buffer,
                bool /*maybe_truncated*/) mutable {
              if (!self.is_alive()) {
                return;
              }

              if (bt_is_error(
                      discover_result,
                      DEBUG,
                      "gap-le",
                      "error reading device name characteristic (peer: %s)",
                      bt_str(self->peer_id_))) {
                device_name_cb(discover_result.take_error());
                return;
              }

              const auto device_name_end =
                  std::find(buffer.begin(), buffer.end(), '\0');
              device_name_cb(
                  fit::ok(std::string(buffer.begin(), device_name_end)));
            });
      });
}

void GenericAccessClient::ReadAppearance(AppearanceCallback callback) {
  if (!service_.is_alive()) {
    callback(ToResult(HostError::kFailed).take_error());
    return;
  }

  service_->DiscoverCharacteristics([self = GetWeakPtr(),
                                     cb = std::move(callback)](
                                        att::Result<> result,
                                        const gatt::CharacteristicMap&
                                            chars) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (result.is_error()) {
      cb(result.take_error());
      return;
    }

    std::optional<gatt::CharacteristicHandle> appearance_value_handle;
    for (auto& [handle, chr] : chars) {
      auto& data = chr.first;
      if (data.type == kAppearanceCharacteristic) {
        appearance_value_handle.emplace(data.value_handle);
        break;
      }
    }

    if (!appearance_value_handle) {
      bt_log(DEBUG,
             "gap-le",
             "GAP service does not have appearance characteristic "
             "(peer: %s)",
             bt_str(self->peer_id_));
      cb(ToResult(HostError::kNotFound).take_error());
      return;
    }

    if (!self->service_.is_alive()) {
      cb(ToResult(HostError::kFailed).take_error());
      return;
    }

    // according to Core Spec v5.3, Vol 3, Part C, 12.2: "2 octets in length"
    self->service_->ReadCharacteristic(
        *appearance_value_handle,
        [self, appearance_cb = std::move(cb)](
            att::Result<> discover_result,
            const ByteBuffer& buffer,
            bool /*maybe_truncated*/) mutable {
          if (!self.is_alive()) {
            return;
          }

          if (bt_is_error(discover_result,
                          DEBUG,
                          "gap-le",
                          "error reading appearance characteristic (peer: %s)",
                          bt_str(self->peer_id_))) {
            appearance_cb(discover_result.take_error());
            return;
          }

          if (buffer.size() != sizeof(uint16_t)) {
            bt_log(
                DEBUG,
                "gap-le",
                "appearance characteristic has invalid value size (peer: %s)",
                bt_str(self->peer_id_));
            appearance_cb(ToResult(HostError::kPacketMalformed).take_error());
            return;
          }

          uint16_t char_value = pw::bytes::ConvertOrderFrom(
              cpp20::endian::little, buffer.template To<uint16_t>());
          appearance_cb(fit::ok(char_value));
        });
  });
}

void GenericAccessClient::ReadPeripheralPreferredConnectionParameters(
    ConnectionParametersCallback callback) {
  if (!service_.is_alive()) {
    callback(ToResult(HostError::kFailed).take_error());
    return;
  }

  service_->DiscoverCharacteristics([self = GetWeakPtr(),
                                     cb = std::move(callback)](
                                        att::Result<> result,
                                        const gatt::CharacteristicMap&
                                            chars) mutable {
    if (!self.is_alive()) {
      return;
    }

    if (result.is_error()) {
      cb(result.take_error());
      return;
    }

    std::optional<gatt::CharacteristicHandle> conn_params_value_handle;
    for (auto& [handle, chr] : chars) {
      auto& data = chr.first;
      if (data.type == kPeripheralPreferredConnectionParametersCharacteristic) {
        conn_params_value_handle.emplace(data.value_handle);
        break;
      }
    }

    if (!conn_params_value_handle) {
      bt_log(DEBUG,
             "gap-le",
             "GAP service does not have peripheral preferred connection "
             "parameters characteristic "
             "(peer: %s)",
             bt_str(self->peer_id_));
      cb(ToResult(HostError::kNotFound).take_error());
      return;
    }

    if (!self->service_.is_alive()) {
      cb(ToResult(HostError::kFailed).take_error());
      return;
    }

    self->service_->ReadCharacteristic(
        *conn_params_value_handle,
        [self, connection_params_cb = std::move(cb)](
            att::Result<> discover_result,
            const ByteBuffer& buffer,
            bool /*maybe_truncated*/) mutable {
          if (!self.is_alive()) {
            return;
          }

          if (bt_is_error(discover_result,
                          DEBUG,
                          "gap-le",
                          "error reading peripheral preferred connection "
                          "parameters characteristic "
                          "(peer: %s)",
                          bt_str(self->peer_id_))) {
            connection_params_cb(discover_result.take_error());
            return;
          }

          if (buffer.size() !=
              sizeof(
                  PeripheralPreferredConnectionParametersCharacteristicValue)) {
            bt_log(DEBUG,
                   "gap-le",
                   "peripheral preferred connection parameters characteristic "
                   "has invalid value size "
                   "(peer: %s)",
                   bt_str(self->peer_id_));
            connection_params_cb(
                ToResult(HostError::kPacketMalformed).take_error());
            return;
          }

          auto char_value = buffer.template To<
              PeripheralPreferredConnectionParametersCharacteristicValue>();
          hci_spec::LEPreferredConnectionParameters params(
              pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          char_value.min_interval),
              pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          char_value.max_interval),
              pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          char_value.max_latency),
              pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                          char_value.supervision_timeout));

          connection_params_cb(fit::ok(params));
        });
  });
}

}  // namespace bt::gap::internal
