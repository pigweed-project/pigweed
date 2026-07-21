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

#include "pw_bluetooth_sapphire/internal/host/common/metrics.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/persisted_data.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/server.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/types.h"

namespace bt {

namespace l2cap {
class Channel;
}  // namespace l2cap

namespace gatt {

// This is the root object of the GATT layer. This object owns:
//
//   * A single local attribute database
//   * All client and server data bearers
//   * L2CAP ATT fixed channels
class GATT : public WeakSelf<GATT> {
 public:
  using RemoteServiceWatcherId = uint64_t;
  using PeerMtuListenerId = uint64_t;

  // Constructs a production GATT object.
  static std::unique_ptr<GATT> Create();

  GATT();
  virtual ~GATT() = default;

  // Registers the given connection with the GATT profile without initiating
  // service discovery. Once a connection is registered with GATT, the peer can
  // access local services and clients can call the "Remote Service" methods
  // below using |peer_id|.
  //
  // |peer_id|: The identifier for the peer device that the link belongs to.
  //            This is used to identify the peer while handling certain events.
  // |client|: The GATT client specific to this connection. This can be a
  // production |Client| or a
  //           |FakeClient| for testing.
  // |server_factory|: Factory method for a GATT server that operates on this
  // connection. This can
  //                   be a production |Server| or a |MockServer| for testing.
  //                   Note: the server handles GATT server procedures, but
  //                   importantly does *not* store any GATT server state
  //                   itself.
  virtual void AddConnection(PeerId peer_id,
                             std::unique_ptr<Client> client,
                             Server::FactoryFunction server_factory) = 0;

  // Unregisters the GATT profile connection to the peer with Id |peer_id|.
  virtual void RemoveConnection(PeerId peer_id) = 0;

  // |PeerMtuListener| will be notified when any MTU negotiation completes
  // without an unrecoverable error. The PeerId is the peer using that MTU, and
  // the uint16_t is the MTU.
  using PeerMtuListener = fit::function<void(PeerId, uint16_t)>;
  virtual PeerMtuListenerId RegisterPeerMtuListener(
      PeerMtuListener listener) = 0;

  // Unregisters the PeerMtuListener associated with |listener_id|. Returns true
  // if a listener was successfully unregistered, or false if |listener_id| was
  // not associated with an active listener.
  virtual bool UnregisterPeerMtuListener(PeerMtuListenerId listener_id) = 0;

  // ==============
  // Local Services
  // ==============
  //
  // The methods below are for managing local GATT services that are available
  // to data bearers in the server role.

  // Registers the GATT service hierarchy represented by |service| with the
  // local attribute database. Once successfully registered, the service will
  // be available to remote clients.
  //
  // Objects under |service| must have unique identifiers to aid in value
  // request handling. These identifiers will be passed to |read_handler| and
  // |write_handler|.
  //
  // The provided handlers will be called to handle remote initiated
  // transactions targeting the service. |disconnect_callback| will be called
  // when a remote client that has subscribed to notifications or indications
  // disconnects from the link.
  //
  // This method returns an opaque identifier on successful registration,
  // which can be used by the caller to refer to the service in the future. This
  // ID will be returned via |callback|.
  //
  // Returns |kInvalidId| on failure. Registration can fail if the attribute
  // database has run out of handles or if the hierarchy contains
  // characteristics or descriptors with repeated IDs.
  using ServiceIdCallback = fit::function<void(IdType)>;
  virtual void RegisterService(
      ServicePtr service,
      ServiceIdCallback callback,
      ReadHandler read_handler,
      WriteHandler write_handler,
      ClientConfigCallback ccc_callback,
      ClientDisconnectCallback disconnect_callback) = 0;

  void RegisterService(ServicePtr service,
                       ServiceIdCallback callback,
                       ReadHandler read_handler,
                       WriteHandler write_handler,
                       ClientConfigCallback ccc_callback) {
    RegisterService(std::move(service),
                    std::move(callback),
                    std::move(read_handler),
                    std::move(write_handler),
                    std::move(ccc_callback),
                    NopDisconnectCallback);
  }

  // Unregisters the GATT service hierarchy identified by |service_id|. Has no
  // effect if |service_id| is not a registered id.
  virtual void UnregisterService(IdType service_id) = 0;

  // Sends a characteristic handle-value notification|indication to a peer that
  // has configured the characteristic for notifications|indications. Does
  // nothing if the given peer has not configured the characteristic.
  //
  // |service_id|: The GATT service that the characteristic belongs to.
  // |chrc_id|: The GATT characteristic that will be notified.
  // |peer_id|: ID of the peer that the notification/indication will be sent to.
  // |value|: The attribute value that will be included in the notification.
  // |indicate_cb|: If nullptr, a notification will be sent. Otherwise, an
  //   indication will be attempted, and |indicate_cb| will be resolved when
  //   the indication is acknowledged by the peer or fails (e.g. if the peer is
  //   not connected, not configured for indications, or fails to confirm the
  //   indication within the ATT timeout of 30s (v5.3, Vol. 3, Part F 3.3.3)).
  //
  // TODO(fxbug.dev/42161294): Revise this API to involve fewer lookups.
  virtual void SendUpdate(IdType service_id,
                          IdType chrc_id,
                          PeerId peer_id,
                          ::std::vector<uint8_t> value,
                          IndicationCallback indicate_cb) = 0;

  // Like SendUpdate, but instead of updating a particular peer, sends a
  // notification|indication to all connected peers that have configured
  // notifications|indications. |indicate_cb|: If nullptr, notifications will be
  // sent. Otherwise, indications will be sent, and
  //   |indicate_cb| will be resolved after all of the indications are
  //   successfully confirmed, or when any of the
  //   connected+configured-for-indications peers fail to confirm the indication
  //   within the ATT timeout of 30s (v5.3, Vol. 3, Part F 3.3.3)).
  virtual void UpdateConnectedPeers(IdType service_id,
                                    IdType chrc_id,
                                    ::std::vector<uint8_t> value,
                                    IndicationCallback indicate_cb) = 0;

  // Sets a callback to run when certain local GATT database changes occur.
  // These changes are to those database attributes which need to be persisted
  // accross reconnects by bonded peers.  This is used by the GAP adapter to
  // store these changes in the peer cache.  This should only be called by the
  // GAP adapter.
  virtual void SetPersistServiceChangedCCCCallback(
      PersistServiceChangedCCCCallback callback) = 0;

  // Sets a callback to run when a peer connects.  This used to set those
  // database attributes which need to be persisted accross reconnects by bonded
  // peers by reading them from the peer cache. This should only be called by
  // the GAP adapter.
  virtual void SetRetrieveServiceChangedCCCCallback(
      RetrieveServiceChangedCCCCallback callback) = 0;

  // ===============
  // Remote Services
  // ===============
  //
  // The methods below are for interacting with remote GATT services. These
  // methods operate asynchronously.

  // Initialize remote services (e.g. exchange MTU, perform service discovery)
  // for the peer with the given |peer_id|. If |services_to_discover| is
  // non-empty, only discover services with the given UUIDs.
  virtual void InitializeClient(PeerId peer_id,
                                std::vector<UUID> services_to_discover) = 0;

  // Register a handler that will be notified when remote services are added,
  // modified, or removed on the peer |peer_id|. Returns an ID that can be used
  // to unregister the handler.
  virtual RemoteServiceWatcherId RegisterRemoteServiceWatcherForPeer(
      PeerId peer_id, RemoteServiceWatcher watcher) = 0;

  // Remove the remote service watcher with ID |watcher_id|. Returns true if the
  // handler existed and was successfully removed.
  virtual bool UnregisterRemoteServiceWatcher(
      RemoteServiceWatcherId watcher_id) = 0;

  // Returns the list of remote services that were found on the device with
  // |peer_id|. If |peer_id| was registered but InitializeClient() has not been
  // called yet, this request will be buffered until remote services have been
  // discovered. If the connection is removed without discovery services,
  // |callback| will be called with an error status.
  virtual void ListServices(PeerId peer_id,
                            std::vector<UUID> uuids,
                            ServiceListCallback callback) = 0;

  // Connects the RemoteService with the given identifier found on the device
  // with |peer_id|. A pointer to the service will be returned if it exists, or
  // nullptr will be returned otherwise.
  virtual RemoteService::WeakPtr FindService(PeerId peer_id,
                                             IdType service_id) = 0;

  // Attaches the GATT inspect properties to the parent node.
  virtual void AttachInspect(inspect::Node& parent, std::string name) = 0;

 private:
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(GATT);
};

}  // namespace gatt
}  // namespace bt
