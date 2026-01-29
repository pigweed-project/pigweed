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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/host_server.h"

#include <cpp-string/string_printf.h>
#include <lib/fpromise/result.h>
#include <pw_assert/check.h>
#include <zircon/errors.h>

#include <utility>
#include <variant>

#include "fuchsia/bluetooth/host/cpp/fidl.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt2_server_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt_server_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_central_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_peripheral_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/profile_server.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bonding_data.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bthost {

namespace fbt = fuchsia::bluetooth;
namespace fsys = fuchsia::bluetooth::sys;
namespace fhost = fuchsia::bluetooth::host;

using bt::PeerId;
using bt::gap::BrEdrSecurityModeToString;
using bt::gap::LeSecurityModeToString;
using bt::sm::IOCapability;
using fidl_helpers::BrEdrSecurityModeFromFidl;
using fidl_helpers::HostErrorToFidl;
using fidl_helpers::LeSecurityModeFromFidl;
using fidl_helpers::NewFidlError;
using fidl_helpers::PeerIdFromString;
using fidl_helpers::ResultToFidl;
using fidl_helpers::SecurityLevelFromFidl;

HostServer::HostServer(
    zx::channel channel,
    const bt::gap::Adapter::WeakPtr& adapter,
    bt::gatt::GATT::WeakPtr gatt,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    uint8_t sco_offload_index,
    async_dispatcher_t* dispatcher)
    : AdapterServerBase(adapter, this, std::move(channel)),
      pairing_delegate_(nullptr),
      gatt_(std::move(gatt)),
      wake_lease_provider_(wake_lease_provider),
      requesting_background_scan_(false),
      requesting_discoverable_(false),
      io_capability_(IOCapability::kNoInputNoOutput),
      sco_offload_index_(sco_offload_index),
      dispatcher_(dispatcher),
      weak_self_(this),
      weak_pairing_(this) {
  PW_CHECK(gatt_.is_alive());

  auto self = weak_self_.GetWeakPtr();
  adapter->peer_cache()->set_peer_bonded_callback([self](const auto& peer) {
    if (self.is_alive()) {
      self->OnPeerBonded(peer);
    }
  });
  adapter->set_auto_connect_callback([self](auto conn_ref) {
    if (self.is_alive()) {
      self->RegisterLowEnergyConnection(std::move(conn_ref),
                                        /*auto_connect=*/true);
    }
  });

  // Watch for changes in LE address.
  adapter->le()->register_address_changed_callback([self]() {
    if (self.is_alive()) {
      self->NotifyInfoChange();
    }
  });

  // Initialize the HostInfo getter with the initial state.
  NotifyInfoChange();
}

HostServer::~HostServer() { Shutdown(); }

void HostServer::RequestProtocol(fhost::ProtocolRequest request) {
  switch (request.Which()) {
    case fhost::ProtocolRequest::Tag::kCentral:
      BindServer<LowEnergyCentralServer>(adapter()->AsWeakPtr(),
                                         std::move(request.central()),
                                         gatt_,
                                         wake_lease_provider_,
                                         dispatcher_);
      break;
    case fhost::ProtocolRequest::Tag::kPeripheral:
      BindServer<LowEnergyPeripheralServer>(adapter()->AsWeakPtr(),
                                            gatt_,
                                            wake_lease_provider_,
                                            std::move(request.peripheral()));
      break;
    case fhost::ProtocolRequest::Tag::kGattServer:
      BindServer<GattServerServer>(gatt_->GetWeakPtr(),
                                   std::move(request.gatt_server()));
      break;
    case fhost::ProtocolRequest::Tag::kGatt2Server:
      BindServer<Gatt2ServerServer>(gatt_->GetWeakPtr(),
                                    std::move(request.gatt2_server()));
      break;
    case fhost::ProtocolRequest::Tag::kProfile:
      BindServer<ProfileServer>(adapter()->AsWeakPtr(),
                                wake_lease_provider_,
                                sco_offload_index_,
                                std::move(request.profile()));
      break;
    case fhost::ProtocolRequest::Tag::kPrivilegedPeripheral:
      BindServer<LowEnergyPrivilegedPeripheralServer>(
          adapter()->AsWeakPtr(),
          gatt_,
          wake_lease_provider_,
          std::move(request.privileged_peripheral()));
      break;
    default:
      bt_log(WARN, "fidl", "received unknown protocol request");
      // The unknown protocol will be closed when `request` is destroyed.
      break;
  }
}

void HostServer::WatchState(WatchStateCallback callback) {
  info_getter_.Watch([cb = std::move(callback)](fsys::HostInfo info) {
    cb(fhost::Host_WatchState_Result::WithResponse(
        fhost::Host_WatchState_Response(std::move(info))));
  });
}

void HostServer::SetLocalData(fsys::HostData host_data) {
  if (host_data.has_irk()) {
    bt_log(DEBUG, "fidl", "assign IRK");
    if (adapter()->le()) {
      adapter()->le()->set_irk(host_data.irk().value);
    }
  }
}

void HostServer::SetPeerWatcher(
    ::fidl::InterfaceRequest<::fuchsia::bluetooth::host::PeerWatcher>
        peer_watcher) {
  if (peer_watcher_server_.has_value()) {
    peer_watcher.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }
  peer_watcher_server_.emplace(
      std::move(peer_watcher), adapter()->peer_cache(), this);
}

void HostServer::SetLocalName(::std::string local_name,
                              SetLocalNameCallback callback) {
  PW_DCHECK(!local_name.empty());
  adapter()->SetLocalName(std::move(local_name),
                          [self = weak_self_.GetWeakPtr(),
                           callback = std::move(callback)](auto status) {
                            // Send adapter state update on success and if the
                            // connection is still open.
                            if (status.is_ok() && self.is_alive()) {
                              self->NotifyInfoChange();
                            }
                            callback(ResultToFidl(status));
                          });
}

// TODO(fxbug.dev/42110379): Add a unit test for this method.
void HostServer::SetDeviceClass(fbt::DeviceClass device_class,
                                SetDeviceClassCallback callback) {
  // Device Class values must only contain data in the lower 3 bytes.
  if (device_class.value >= 1 << 24) {
    callback(fpromise::error(fsys::Error::INVALID_ARGUMENTS));
    return;
  }
  bt::DeviceClass dev_class(device_class.value);
  adapter()->SetDeviceClass(dev_class,
                            [callback = std::move(callback)](auto status) {
                              callback(ResultToFidl(status));
                            });
}

void HostServer::StartLEDiscovery() {
  if (!adapter()->le()) {
    StopDiscovery(ZX_ERR_INTERNAL);
    return;
  }

  // Set up a general-discovery filter for connectable devices.
  // NOTE(armansito): This currently has no effect since peer updates
  // are driven by PeerCache events. |session|'s "result callback" is
  // unused.
  bt::hci::DiscoveryFilter filter;
  filter.set_connectable(true);
  filter.SetGeneralDiscoveryFlags();

  adapter()->le()->StartDiscovery(
      /*active=*/true,
      {filter},
      [self = weak_self_.GetWeakPtr()](auto session) {
        // End the new session if this AdapterServer got destroyed in the
        // meantime (e.g. because the client disconnected).
        if (!self.is_alive() || self->discovery_session_servers_.empty()) {
          return;
        }

        if (!session) {
          bt_log(ERROR, "fidl", "failed to start active LE discovery session");
          self->StopDiscovery(ZX_ERR_INTERNAL);
          return;
        }

        self->le_discovery_session_ = std::move(session);

        // Send the adapter state update.
        self->NotifyInfoChange();
      });
}

void HostServer::StartDiscovery(
    ::fuchsia::bluetooth::host::HostStartDiscoveryRequest request) {
  bt_log(DEBUG, "fidl", "%s", __FUNCTION__);
  PW_DCHECK(adapter().is_alive());

  if (!request.has_token()) {
    bt_log(WARN, "fidl", "missing Discovery token");
    return;
  }
  fidl::InterfaceRequest<fhost::DiscoverySession> token =
      std::move(*request.mutable_token());

  std::unique_ptr<DiscoverySessionServer> server =
      std::make_unique<DiscoverySessionServer>(std::move(token), this);
  DiscoverySessionServer* server_ptr = server.get();
  discovery_session_servers_.emplace(server_ptr, std::move(server));

  // If there were existing sessions, then discovery is already
  // starting/started.
  if (discovery_session_servers_.size() != 1) {
    return;
  }

  if (!adapter()->bredr()) {
    StartLEDiscovery();
    return;
  }
  // TODO(jamuraa): start these in parallel instead of sequence
  adapter()->bredr()->RequestDiscovery([self = weak_self_.GetWeakPtr(),
                                        func = __FUNCTION__](
                                           bt::hci::Result<> result,
                                           auto session) mutable {
    if (!self.is_alive() || self->discovery_session_servers_.empty()) {
      return;
    }

    if (result.is_error() || !session) {
      bt_log(
          ERROR, "fidl", "%s: failed to start BR/EDR discovery session", func);
      self->StopDiscovery(ZX_ERR_INTERNAL);
      return;
    }

    self->bredr_discovery_session_ = std::move(session);
    self->StartLEDiscovery();
  });
}

void HostServer::StopDiscovery(zx_status_t epitaph, bool notify_info_change) {
  bool discovering = le_discovery_session_ || bredr_discovery_session_;
  bredr_discovery_session_ = nullptr;
  le_discovery_session_ = nullptr;
  for (auto& [server, _] : discovery_session_servers_) {
    server->Close(epitaph);
  }
  discovery_session_servers_.clear();

  if (discovering && notify_info_change) {
    NotifyInfoChange();
  }
}

void HostServer::OnDiscoverySessionServerClose(DiscoverySessionServer* server) {
  server->Close(ZX_ERR_CANCELED);
  discovery_session_servers_.erase(server);
  if (discovery_session_servers_.empty()) {
    StopDiscovery(ZX_ERR_CANCELED);
  }
}

void HostServer::SetConnectable(bool connectable,
                                SetConnectableCallback callback) {
  bt_log(INFO, "fidl", "%s: %s", __FUNCTION__, connectable ? "true" : "false");

  auto classic = adapter()->bredr();
  if (!classic) {
    callback(fpromise::error(fsys::Error::NOT_SUPPORTED));
    return;
  }
  classic->SetConnectable(connectable,
                          [callback = std::move(callback)](const auto& result) {
                            callback(ResultToFidl(result));
                          });
}

void HostServer::RestoreBonds(
    ::std::vector<fsys::BondingData> bonds,
    fhost::BondingDelegate::RestoreBondsCallback callback) {
  bt_log(INFO, "fidl", "%s", __FUNCTION__);

  if (bonds.empty()) {
    // Nothing to do. Reply with an empty list.
    callback(fhost::BondingDelegate_RestoreBonds_Result::WithResponse(
        fhost::BondingDelegate_RestoreBonds_Response()));
    return;
  }

  std::vector<fsys::BondingData> errors;
  for (auto& bond : bonds) {
    if (!bond.has_identifier() || !bond.has_address() ||
        !(bond.has_le_bond() || bond.has_bredr_bond())) {
      bt_log(ERROR,
             "fidl",
             "%s: BondingData mandatory fields missing!",
             __FUNCTION__);
      errors.push_back(std::move(bond));
      continue;
    }

    auto address = fidl_helpers::AddressFromFidlBondingData(bond);
    if (!address) {
      bt_log(ERROR,
             "fidl",
             "%s: BondingData address missing or invalid!",
             __FUNCTION__);
      errors.push_back(std::move(bond));
      continue;
    }

    bt::gap::BondingData bd;
    bd.identifier = bt::PeerId{bond.identifier().value};
    bd.address = *address;
    if (bond.has_name()) {
      bd.name = {bond.name()};
    }
    if (bond.has_device_class()) {
      bd.device_class = bt::DeviceClass(bond.device_class().value);
    }

    if (bond.has_le_bond()) {
      bd.le_pairing_data =
          fidl_helpers::LePairingDataFromFidl(*address, bond.le_bond());
    }
    if (bond.has_bredr_bond()) {
      bd.bredr_link_key = fidl_helpers::BredrKeyFromFidl(bond.bredr_bond());
      bd.bredr_services =
          fidl_helpers::BredrServicesFromFidl(bond.bredr_bond());
    }

    // TODO(fxbug.dev/42137736): Convert bond.bredr.services to
    // BondingData::bredr_services
    if (!adapter()->AddBondedPeer(bd)) {
      bt_log(ERROR,
             "fidl",
             "%s: failed to restore bonding data entry",
             __FUNCTION__);
      errors.push_back(std::move(bond));
    }
  }

  callback(fhost::BondingDelegate_RestoreBonds_Result::WithResponse(
      fhost::BondingDelegate_RestoreBonds_Response(std::move(errors))));
}

void HostServer::OnPeerBonded(const bt::gap::Peer& peer) {
  bt_log(DEBUG, "fidl", "%s", __FUNCTION__);
  if (bonding_delegate_server_) {
    bonding_delegate_server_->OnNewBondingData(peer);
  }
}

void HostServer::RegisterLowEnergyConnection(
    std::unique_ptr<bt::gap::LowEnergyConnectionHandle> conn_ref,
    bool auto_connect) {
  PW_DCHECK(conn_ref);

  bt::PeerId id = conn_ref->peer_identifier();
  auto iter = le_connections_.find(id);
  if (iter != le_connections_.end()) {
    bt_log(
        WARN,
        "fidl",
        "%s: peer already connected; connection reference dropped (peer: %s)",
        __FUNCTION__,
        bt_str(id));
    return;
  }

  bt_log(DEBUG,
         "fidl",
         "LE peer connected (%s): %s ",
         (auto_connect ? "auto" : "direct"),
         bt_str(id));
  conn_ref->set_closed_callback([self = weak_self_.GetWeakPtr(), id] {
    if (self.is_alive())
      self->le_connections_.erase(id);
  });
  le_connections_[id] = std::move(conn_ref);
}

void HostServer::SetDiscoverable(bool discoverable,
                                 SetDiscoverableCallback callback) {
  bt_log(INFO, "fidl", "%s(%s)", __FUNCTION__, discoverable ? "true" : "false");
  // TODO(fxbug.dev/42177512): advertise LE here
  if (!discoverable) {
    bredr_discoverable_session_ = nullptr;
    NotifyInfoChange();
    callback(fpromise::ok());
    return;
  }
  if (discoverable && requesting_discoverable_) {
    bt_log(DEBUG, "fidl", "%s already in progress", __FUNCTION__);
    callback(fpromise::error(fsys::Error::IN_PROGRESS));
    return;
  }
  requesting_discoverable_ = true;
  if (!adapter()->bredr()) {
    callback(fpromise::error(fsys::Error::FAILED));
    return;
  }
  adapter()->bredr()->RequestDiscoverable([self = weak_self_.GetWeakPtr(),
                                           callback = std::move(callback),
                                           func = __FUNCTION__](
                                              bt::hci::Result<> result,
                                              auto session) {
    if (!self.is_alive()) {
      callback(fpromise::error(fsys::Error::FAILED));
      return;
    }

    if (!self->requesting_discoverable_) {
      callback(fpromise::error(fsys::Error::CANCELED));
      return;
    }

    if (result.is_error() || !session) {
      bt_log(ERROR, "fidl", "%s: failed (result: %s)", func, bt_str(result));
      fpromise::result<void, fsys::Error> fidl_result = ResultToFidl(result);
      if (result.is_ok()) {
        PW_CHECK(session == nullptr);
        fidl_result = fpromise::error(fsys::Error::FAILED);
      }
      self->requesting_discoverable_ = false;
      callback(std::move(fidl_result));
      return;
    }

    self->bredr_discoverable_session_ = std::move(session);
    self->requesting_discoverable_ = false;
    self->NotifyInfoChange();
    callback(fpromise::ok());
  });
}

void HostServer::EnableBackgroundScan(bool enabled) {
  bt_log(INFO, "fidl", "%s background scan", (enabled ? "enable" : "disable"));
  if (!adapter()->le()) {
    bt_log(ERROR, "fidl", "%s: adapter does not support LE", __FUNCTION__);
    return;
  }

  if (!enabled) {
    requesting_background_scan_ = false;
    le_background_scan_ = nullptr;
    return;
  }

  // If a scan is already starting or is in progress, there is nothing to do to
  // enable the scan.
  if (requesting_background_scan_ || le_background_scan_) {
    return;
  }

  requesting_background_scan_ = true;
  adapter()->le()->StartDiscovery(
      /*active=*/false, {}, [self = weak_self_.GetWeakPtr()](auto session) {
        if (!self.is_alive()) {
          return;
        }

        // Background scan may have been disabled while discovery was starting.
        if (!self->requesting_background_scan_) {
          return;
        }

        if (!session) {
          bt_log(ERROR, "fidl", "failed to start LE background scan");
          self->le_background_scan_ = nullptr;
          self->requesting_background_scan_ = false;
          return;
        }

        self->le_background_scan_ = std::move(session);
        self->requesting_background_scan_ = false;
      });
}

void HostServer::EnablePrivacy(bool enabled) {
  bt_log(INFO,
         "fidl",
         "%s: %s LE privacy",
         __FUNCTION__,
         (enabled ? "enable" : "disable"));
  if (adapter()->le()) {
    adapter()->le()->EnablePrivacy(enabled);
  }
}

void HostServer::SetBrEdrSecurityMode(fsys::BrEdrSecurityMode mode) {
  std::optional<bt::gap::BrEdrSecurityMode> gap_mode =
      BrEdrSecurityModeFromFidl(mode);
  if (!gap_mode.has_value()) {
    bt_log(WARN, "fidl", "%s: Unrecognized BR/EDR security mode", __FUNCTION__);
    return;
  }

  bt_log(INFO,
         "fidl",
         "%s: %s",
         __FUNCTION__,
         BrEdrSecurityModeToString(gap_mode.value()));
  if (adapter()->bredr()) {
    adapter()->bredr()->SetBrEdrSecurityMode(gap_mode.value());
  }
}

void HostServer::SetLeSecurityMode(fsys::LeSecurityMode mode) {
  bt::gap::LESecurityMode gap_mode = LeSecurityModeFromFidl(mode);
  bt_log(
      INFO, "fidl", "%s: %s", __FUNCTION__, LeSecurityModeToString(gap_mode));
  if (adapter()->le()) {
    adapter()->le()->SetLESecurityMode(gap_mode);
  }
}

void HostServer::SetPairingDelegate(
    fsys::InputCapability input,
    fsys::OutputCapability output,
    ::fidl::InterfaceHandle<fsys::PairingDelegate> delegate) {
  bool cleared = !delegate;
  pairing_delegate_.Bind(std::move(delegate));

  if (cleared) {
    bt_log(INFO, "fidl", "%s: PairingDelegate cleared", __FUNCTION__);
    ResetPairingDelegate();
    return;
  }

  io_capability_ = fidl_helpers::IoCapabilityFromFidl(input, output);
  bt_log(INFO,
         "fidl",
         "%s: PairingDelegate assigned (I/O capability: %s)",
         __FUNCTION__,
         bt::sm::util::IOCapabilityToString(io_capability_).c_str());

  auto pairing = weak_pairing_.GetWeakPtr();
  auto self = weak_self_.GetWeakPtr();
  adapter()->SetPairingDelegate(pairing);
  pairing_delegate_.set_error_handler([self, func = __FUNCTION__](
                                          zx_status_t status) {
    bt_log(
        INFO, "fidl", "%s error handler: PairingDelegate disconnected", func);
    if (self.is_alive()) {
      self->ResetPairingDelegate();
    }
  });
}

// Attempt to connect to peer identified by |peer_id|. The peer must be
// in our peer cache. We will attempt to connect technologies (LowEnergy,
// Classic or Dual-Mode) as the peer claims to support when discovered
void HostServer::Connect(fbt::PeerId peer_id, ConnectCallback callback) {
  bt::PeerId id{peer_id.value};
  bt_log(INFO, "fidl", "%s: (peer: %s)", __FUNCTION__, bt_str(id));

  auto peer = adapter()->peer_cache()->FindById(id);
  if (!peer) {
    // We don't support connecting to peers that are not in our cache
    bt_log(WARN,
           "fidl",
           "%s: peer not found in peer cache (peer: %s)",
           __FUNCTION__,
           bt_str(id));
    callback(fpromise::error(fsys::Error::PEER_NOT_FOUND));
    return;
  }

  // TODO(fxbug.dev/42075069): Dual-mode currently not supported; if the peer
  // supports BR/EDR we prefer BR/EDR. If a dual-mode peer, we should attempt to
  // connect both protocols.
  if (peer->bredr()) {
    ConnectBrEdr(id, std::move(callback));
    return;
  }

  ConnectLowEnergy(id, std::move(callback));
}

// Attempt to disconnect the peer identified by |peer_id| from all transports.
// If the peer is already not connected, return success. If the peer is
// disconnected succesfully, return success.
void HostServer::Disconnect(fbt::PeerId peer_id, DisconnectCallback callback) {
  bt::PeerId id{peer_id.value};
  bt_log(INFO, "fidl", "%s: (peer: %s)", __FUNCTION__, bt_str(id));

  bool le_disc = adapter()->le() ? adapter()->le()->Disconnect(id) : true;
  bool bredr_disc = adapter()->bredr()
                        ? adapter()->bredr()->Disconnect(
                              id, bt::gap::DisconnectReason::kApiRequest)
                        : true;
  if (le_disc && bredr_disc) {
    callback(fpromise::ok());
  } else {
    bt_log(WARN, "fidl", "%s: failed (peer: %s)", __FUNCTION__, bt_str(id));
    callback(fpromise::error(fsys::Error::FAILED));
  }
}

void HostServer::ConnectLowEnergy(PeerId peer_id, ConnectCallback callback) {
  auto self = weak_self_.GetWeakPtr();
  auto on_complete = [self,
                      callback = std::move(callback),
                      peer_id,
                      func = __FUNCTION__](auto result) {
    if (result.is_error()) {
      bt_log(INFO,
             "fidl",
             "%s: failed to connect LE transport to peer (peer: %s)",
             func,
             bt_str(peer_id));
      callback(fpromise::error(HostErrorToFidl(result.error_value())));
      return;
    }

    // We must be connected and to the right peer
    auto connection = std::move(result).value();
    PW_CHECK(connection);
    PW_CHECK(peer_id == connection->peer_identifier());

    callback(fpromise::ok());

    if (self.is_alive())
      self->RegisterLowEnergyConnection(std::move(connection),
                                        /*auto_connect=*/false);
  };

  adapter()->le()->Connect(
      peer_id, std::move(on_complete), bt::gap::LowEnergyConnectionOptions());
}

// Initiate an outgoing Br/Edr connection, unless already connected
// Br/Edr connections are host-wide, and stored in BrEdrConnectionManager
void HostServer::ConnectBrEdr(PeerId peer_id, ConnectCallback callback) {
  auto on_complete = [callback = std::move(callback),
                      peer_id,
                      func = __FUNCTION__](auto status, auto connection) {
    if (status.is_error()) {
      PW_CHECK(!connection);
      bt_log(INFO,
             "fidl",
             "%s: failed to connect BR/EDR transport to peer (peer: %s)",
             func,
             bt_str(peer_id));
      callback(fpromise::error(HostErrorToFidl(status.error_value())));
      return;
    }

    // We must be connected and to the right peer
    PW_CHECK(connection);
    PW_CHECK(peer_id == connection->peer_id());

    callback(fpromise::ok());
  };

  if (!adapter()->bredr()->Connect(peer_id, std::move(on_complete))) {
    bt_log(
        INFO,
        "fidl",
        "%s: failed to initiate BR/EDR transport connection to peer (peer: %s)",
        __FUNCTION__,
        bt_str(peer_id));
    callback(fpromise::error(fsys::Error::FAILED));
  }
}

void HostServer::Forget(fbt::PeerId peer_id, ForgetCallback callback) {
  bt::PeerId id{peer_id.value};
  auto peer = adapter()->peer_cache()->FindById(id);
  if (!peer) {
    bt_log(DEBUG, "fidl", "peer %s to forget wasn't found", bt_str(id));
    callback(fpromise::ok());
    return;
  }

  const bool le_disconnected =
      adapter()->le() ? adapter()->le()->Disconnect(id) : true;
  const bool bredr_disconnected =
      adapter()->bredr() ? adapter()->bredr()->Disconnect(
                               id, bt::gap::DisconnectReason::kApiRequest)
                         : true;
  const bool peer_removed = adapter()->peer_cache()->RemoveDisconnectedPeer(id);

  if (!le_disconnected || !bredr_disconnected) {
    const auto message =
        bt_lib_cpp_string::StringPrintf("link(s) failed to close:%s%s",
                                        le_disconnected ? "" : " LE",
                                        bredr_disconnected ? "" : " BR/EDR");
    callback(fpromise::error(fsys::Error::FAILED));
  } else {
    PW_CHECK(peer_removed);
    callback(fpromise::ok());
  }
}

void HostServer::Pair(fbt::PeerId id,
                      fsys::PairingOptions options,
                      PairCallback callback) {
  auto peer_id = bt::PeerId(id.value);
  auto peer = adapter()->peer_cache()->FindById(peer_id);
  if (!peer) {
    bt_log(WARN, "fidl", "%s: unknown peer %s", __FUNCTION__, bt_str(peer_id));
    // We don't support pairing to peers that are not in our cache
    callback(fpromise::error(fsys::Error::PEER_NOT_FOUND));
    return;
  }

  // If options specifies a transport preference for LE or BR/EDR, we use that.
  // Otherwise, we use whichever transport connection exists, preferring BR/EDR
  // if both connections exist.
  if (options.has_transport()) {
    switch (options.transport()) {
      case fsys::TechnologyType::CLASSIC:
        PairBrEdr(peer_id, std::move(callback));
        return;
      case fsys::TechnologyType::LOW_ENERGY:
        PairLowEnergy(peer_id, std::move(options), std::move(callback));
        return;
      case fsys::TechnologyType::DUAL_MODE:
        break;
    }
  }
  if (peer->bredr() && peer->bredr()->connection_state() !=
                           bt::gap::Peer::ConnectionState::kNotConnected) {
    PairBrEdr(peer_id, std::move(callback));
    return;
  }
  if (peer->le() && peer->le()->connection_state() !=
                        bt::gap::Peer::ConnectionState::kNotConnected) {
    PairLowEnergy(peer_id, std::move(options), std::move(callback));
    return;
  }
  callback(fpromise::error(fsys::Error::PEER_NOT_FOUND));
}

void HostServer::PairLowEnergy(PeerId peer_id,
                               fsys::PairingOptions options,
                               PairCallback callback) {
  std::optional<bt::sm::SecurityLevel> security_level;
  if (options.has_le_security_level()) {
    security_level = SecurityLevelFromFidl(options.le_security_level());
    if (!security_level.has_value()) {
      bt_log(WARN,
             "fidl",
             "%s: pairing options missing LE security level (peer: %s)",
             __FUNCTION__,
             bt_str(peer_id));
      callback(fpromise::error(fsys::Error::INVALID_ARGUMENTS));
      return;
    }
  } else {
    security_level = bt::sm::SecurityLevel::kAuthenticated;
  }
  bt::sm::BondableMode bondable_mode = bt::sm::BondableMode::Bondable;
  if (options.has_bondable_mode() &&
      options.bondable_mode() == fsys::BondableMode::NON_BONDABLE) {
    bondable_mode = bt::sm::BondableMode::NonBondable;
  }
  auto on_complete = [peer_id,
                      callback = std::move(callback),
                      func = __FUNCTION__](bt::sm::Result<> status) {
    if (status.is_error()) {
      bt_log(
          WARN, "fidl", "%s: failed to pair (peer: %s)", func, bt_str(peer_id));
      callback(fpromise::error(HostErrorToFidl(status.error_value())));
    } else {
      callback(fpromise::ok());
    }
  };
  PW_CHECK(adapter()->le());
  adapter()->le()->Pair(
      peer_id, *security_level, bondable_mode, std::move(on_complete));
}

void HostServer::PairBrEdr(PeerId peer_id, PairCallback callback) {
  auto on_complete = [peer_id,
                      callback = std::move(callback),
                      func = __FUNCTION__](bt::hci::Result<> status) {
    if (status.is_error()) {
      bt_log(
          WARN, "fidl", "%s: failed to pair (peer: %s)", func, bt_str(peer_id));
      callback(fpromise::error(HostErrorToFidl(status.error_value())));
    } else {
      callback(fpromise::ok());
    }
  };
  // TODO(fxbug.dev/42135898): Add security parameter to Pair and use that here
  // instead of hardcoding default.
  bt::gap::BrEdrSecurityRequirements security{.authentication = false,
                                              .secure_connections = false};
  PW_CHECK(adapter()->bredr());
  adapter()->bredr()->Pair(peer_id, security, std::move(on_complete));
}

void HostServer::Shutdown() {
  bt_log(INFO, "fidl", "closing FIDL handles");

  // Invalidate all weak pointers. This will guarantee that all pending tasks
  // that reference this HostServer will return early if they run in the future.
  weak_self_.InvalidatePtrs();

  // Destroy all FIDL bindings.
  servers_.clear();

  // Cancel pending requests.
  requesting_discoverable_ = false;
  requesting_background_scan_ = false;

  le_background_scan_ = nullptr;
  bredr_discoverable_session_ = nullptr;

  StopDiscovery(ZX_ERR_CANCELED, /*notify_info_change=*/false);

  // Drop all connections that are attached to this HostServer.
  le_connections_.clear();

  if (adapter()->le()) {
    // Stop background scan if enabled.
    adapter()->le()->EnablePrivacy(false);
    adapter()->le()->set_irk(std::nullopt);
  }

  // Disallow future pairing.
  pairing_delegate_ = nullptr;
  ResetPairingDelegate();

  // Send adapter state change.
  if (binding()->is_bound()) {
    NotifyInfoChange();
  }
}

void HostServer::SetBondingDelegate(
    ::fidl::InterfaceRequest<::fuchsia::bluetooth::host::BondingDelegate>
        request) {
  if (bonding_delegate_server_.has_value()) {
    request.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }
  bonding_delegate_server_.emplace(std::move(request), this);
}

void HostServer::handle_unknown_method(uint64_t ordinal,
                                       bool method_has_response) {
  bt_log(WARN, "fidl", "Received unknown method with ordinal: %lu", ordinal);
}

void HostServer::DiscoverySessionServer::Stop() {
  host_->OnDiscoverySessionServerClose(this);
}

void HostServer::DiscoverySessionServer::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN, "fidl", "Received unknown method with ordinal: %lu", ordinal);
}

HostServer::DiscoverySessionServer::DiscoverySessionServer(
    fidl::InterfaceRequest<::fuchsia::bluetooth::host::DiscoverySession>
        request,
    HostServer* host)
    : ServerBase(this, std::move(request)), host_(host) {
  binding()->set_error_handler([this, host](zx_status_t /*status*/) {
    host->OnDiscoverySessionServerClose(this);
  });
}

HostServer::PeerWatcherServer::PeerWatcherServer(
    ::fidl::InterfaceRequest<::fuchsia::bluetooth::host::PeerWatcher> request,
    bt::gap::PeerCache* peer_cache,
    HostServer* host)
    : ServerBase(this, std::move(request)),
      peer_cache_(peer_cache),
      host_(host),
      weak_self_(this) {
  auto self = weak_self_.GetWeakPtr();

  peer_updated_callback_id_ =
      peer_cache_->add_peer_updated_callback([self](const auto& peer) {
        if (self.is_alive()) {
          self->OnPeerUpdated(peer);
        }
      });
  peer_cache_->set_peer_removed_callback([self](const auto& identifier) {
    if (self.is_alive()) {
      self->OnPeerRemoved(identifier);
    }
  });

  // Initialize the peer watcher with all known connectable peers that are in
  // the cache.
  peer_cache_->ForEach(
      [this](const bt::gap::Peer& peer) { OnPeerUpdated(peer); });

  binding()->set_error_handler(
      [this](zx_status_t /*status*/) { host_->peer_watcher_server_.reset(); });
}

HostServer::PeerWatcherServer::~PeerWatcherServer() {
  // Unregister PeerCache callbacks.
  peer_cache_->remove_peer_updated_callback(peer_updated_callback_id_);
  peer_cache_->set_peer_removed_callback(nullptr);
}

void HostServer::PeerWatcherServer::OnPeerUpdated(const bt::gap::Peer& peer) {
  updated_.insert(peer.identifier());
  removed_.erase(peer.identifier());
  MaybeCallCallback();
}

void HostServer::PeerWatcherServer::OnPeerRemoved(bt::PeerId id) {
  updated_.erase(id);
  removed_.insert(id);
  MaybeCallCallback();
}

void HostServer::PeerWatcherServer::MaybeCallCallback() {
  if (updated_.empty() && removed_.empty()) {
    wake_lease_.reset();
  } else if (!wake_lease_) {
    wake_lease_ = PW_SAPPHIRE_ACQUIRE_LEASE(host_->wake_lease_provider_,
                                            "PeerWatcherServer")
                      .value_or(pw::bluetooth_sapphire::Lease());
  }

  if (!callback_) {
    return;
  }

  if (!removed_.empty()) {
    Removed removed_fidl;
    for (const bt::PeerId& id : removed_) {
      removed_fidl.push_back(fbt::PeerId{id.value()});
    }
    removed_.clear();
    callback_(fhost::PeerWatcher_GetNext_Result::WithResponse(
        fhost::PeerWatcher_GetNext_Response::WithRemoved(
            std::move(removed_fidl))));
    callback_ = nullptr;
    return;
  }

  if (!updated_.empty()) {
    Updated updated_fidl;
    for (const bt::PeerId& id : updated_) {
      bt::gap::Peer* peer = peer_cache_->FindById(id);
      // All ids in |updated_| are assumed to be valid as they would otherwise
      // be in |removed_|.
      PW_CHECK(peer);
      updated_fidl.push_back(fidl_helpers::PeerToFidl(*peer));
    }
    updated_.clear();
    callback_(fhost::PeerWatcher_GetNext_Result::WithResponse(
        fhost::PeerWatcher_GetNext_Response::WithUpdated(
            std::move(updated_fidl))));
    callback_ = nullptr;
    return;
  }
}

void HostServer::PeerWatcherServer::GetNext(
    ::fuchsia::bluetooth::host::PeerWatcher::GetNextCallback callback) {
  if (callback_) {
    binding()->Close(ZX_ERR_BAD_STATE);
    host_->peer_watcher_server_.reset();
    return;
  }
  callback_ = std::move(callback);
  MaybeCallCallback();
}

void HostServer::PeerWatcherServer::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN,
         "fidl",
         "PeerWatcher received unknown method with ordinal %lu",
         ordinal);
}

HostServer::BondingDelegateServer::BondingDelegateServer(
    ::fidl::InterfaceRequest<::fuchsia::bluetooth::host::BondingDelegate>
        request,
    HostServer* host)
    : ServerBase(this, std::move(request)), host_(host) {
  binding()->set_error_handler(
      [this](zx_status_t status) { host_->bonding_delegate_server_.reset(); });
  // Initialize the peer watcher with all known bonded peers that are in the
  // cache.
  host_->adapter()->peer_cache()->ForEach([this](const bt::gap::Peer& peer) {
    if (peer.bonded()) {
      OnNewBondingData(peer);
    }
  });
}

void HostServer::BondingDelegateServer::OnNewBondingData(
    const bt::gap::Peer& peer) {
  updated_.push(
      fidl_helpers::PeerToFidlBondingData(host_->adapter().get(), peer));
  MaybeNotifyWatchBonds();
}

void HostServer::BondingDelegateServer::RestoreBonds(
    ::std::vector<::fuchsia::bluetooth::sys::BondingData> bonds,
    RestoreBondsCallback callback) {
  host_->RestoreBonds(std::move(bonds), std::move(callback));
}
void HostServer::BondingDelegateServer::WatchBonds(
    WatchBondsCallback callback) {
  if (watch_bonds_cb_) {
    binding()->Close(ZX_ERR_ALREADY_EXISTS);
    host_->bonding_delegate_server_.reset();
    return;
  }
  watch_bonds_cb_ = std::move(callback);
  MaybeNotifyWatchBonds();
}

void HostServer::BondingDelegateServer::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN,
         "fidl",
         "BondingDelegate received unknown method with ordinal %lu",
         ordinal);
}

// TODO(fxbug.dev/42158854): Support notifying removed bonds.
void HostServer::BondingDelegateServer::MaybeNotifyWatchBonds() {
  if (!watch_bonds_cb_ || updated_.empty()) {
    return;
  }

  watch_bonds_cb_(fhost::BondingDelegate_WatchBonds_Result::WithResponse(
      ::fuchsia::bluetooth::host::BondingDelegate_WatchBonds_Response::
          WithUpdated(std::move(updated_.front()))));
  updated_.pop();
}

bt::sm::IOCapability HostServer::io_capability() const {
  bt_log(DEBUG,
         "fidl",
         "I/O capability: %s",
         bt::sm::util::IOCapabilityToString(io_capability_).c_str());
  return io_capability_;
}

void HostServer::CompletePairing(PeerId id, bt::sm::Result<> status) {
  bt_log(DEBUG,
         "fidl",
         "pairing complete for peer: %s, status: %s",
         bt_str(id),
         bt_str(status));
  PW_DCHECK(pairing_delegate_);
  pairing_delegate_->OnPairingComplete(fbt::PeerId{id.value()}, status.is_ok());
}

void HostServer::ConfirmPairing(PeerId id, ConfirmCallback confirm) {
  bt_log(
      DEBUG, "fidl", "pairing confirmation request for peer: %s", bt_str(id));
  DisplayPairingRequest(
      id, std::nullopt, fsys::PairingMethod::CONSENT, std::move(confirm));
}

void HostServer::DisplayPasskey(PeerId id,
                                uint32_t passkey,
                                DisplayMethod method,
                                ConfirmCallback confirm) {
  auto fidl_method = fsys::PairingMethod::PASSKEY_DISPLAY;
  if (method == DisplayMethod::kComparison) {
    bt_log(
        DEBUG, "fidl", "compare passkey %06u on peer: %s", passkey, bt_str(id));
    fidl_method = fsys::PairingMethod::PASSKEY_COMPARISON;
  } else {
    bt_log(
        DEBUG, "fidl", "enter passkey %06u on peer: %s", passkey, bt_str(id));
  }
  DisplayPairingRequest(id, passkey, fidl_method, std::move(confirm));
}

void HostServer::RequestPasskey(PeerId id, PasskeyResponseCallback respond) {
  bt_log(DEBUG, "fidl", "passkey request for peer: %s", bt_str(id));
  auto found_peer = adapter()->peer_cache()->FindById(id);
  PW_CHECK(found_peer);
  auto peer = fidl_helpers::PeerToFidl(*found_peer);

  PW_CHECK(pairing_delegate_);
  pairing_delegate_->OnPairingRequest(
      std::move(peer),
      fsys::PairingMethod::PASSKEY_ENTRY,
      0u,
      [respond = std::move(respond), id, func = __FUNCTION__](
          const bool accept, uint32_t entered_passkey) mutable {
        if (!respond) {
          bt_log(WARN,
                 "fidl",
                 "%s: The PairingDelegate invoked the Pairing Request callback "
                 "more than once, which "
                 "should not happen (peer: %s)",
                 func,
                 bt_str(id));
          return;
        }
        bt_log(INFO,
               "fidl",
               "%s: got PairingDelegate response: %s with passkey code \"%u\" "
               "(peer: %s)",
               func,
               accept ? "accept" : "reject",
               entered_passkey,
               bt_str(id));
        if (!accept) {
          respond(-1);
        } else {
          respond(entered_passkey);
        }
      });
}

void HostServer::DisplayPairingRequest(bt::PeerId id,
                                       std::optional<uint32_t> passkey,
                                       fsys::PairingMethod method,
                                       ConfirmCallback confirm) {
  auto found_peer = adapter()->peer_cache()->FindById(id);
  PW_CHECK(found_peer);
  auto peer = fidl_helpers::PeerToFidl(*found_peer);

  PW_CHECK(pairing_delegate_);
  uint32_t displayed_passkey = passkey ? *passkey : 0u;
  pairing_delegate_->OnPairingRequest(
      std::move(peer),
      method,
      displayed_passkey,
      [confirm = std::move(confirm), id, func = __FUNCTION__](
          const bool accept, uint32_t entered_passkey) mutable {
        if (!confirm) {
          bt_log(WARN,
                 "fidl",
                 "%s: The PairingDelegate invoked the Pairing Request callback "
                 "more than once, which "
                 "should not happen (peer: %s)",
                 func,
                 bt_str(id));
          return;
        }
        bt_log(INFO,
               "fidl",
               "%s: got PairingDelegate response: %s, \"%u\" (peer: %s)",
               func,
               accept ? "accept" : "reject",
               entered_passkey,
               bt_str(id));
        confirm(accept);
      });
}

void HostServer::OnConnectionError(Server* server) {
  PW_DCHECK(server);
  servers_.erase(server);
}

void HostServer::ResetPairingDelegate() {
  io_capability_ = IOCapability::kNoInputNoOutput;
  adapter()->SetPairingDelegate(PairingDelegate::WeakPtr());
}

void HostServer::NotifyInfoChange() {
  info_getter_.Set(fidl_helpers::HostInfoToFidl(adapter().get()));
}

}  // namespace bthost
