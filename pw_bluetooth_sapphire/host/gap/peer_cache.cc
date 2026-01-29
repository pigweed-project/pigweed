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

#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"

#include <lib/fit/function.h>
#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::gap {

namespace {

// Return an address with the same value as given, but with type kBREDR for
// kLEPublic addresses and vice versa.
DeviceAddress GetAliasAddress(const DeviceAddress& address) {
  if (address.type() == DeviceAddress::Type::kBREDR) {
    return {DeviceAddress::Type::kLEPublic, address.value()};
  } else if (address.type() == DeviceAddress::Type::kLEPublic) {
    return {DeviceAddress::Type::kBREDR, address.value()};
  }
  return address;
}

}  // namespace

Peer* PeerCache::NewPeer(const DeviceAddress& address, bool connectable) {
  auto* const peer = InsertPeerRecord(RandomPeerId(), address, connectable);
  if (peer) {
    UpdateExpiry(*peer);
    NotifyPeerUpdated(*peer, Peer::NotifyListenersChange::kBondNotUpdated);
  }
  return peer;
}

void PeerCache::ForEach(PeerCallback f) {
  PW_DCHECK(f);
  for (const auto& iter : peers_) {
    f(*iter.second.peer());
  }
}

bool PeerCache::AddBondedPeer(BondingData bd) {
  PW_DCHECK(bd.address.type() != DeviceAddress::Type::kLEAnonymous);

  const bool bond_le = bd.le_pairing_data.peer_ltk ||
                       bd.le_pairing_data.local_ltk || bd.le_pairing_data.csrk;
  const bool bond_bredr = bd.bredr_link_key.has_value();

  // |bd.le_pairing_data| must contain either a LTK or CSRK for LE Security Mode
  // 1 or 2.
  //
  // TODO(fxbug.dev/42102158): the address type checks here don't add much value
  // because the address type is derived from the presence of FIDL bredr_bond
  // and le_bond fields, so the check really should be whether at least one of
  // the mandatory bond secrets is present.
  if (bd.address.IsLowEnergy() && !bond_le) {
    bt_log(ERROR,
           "gap-le",
           "mandatory keys missing: no LTK or CSRK (id: %s)",
           bt_str(bd.identifier));
    return false;
  }

  if (bd.address.IsBrEdr() && !bond_bredr) {
    bt_log(ERROR,
           "gap-bredr",
           "mandatory link key missing (id: %s)",
           bt_str(bd.identifier));
    return false;
  }

  auto* peer =
      InsertPeerRecord(bd.identifier, bd.address, /*connectable=*/true);
  if (!peer) {
    return false;
  }

  if (bd.name.has_value()) {
    peer->RegisterName(bd.name.value(), Peer::NameSource::kUnknown);
  }

  if (bd.device_class.has_value()) {
    peer->MutBrEdr().SetDeviceClass(*bd.device_class);
  }

  if (bond_le) {
    PW_CHECK(bd.le_pairing_data.irk.has_value() ==
             bd.le_pairing_data.identity_address.has_value());
    peer->MutLe().SetBondData(bd.le_pairing_data);
    PW_CHECK(peer->le()->bonded());

    // Add the peer to the resolving list if it has an IRK.
    if (bd.le_pairing_data.irk) {
      le_resolving_list_.Add(bd.le_pairing_data.identity_address.value(),
                             bd.le_pairing_data.irk.value().value());
    }
  }

  if (bond_bredr) {
    for (auto& service : bd.bredr_services) {
      peer->MutBrEdr().AddService(std::move(service));
    }

    PW_CHECK(peer->MutBrEdr().SetBondData(*bd.bredr_link_key));
    PW_DCHECK(peer->bredr()->bonded());
  }

  if (peer->technology() == TechnologyType::kDualMode) {
    address_map_[GetAliasAddress(bd.address)] = bd.identifier;
  }

  PW_DCHECK(!peer->temporary());
  PW_DCHECK(peer->bonded());
  bt_log(TRACE,
         "gap",
         "restored bonded peer: %s, id: %s",
         bt_str(bd.address),
         bt_str(bd.identifier));

  // Don't call UpdateExpiry(). Since a bonded peer starts out as
  // non-temporary it is not necessary to ever set up the expiration callback.
  NotifyPeerUpdated(*peer, Peer::NotifyListenersChange::kBondNotUpdated);
  return true;
}

bool PeerCache::StoreLowEnergyBond(PeerId identifier,
                                   const sm::PairingData& bond_data) {
  PW_CHECK(bond_data.irk.has_value() == bond_data.identity_address.has_value());

  auto log_bond_failure =
      fit::defer([this] { peer_metrics_.LogLeBondFailureEvent(); });

  auto* peer = FindById(identifier);
  if (!peer) {
    bt_log(WARN,
           "gap-le",
           "failed to store bond for unknown peer (peer: %s)",
           bt_str(identifier));
    return false;
  }

  // Either a LTK or CSRK is mandatory for bonding (the former is needed for LE
  // Security Mode 1 and the latter is needed for Mode 2).
  if (!bond_data.peer_ltk && !bond_data.local_ltk && !bond_data.csrk) {
    bt_log(WARN,
           "gap-le",
           "mandatory keys missing: no LTK or CSRK (peer: %s)",
           bt_str(identifier));
    return false;
  }

  if (bond_data.identity_address) {
    auto existing_id = FindIdByAddress(*bond_data.identity_address);
    if (!existing_id) {
      // Map the new address to |peer|. We leave old addresses that map to
      // this peer in the cache in case there are any pending controller
      // procedures that expect them.
      // TODO(armansito): Maybe expire the old address after a while?
      address_map_[*bond_data.identity_address] = identifier;
    } else if (*existing_id != identifier) {
      bt_log(WARN,
             "gap-le",
             "identity address %s for peer %s belongs to another peer %s!",
             bt_str(*bond_data.identity_address),
             bt_str(identifier),
             bt_str(*existing_id));
      return false;
    }
    // We have either created a new mapping or the identity address already
    // maps to this peer.
  }

  // TODO(fxbug.dev/42072204): Check that we're not downgrading the security
  // level before overwriting the bond.
  peer->MutLe().SetBondData(bond_data);
  PW_DCHECK(!peer->temporary());
  PW_DCHECK(peer->le()->bonded());

  // Add the peer to the resolving list if it has an IRK.
  if (peer->identity_known() && bond_data.irk) {
    le_resolving_list_.Add(*bond_data.identity_address, bond_data.irk->value());
  }

  log_bond_failure.cancel();
  peer_metrics_.LogLeBondSuccessEvent();

  if (bond_data.cross_transport_key.has_value()) {
    if (peer->identity_known()) {
      if (!peer->MutBrEdr().SetBondData(
              bond_data.cross_transport_key.value())) {
        bt_log(WARN,
               "gap",
               "failed to store BR/EDR cross transport key"
               "(peer: %s)",
               bt_str(peer->identifier()));
      }
    } else {
      bt_log(WARN,
             "gap",
             "cannot use BR/EDR cross transport key without identity address "
             "(peer: %s)",
             bt_str(peer->identifier()));
    }
  }

  // Report the bond for persisting only if the identity of the peer is known.
  if (peer->identity_known()) {
    NotifyPeerBonded(*peer);
  }

  return true;
}

bool PeerCache::StoreBrEdrBond(const DeviceAddress& address,
                               const sm::LTK& link_key) {
  PW_DCHECK(address.IsPublic());
  auto* peer = FindByAddress(address);
  if (!peer) {
    bt_log(WARN,
           "gap-bredr",
           "failed to store bond for unknown peer (address: %s)",
           bt_str(address));
    return false;
  }

  if (!peer->MutBrEdr().SetBondData(link_key)) {
    return false;
  }
  PW_DCHECK(!peer->temporary());
  PW_DCHECK(peer->bredr()->bonded());

  NotifyPeerBonded(*peer);
  return true;
}

bool PeerCache::SetAutoConnectBehaviorForIntentionalDisconnect(PeerId peer_id) {
  Peer* const peer = FindById(peer_id);
  if (!peer) {
    bt_log(WARN,
           "gap-le",
           "failed to update auto-connect behavior to kSkipUntilNextConnection "
           "for "
           "unknown peer: %s",
           bt_str(peer_id));
    return false;
  }

  bt_log(DEBUG,
         "gap-le",
         "updated auto-connect behavior to kSkipUntilNextConnection (peer: %s)",
         bt_str(peer_id));

  peer->MutLe().set_auto_connect_behavior(
      Peer::AutoConnectBehavior::kSkipUntilNextConnection);

  // TODO(fxbug.dev/42113239): When implementing auto-connect behavior tracking
  // for classic bluetooth, consider tracking this policy for the peer as a
  // whole unless we think this policy should be applied separately for each
  // transport (per armansito@).

  return true;
}

bool PeerCache::SetAutoConnectBehaviorForSuccessfulConnection(PeerId peer_id) {
  Peer* const peer = FindById(peer_id);
  if (!peer) {
    bt_log(WARN,
           "gap-le",
           "failed to update auto-connect behavior to kAlways for unknown "
           "peer: %s",
           bt_str(peer_id));
    return false;
  }

  bt_log(DEBUG,
         "gap-le",
         "updated auto-connect behavior to kAlways (peer: %s)",
         bt_str(peer_id));

  peer->MutLe().set_auto_connect_behavior(Peer::AutoConnectBehavior::kAlways);

  // TODO(fxbug.dev/42113239): Implement auto-connect behavior tracking for
  // classic bluetooth.

  return true;
}

bool PeerCache::RemoveDisconnectedPeer(PeerId peer_id) {
  Peer* const peer = FindById(peer_id);
  if (!peer) {
    return true;
  }

  if (peer->connected()) {
    return false;
  }

  RemovePeer(peer);
  return true;
}

Peer* PeerCache::FindById(PeerId peer_id) const {
  auto iter = peers_.find(peer_id);
  return iter != peers_.end() ? iter->second.peer() : nullptr;
}

Peer* PeerCache::FindByAddress(const DeviceAddress& in_address) const {
  std::optional<DeviceAddress> address;
  if (in_address.IsResolvablePrivate()) {
    address = le_resolving_list_.Resolve(in_address);
  }

  // Fall back to the input if an identity wasn't resolved.
  if (!address) {
    address = in_address;
  }

  PW_DCHECK(address);
  auto identifier = FindIdByAddress(*address);
  if (!identifier) {
    return nullptr;
  }

  auto* p = FindById(*identifier);
  PW_DCHECK(p);
  return p;
}

void PeerCache::AttachInspect(inspect::Node& parent, std::string name) {
  node_ = parent.CreateChild(name);

  if (!node_) {
    return;
  }

  peer_metrics_.AttachInspect(node_);

  for (auto& [_, record] : peers_) {
    record.peer()->AttachInspect(node_, node_.UniqueName("peer_"));
  }
}

PeerCache::CallbackId PeerCache::add_peer_updated_callback(
    PeerCallback callback) {
  auto [iter, success] =
      peer_updated_callbacks_.emplace(next_callback_id_++, std::move(callback));
  PW_CHECK(success);
  return iter->first;
}

bool PeerCache::remove_peer_updated_callback(CallbackId id) {
  return peer_updated_callbacks_.erase(id);
}

// Private methods below.

Peer* PeerCache::InsertPeerRecord(PeerId identifier,
                                  const DeviceAddress& address,
                                  bool connectable) {
  if (FindIdByAddress(address)) {
    bt_log(WARN,
           "gap",
           "tried to insert peer with existing address: %s",
           address.ToString().c_str());
    return nullptr;
  }

  auto store_le_bond_cb = [this, identifier](const sm::PairingData& data) {
    return StoreLowEnergyBond(identifier, data);
  };

  std::unique_ptr<Peer> peer(
      new Peer(fit::bind_member<&PeerCache::NotifyPeerUpdated>(this),
               fit::bind_member<&PeerCache::UpdateExpiry>(this),
               fit::bind_member<&PeerCache::MakeDualMode>(this),
               std::move(store_le_bond_cb),
               identifier,
               address,
               connectable,
               &peer_metrics_,
               dispatcher_));
  if (node_) {
    peer->AttachInspect(node_, node_.UniqueName("peer_"));
  }

  // Note: we must construct the PeerRecord in-place, because it doesn't
  // support copy or move.
  auto [iter, inserted] = peers_.try_emplace(
      peer->identifier(),
      std::move(peer),
      [this, p = peer.get()] { RemovePeer(p); },
      dispatcher_);
  if (!inserted) {
    bt_log(WARN,
           "gap",
           "tried to insert peer with existing ID: %s",
           bt_str(identifier));
    return nullptr;
  }

  address_map_[address] = identifier;
  return iter->second.peer();
}

void PeerCache::NotifyPeerBonded(const Peer& peer) {
  PW_DCHECK(peers_.find(peer.identifier()) != peers_.end());
  PW_DCHECK(peers_.at(peer.identifier()).peer() == &peer);
  PW_DCHECK(peer.identity_known(),
            "peers not allowed to bond with unknown identity!");

  bt_log(INFO, "gap", "successfully bonded (peer: %s)", bt_str(peer));
  if (peer_bonded_callback_) {
    peer_bonded_callback_(peer);
  }
}

void PeerCache::NotifyPeerUpdated(const Peer& peer,
                                  Peer::NotifyListenersChange change) {
  PW_DCHECK(peers_.find(peer.identifier()) != peers_.end());
  PW_DCHECK(peers_.at(peer.identifier()).peer() == &peer);

  for (auto& [_, peer_updated_callback] : peer_updated_callbacks_) {
    peer_updated_callback(peer);
  }

  if (change == Peer::NotifyListenersChange::kBondUpdated) {
    PW_CHECK(peer.bonded());
    bt_log(INFO, "gap", "peer bond updated %s", bt_str(peer));
    if (peer_bonded_callback_) {
      peer_bonded_callback_(peer);
    }
  }
}

void PeerCache::UpdateExpiry(const Peer& peer) {
  auto peer_record_iter = peers_.find(peer.identifier());
  PW_DCHECK(peer_record_iter != peers_.end());

  auto& peer_record = peer_record_iter->second;
  PW_DCHECK(peer_record.peer() == &peer);

  peer_record.removal_task()->Cancel();

  // Previous expiry task has been canceled. Re-schedule only if the peer is
  // temporary.
  if (peer.temporary()) {
    peer_record.removal_task()->PostAfter(kCacheTimeout);
  }
}

void PeerCache::MakeDualMode(const Peer& peer) {
  PW_CHECK(address_map_.at(peer.address()) == peer.identifier());
  const auto address_alias = GetAliasAddress(peer.address());
  auto [iter, inserted] =
      address_map_.try_emplace(address_alias, peer.identifier());
  PW_CHECK(inserted || iter->second == peer.identifier(),
           "%s can't become dual-mode because %s maps to %s",
           bt_str(peer.identifier()),
           bt_str(address_alias),
           bt_str(iter->second));
  bt_log(INFO,
         "gap",
         "peer became dual mode (peer: %s, address: %s, alias: %s)",
         bt_str(peer.identifier()),
         bt_str(peer.address()),
         bt_str(address_alias));

  // The peer became dual mode in lieu of adding a new peer but is as
  // significant, so notify listeners of the change.
  NotifyPeerUpdated(peer, Peer::NotifyListenersChange::kBondNotUpdated);
}

void PeerCache::RemovePeer(Peer* peer) {
  PW_DCHECK(peer);

  auto peer_record_it = peers_.find(peer->identifier());
  PW_DCHECK(peer_record_it != peers_.end());
  PW_DCHECK(peer_record_it->second.peer() == peer);

  PeerId id = peer->identifier();
  bt_log(DEBUG, "gap", "removing peer %s", bt_str(id));
  for (auto iter = address_map_.begin(); iter != address_map_.end();) {
    if (iter->second == id) {
      iter = address_map_.erase(iter);
    } else {
      iter++;
    }
  }

  if (peer->le() && peer->le()->bonded()) {
    if (auto& address = peer->le()->bond_data()->identity_address) {
      le_resolving_list_.Remove(*address);
    }
  }

  peers_.erase(peer_record_it);  // Destroys |peer|.
  if (peer_removed_callback_) {
    peer_removed_callback_(id);
  }
}

std::optional<PeerId> PeerCache::FindIdByAddress(
    const DeviceAddress& address) const {
  auto iter = address_map_.find(address);
  if (iter == address_map_.end()) {
    // Search again using the other technology's address. This is necessary when
    // a dual-mode peer is known by only one technology and is then discovered
    // or connected on its other technology.
    iter = address_map_.find(GetAliasAddress(address));
  }

  if (iter == address_map_.end()) {
    return {};
  }
  return {iter->second};
}

}  // namespace bt::gap
