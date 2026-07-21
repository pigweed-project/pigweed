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
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/att/database.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/types.h"

namespace bt::gatt {

// Called to read the value of a dynamic characteristic or characteristic
// descriptor.
//   - |peer_id|: The PeerId of the peer making the request.
//   - |service_id|: Identifies the service that the object belongs to.
//   - |id|: Identifies the object to be read. This is a user assigned
//           identifier provided while registering the service.
//   - |offset|: The offset into the value that is being read.
//   - |responder|: Should be called to respond to the read request with a
//                  characteristic or descriptor value, or an ATT error code.
using ReadHandler = fit::function<void(PeerId peer_id,
                                       IdType service_id,
                                       IdType id,
                                       uint16_t offset,
                                       ReadResponder responder)>;

// Called to write the value of a dynamic characteristic or characteristic
// descriptor.
//   - |peer_id|: The PeerId of the peer making the request.
//   - |service_id|: Identifies the service that the object belongs to.
//   - |id|: Identifies the object to be written. This is a user assigned
//           identifier provided while registering the service.
//   - |offset|: The offset into the value that is being written.
//   - |responder|: Should be called to respond to the write request with
//                  success or an ATT error code. This can be a null callback
//                  if the client has initiated a "Write Without Response"
//                  procedure, in which case a response is not required.
using WriteHandler = fit::function<void(PeerId peer_id,
                                        IdType service_id,
                                        IdType id,
                                        uint16_t offset,
                                        const ByteBuffer& value,
                                        WriteResponder responder)>;

// Called when the peer device with the given |peer_id| has enabled or disabled
// notifications/indications on the characteristic with id |chrc_id|.
using ClientConfigCallback = fit::function<void(IdType service_id,
                                                IdType chrc_id,
                                                PeerId peer_id,
                                                bool notify,
                                                bool indicate)>;

// Called when the peer device with the given |peer_id| disconnects so
// services can clean up volatile per-peer state.
using ClientDisconnectCallback =
    fit::function<void(IdType service_id, PeerId peer_id)>;

// Called with the ID and range of attributes handles spanned (inclusive) by a
// service that was added or removed.
using ServiceChangedCallback =
    fit::function<void(IdType service_id, att::Handle start, att::Handle end)>;

// LocalServiceManager allows clients to implement GATT services. This
// internally maintains an attribute database and provides hooks for clients to
// respond to read and write requests, send notifications/indications,
// add/remove services, etc.
class LocalServiceManager final : public WeakSelf<LocalServiceManager> {
 public:
  LocalServiceManager();
  // Even though this is a noop, cannot be defaulted due to forward declaration
  // of ServiceData.
  ~LocalServiceManager();

  // Registers the GATT service hierarchy represented by |service| with the
  // local attribute database. Once successfully registered, the service will be
  // available for discovery and other ATT protocol requests.
  //
  // |disconnect_callback| will be called when a remote client that has
  // subscribed to notifications or indications disconnects from the link.
  //
  // This method returns an opaque identifier on successful registration, which
  // can be used by the caller to refer to the service in the future.
  //
  // Returns |kInvalidId| on failure. Registration can fail if the attribute
  // database has run out of handles or if the hierarchy contains
  // characteristics or descriptors with repeated IDs. Objects under |service|
  // must have unique identifiers to aid in value request handling.
  IdType RegisterService(
      ServicePtr service,
      ReadHandler read_handler,
      WriteHandler write_handler,
      ClientConfigCallback ccc_callback,
      ClientDisconnectCallback disconnect_callback = NopDisconnectCallback);

  // Unregisters the GATT service hierarchy identified by |service_id|. Returns
  // false if |service_id| is unrecognized.
  bool UnregisterService(IdType service_id);

  // Returns the client characteristic configuration for the given |peer_id| and
  // the characteristic identified by |service_id| and |chrc_id|. Returns false
  // if |service_id| is unknown or no configurations exist for |chrc_id|.
  struct ClientCharacteristicConfig {
    att::Handle handle;
    bool notify;
    bool indicate;
  };
  bool GetCharacteristicConfig(IdType service_id,
                               IdType chrc_id,
                               PeerId peer_id,
                               ClientCharacteristicConfig* out_config);

  // Erase any client characteristic configuration associated to a specific
  // client and invoke its ClientConfigCallback to signal that notifications and
  // indications are now disabled.
  void DisconnectClient(PeerId peer_id);

  void set_service_changed_callback(ServiceChangedCallback callback) {
    service_changed_callback_ = std::move(callback);
  }

  inline att::Database::WeakPtr database() { return db_->GetWeakPtr(); }

 private:
  class ServiceData;

  std::unique_ptr<att::Database> db_;
  IdType next_service_id_;

  // Mapping from service instance ids to ServiceData.
  std::unordered_map<IdType, std::unique_ptr<ServiceData>> services_;

  ServiceChangedCallback service_changed_callback_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LocalServiceManager);
};

}  // namespace bt::gatt
