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
#include <lib/fit/function.h>

#include <unordered_set>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/persisted_data.h"

namespace bt::gatt {
inline constexpr IdType kServiceChangedChrcId = 0u;
inline constexpr IdType kServerSupportedFeaturesChrcId = 1u;

// Callback to send an indication. Used to inject the GATT object's
// update-sending ability without requiring this service to carry a reference to
// GATT or Server.
//   |chrc_id|: the service-defined ID of the characteristic to indicate
//   |svc_id|: the gatt::GATT-defined ID of the service containing |chrc_id|.
// For example, to indicate a new service to a peer via the Service Changed
// chrc, one would invoke this with svc_id = the GenericAttributeService's
// service_id_, chrc_id = kServiceChangedChrcId, peer_id of the peer, and value
// = the att::Handle range of the new service.
using SendIndicationCallback = fit::function<void(
    IdType svc_id, IdType chrc_id, PeerId peer_id, BufferView value)>;

// Implements the "Generic Attribute Profile Service" containing the "Service
// Changed" characteristic that is "...used to indicate to connected devices
// that services have changed (Vol 3, Part G, 7)."
class GenericAttributeService final {
 public:
  // Registers this service and makes this service the callee of the Service
  // Changed callback. GATT remote clients must still request that they be sent
  // indications for the Service Changed characteristic. Holds the
  // LocalServiceManager pointer for this object's lifetime. Do not register
  // multiple instances of this service in a single bt-host.
  GenericAttributeService(LocalServiceManager::WeakPtr local_service_manager,
                          SendIndicationCallback send_indication_callback);
  ~GenericAttributeService();

  // This callback is called when a client changes the CCC for the service
  // changed characteristic to inform the upper layers of the stack to persist
  // this value.
  void SetPersistServiceChangedCCCCallback(
      PersistServiceChangedCCCCallback callback) {
    persist_service_changed_ccc_callback_ = std::move(callback);
  }

  // Set the service changed indication subscription for a given peer.
  void SetServiceChangedIndicationSubscription(PeerId peer_id, bool indicate);

  inline IdType service_id() const { return service_id_; }

 private:
  void Register();

  // Send indications to subscribed clients when a service has changed.
  void OnServiceChanged(IdType service_id, att::Handle start, att::Handle end);
  void OnDisconnect(PeerId peer_id);

  // Data store against which to register and unregister this service. It must
  // outlive this instance.
  LocalServiceManager::WeakPtr local_service_manager_;
  const SendIndicationCallback send_indication_callback_;

  // Peers that have subscribed to indications.
  std::unordered_set<PeerId> subscribed_peers_;

  // Local service ID; hidden because registration is tied to instance lifetime.
  IdType service_id_ = kInvalidId;

  // Callback to inform uper stack layers to persist service changed CCC.
  PersistServiceChangedCCCCallback persist_service_changed_ccc_callback_;
};

}  // namespace bt::gatt
