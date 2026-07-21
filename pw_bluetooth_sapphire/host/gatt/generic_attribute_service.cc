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

#include "pw_bluetooth_sapphire/internal/host/gatt/generic_attribute_service.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"

namespace bt::gatt {

GenericAttributeService::GenericAttributeService(
    LocalServiceManager::WeakPtr local_service_manager,
    SendIndicationCallback send_indication_callback)
    : local_service_manager_(std::move(local_service_manager)),
      send_indication_callback_(std::move(send_indication_callback)) {
  PW_CHECK(local_service_manager_.is_alive());
  PW_DCHECK(send_indication_callback_);

  Register();
}

GenericAttributeService::~GenericAttributeService() {
  if (local_service_manager_.is_alive() && service_id_ != kInvalidId) {
    local_service_manager_->UnregisterService(service_id_);
  }
}

void GenericAttributeService::Register() {
  const att::AccessRequirements kDisallowed;
  const att::AccessRequirements kAllowedNoSecurity(/*encryption=*/false,
                                                   /*authentication=*/false,
                                                   /*authorization=*/false);
  CharacteristicPtr service_changed_chr = std::make_unique<Characteristic>(
      kServiceChangedChrcId,                 // id
      types::kServiceChangedCharacteristic,  // type
      Property::kIndicate,                   // properties
      0u,                                    // extended_properties
      kDisallowed,                           // read
      kDisallowed,                           // write
      kAllowedNoSecurity);                   // update
  auto service =
      std::make_unique<Service>(true, types::kGenericAttributeService);
  service->AddCharacteristic(std::move(service_changed_chr));

  ClientConfigCallback ccc_callback = [this](IdType,
                                             IdType chrc_id,
                                             PeerId peer_id,
                                             bool notify,
                                             bool indicate) {
    PW_DCHECK(chrc_id == 0u);

    SetServiceChangedIndicationSubscription(peer_id, indicate);
    if (persist_service_changed_ccc_callback_) {
      ServiceChangedCCCPersistedData persisted = {.notify = notify,
                                                  .indicate = indicate};
      persist_service_changed_ccc_callback_(peer_id, persisted);
    } else {
      bt_log(WARN,
             "gatt",
             "Attempted to persist service changed ccc but no callback found.");
    }
  };

  // Server Supported Features (Vol 3, Part G, Section 7.4)
  CharacteristicPtr server_features_chr = std::make_unique<Characteristic>(
      kServerSupportedFeaturesChrcId,                 // id
      types::kServerSupportedFeaturesCharacteristic,  // type
      Property::kRead,                                // properties
      0u,                                             // extended_properties
      kAllowedNoSecurity,                             // read
      kDisallowed,                                    // write
      kDisallowed);                                   // update
  service->AddCharacteristic(std::move(server_features_chr));

  ReadHandler read_handler =
      [](PeerId, IdType, IdType chrc_id, uint16_t, ReadResponder responder) {
        // The stack shouldn't send us any read requests other than this id,
        // none of the other characteristics or descriptors support it.
        PW_DCHECK(chrc_id == kServerSupportedFeaturesChrcId);

        // The only octet is the first octet.  The only bit is the EATT
        // supported bit.
        // TODO(fxbug.dev/364660604): Support EATT, then flip this bit to 1.
        responder(fit::ok(), StaticByteBuffer(0x00));
      };

  ClientDisconnectCallback disconnect_callback =
      [this](IdType, PeerId peer_id) { OnDisconnect(peer_id); };

  service_id_ =
      local_service_manager_->RegisterService(std::move(service),
                                              std::move(read_handler),
                                              NopWriteHandler,
                                              std::move(ccc_callback),
                                              std::move(disconnect_callback));
  PW_DCHECK(service_id_ != kInvalidId);

  local_service_manager_->set_service_changed_callback(
      fit::bind_member<&GenericAttributeService::OnServiceChanged>(this));
}

void GenericAttributeService::SetServiceChangedIndicationSubscription(
    PeerId peer_id, bool indicate) {
  if (indicate) {
    subscribed_peers_.insert(peer_id);
    bt_log(DEBUG,
           "gatt",
           "service: Service Changed enabled for peer %s",
           bt_str(peer_id));
  } else {
    subscribed_peers_.erase(peer_id);
    bt_log(DEBUG,
           "gatt",
           "service: Service Changed disabled for peer %s",
           bt_str(peer_id));
  }
}

void GenericAttributeService::OnDisconnect(PeerId peer_id) {
  subscribed_peers_.erase(peer_id);
}

void GenericAttributeService::OnServiceChanged(IdType service_id,
                                               att::Handle start,
                                               att::Handle end) {
  // Don't send indications for this service's removal.
  if (service_id_ == service_id) {
    return;
  }

  StaticByteBuffer<2 * sizeof(uint16_t)> value;

  value[0] = static_cast<uint8_t>(start);
  value[1] = static_cast<uint8_t>(start >> 8);
  value[2] = static_cast<uint8_t>(end);
  value[3] = static_cast<uint8_t>(end >> 8);

  for (auto peer_id : subscribed_peers_) {
    bt_log(TRACE,
           "gatt",
           "service: indicating peer %s of service(s) changed "
           "(start: %#.4x, end: %#.4x)",
           bt_str(peer_id),
           start,
           end);
    send_indication_callback_(
        service_id_, kServiceChangedChrcId, peer_id, value.view());
  }
}

}  // namespace bt::gatt
