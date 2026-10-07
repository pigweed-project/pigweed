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

#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/smart_task.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bonding_data.h"
#include "pw_bluetooth_sapphire/internal/host/gap/identity_resolving_list.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_metrics.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt {

class ByteBuffer;

namespace hci {
class LowEnergyScanResult;
}  // namespace hci

namespace gap {

// A PeerCache provides access to remote Bluetooth devices that are
// known to the system.
class PeerCache final {
 public:
  using CallbackId = uint64_t;
  using PeerCallback = fit::function<void(const Peer& peer)>;
  using PeerIdCallback = fit::function<void(PeerId identifier)>;

  PeerCache(pw::async::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}

  // Creates a new peer entry using the given parameters, and returns a
  // (non-owning) pointer to that peer. The caller must not retain the pointer
  // beyond the current dispatcher task, as the underlying Peer is owned
  // by |this| PeerCache, and may be invalidated spontaneously.
  //
  // Returns nullptr if an entry matching |address| already exists in the cache,
  // including as a public identity of a peer with a different technology.
  Peer* NewPeer(const DeviceAddress& address, bool connectable);

  // Iterates over all current peers in the map, running |f| on each entry
  // synchronously. This is intended for IPC methods that request a list of
  // peers. |f| must not call RemoveDisconnectedPeer, as peers cannot be removed
  // while iterating.
  //
  // Clients should use the FindBy*() methods below to interact with
  // Peer objects.
  void ForEach(PeerCallback f);

  // Creates a new non-temporary peer entry using the given |bd.identifier| and
  // identity |bd.address|. This is intended to initialize this PeerCache
  // with previously bonded peers while bootstrapping a bt-host peer. The
  // "peer bonded" callback will not be invoked.
  //
  // This method is not intended for updating the bonding data of a peer that
  // already exists the cache and returns false if a mapping for
  // |bd. identifier| or |bd.address| is already present. Use Store*Bond()
  // methods to update pairing information of an existing peer.
  //
  // If a peer already exists that has the same public identity address with a
  // different technology, this method will return false. The existing peer
  // should be instead updated with new bond information to create a dual-mode
  // peer.
  //
  bool AddBondedPeer(BondingData bd);

  // Update the peer with the given identifier with new LE bonding
  // information. The peer will be considered "bonded" and the bonded callback
  // will be notified. If the peer is already bonded then bonding data will be
  // updated.
  //
  // If |bond_data| contains an |identity_address|, the peer cache will be
  // updated with a new mapping from that address to this peer identifier. If
  // the identity address already maps to an existing peer, this method will
  // return false. TODO(armansito): Merge the peers instead of failing? What
  // happens if we obtain a LE identity address from a dual-mode peer that
  // matches the BD_ADDR previously obtained from it over BR/EDR?
  bool StoreLowEnergyBond(PeerId identifier, const sm::PairingData& bond_data);

  // Update a peer identified by BD_ADDR |address| with a new BR/EDR link key.
  // The peer will be considered "bonded" and the bonded callback notified. If
  // the peer is already bonded then the link key will be updated. Returns
  // false if the address does not match that of a known peer.
  bool StoreBrEdrBond(const DeviceAddress& address, const sm::LTK& link_key);

  // Update a peer's auto-connect behavior appropriately for an intentional
  // (eg. manual) disconnect. Returns false if the address does not match that
  // of a known peer.
  bool SetAutoConnectBehaviorForIntentionalDisconnect(PeerId peer_id);

  // Update a peer's auto-connect behavior appropriately after a successful
  // connection. Returns false if the address does not match that of a known
  // peer.
  bool SetAutoConnectBehaviorForSuccessfulConnection(PeerId peer_id);

  // If a peer identified by |peer_id| exists and is not connected on either
  // transport, remove it from the cache immediately. Must not be called
  // re-entrantly from a peer updated, peer bonded, or ForEach callback. Returns
  // true after no peer with |peer_id| exists in the cache, false otherwise.
  [[nodiscard]] bool RemoveDisconnectedPeer(PeerId peer_id);

  // Returns the remote peer with identifier |peer_id|. Returns nullptr if
  // |peer_id| is not recognized.
  Peer* FindById(PeerId peer_id) const;

  // Finds and returns a Peer with address |address| if it exists,
  // returns nullptr otherwise. Tries to resolve |address| if it is resolvable.
  // If |address| is of type kBREDR or kLEPublic, then this searches for peers
  // that have either type of address.
  Peer* FindByAddress(const DeviceAddress& address) const;

  // Attach peer cache inspect node as a child node of |parent|.
  static constexpr const char* kInspectNodeName = "peer_cache";
  void AttachInspect(inspect::Node& parent,
                     std::string name = kInspectNodeName);

  // Register a |callback| to be invoked whenever a peer is added or updated.
  // |callback| must not call RemoveDisconnectedPeer, as peers cannot be removed
  // while callbacks are being notified.
  CallbackId add_peer_updated_callback(PeerCallback callback);

  // Unregister the callback indicated by |id|. Returns true if the callback was
  // removed successfully, or false otherwise (e.g. the callback was previously
  // removed).
  bool remove_peer_updated_callback(CallbackId id);

  // When set, |callback| will be invoked whenever a peer is removed.
  void set_peer_removed_callback(PeerIdCallback callback) {
    peer_removed_callback_ = std::move(callback);
  }

  // When this callback is set, |callback| will be invoked whenever the bonding
  // data of a peer is updated and should be persisted. The caller must ensure
  // that |callback| outlives |this|. |callback| must not call
  // RemoveDisconnectedPeer.
  void set_peer_bonded_callback(PeerCallback callback) {
    peer_bonded_callback_ = std::move(callback);
  }

  // Returns the number of peers that are currently in the peer cache.
  size_t count() const { return peers_.size(); }

  // Used by connection managers to increment peer bonding metrics.
  void LogBrEdrBondingEvent(bool success) {
    if (success) {
      peer_metrics_.LogBrEdrBondSuccessEvent();
    } else {
      peer_metrics_.LogBrEdrBondFailureEvent();
    }
  }

 private:
  class PeerRecord final {
   public:
    PeerRecord(std::unique_ptr<Peer> peer,
               fit::closure remove_peer_callback,
               pw::async::Dispatcher& dispatcher)
        : peer_(std::move(peer)),
          remove_peer_callback_(std::move(remove_peer_callback)),
          removal_task_(dispatcher) {
      removal_task_.set_function(
          [this](pw::async::Context /*ctx*/, pw::Status status) {
            if (status.ok() && remove_peer_callback_) {
              remove_peer_callback_();
            }
          });
    }

    // The copy and move ctors cannot be implicitly defined, since
    // async::TaskClosure does not support those operations. Nor is any
    // meaningful explicit definition possible.
    PeerRecord(const PeerRecord&) = delete;
    PeerRecord(PeerRecord&&) = delete;

    Peer* peer() const { return peer_.get(); }

    // Returns a pointer to removal_task_, which can be used to (re-)schedule or
    // cancel |remove_peer_callback|.
    SmartTask* removal_task() { return &removal_task_; }

   private:
    std::unique_ptr<Peer> peer_;
    fit::closure remove_peer_callback_;
    SmartTask removal_task_;
  };

  // Create and track a record of a remote peer with a given |identifier|,
  // |address|, and connectability (|connectable|). Returns a pointer to the
  // inserted peer or nullptr if |identifier| or |address| already exists in
  // the cache.
  Peer* InsertPeerRecord(PeerId identifier,
                         const DeviceAddress& address,
                         bool connectable);

  // Notifies interested parties that |peer| has bonded
  // |peer| must already exist in the cache.
  void NotifyPeerBonded(const Peer& peer);

  // Notifies interested parties that |peer| has seen a significant change.
  // |peer| must already exist in the cache.
  void NotifyPeerUpdated(const Peer& peer, Peer::NotifyListenersChange change);

  // Updates the expiration time for |peer|, if a temporary. Cancels expiry,
  // if a non-temporary. Pre-conditions:
  // - |peer| must already exist in the cache
  // - can only be called from the thread that created |peer|
  void UpdateExpiry(const Peer& peer);

  // Updates the cache when an existing peer is found to be dual-mode. Also
  // notifies listeners of the "peer updated" callback.
  // |peer| must already exist in the cache.
  void MakeDualMode(const Peer& peer);

  // Removes |peer| from this cache, and notifies listeners of the removal.
  void RemovePeer(Peer* peer);

  // Search for an unique peer ID by its peer address |address|, by both
  // technologies if it is a public address. |address| should be already
  // resolved, if it is resolvable. If found, returns a valid peer ID;
  // otherwise returns std::nullopt.
  std::optional<PeerId> FindIdByAddress(const DeviceAddress& address) const;

  pw::async::Dispatcher& dispatcher_;

  // Mapping from unique peer IDs to PeerRecords.
  // Owns the corresponding Peers.
  std::unordered_map<PeerId, PeerRecord> peers_;

  // Mapping from peer addresses to unique peer identifiers for all known
  // peers. This is used to look-up and update existing cached data for a
  // particular scan result so as to avoid creating duplicate entries for the
  // same peer.
  //
  // Dual-mode peers shall have identity addresses of both technologies
  // mapped to the same ID, if the addresses have the same value.
  std::unordered_map<DeviceAddress, PeerId> address_map_;

  // The LE identity resolving list used to resolve RPAs.
  IdentityResolvingList le_resolving_list_;

  CallbackId next_callback_id_ = 0u;
  size_t notify_listeners_depth_ = 0u;
  std::unordered_map<CallbackId, PeerCallback> peer_updated_callbacks_;
  PeerIdCallback peer_removed_callback_;
  PeerCallback peer_bonded_callback_;

  inspect::Node node_;

  PeerMetrics peer_metrics_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PeerCache);
};

}  // namespace gap
}  // namespace bt
