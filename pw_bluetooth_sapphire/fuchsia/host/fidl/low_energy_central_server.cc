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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_central_server.h"

#include <pw_assert/check.h>
#include <zircon/types.h>

#include <utility>

#include "fuchsia/bluetooth/le/cpp/fidl.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt_client_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/measure_tape/hlcpp_measure_tape_for_peer.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/periodic_advertising_sync_server.h"
#include "pw_bluetooth_sapphire/internal/host/common/error.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Int8;
using fuchsia::bluetooth::Status;

using bt::sm::BondableMode;
using fuchsia::bluetooth::gatt::Client;
using fuchsia::bluetooth::le::ScanFilterPtr;
namespace fble = fuchsia::bluetooth::le;
namespace measure_fble = measure_tape::fuchsia::bluetooth::le;

namespace bthost {

namespace {

bt::gap::LowEnergyConnectionOptions ConnectionOptionsFromFidl(
    const fble::ConnectionOptions& options) {
  BondableMode bondable_mode =
      (!options.has_bondable_mode() || options.bondable_mode())
          ? BondableMode::Bondable
          : BondableMode::NonBondable;

  std::optional<bt::UUID> service_uuid =
      options.has_service_filter()
          ? std::optional(fidl_helpers::UuidFromFidl(options.service_filter()))
          : std::nullopt;

  return bt::gap::LowEnergyConnectionOptions{.bondable_mode = bondable_mode,
                                             .service_uuid = service_uuid};
}

fble::CreateCigError HostErrorToCreateCigError(bt::HostError error) {
  switch (error) {
    case bt::HostError::kInvalidParameters:
      return fble::CreateCigError::INVALID_CIS_PARAMETERS;
    case bt::HostError::kOutOfMemory:
      return fble::CreateCigError::NOT_ENOUGH_RESOURCES;
    default:
      return fble::CreateCigError::UNKNOWN;
  }
}

}  // namespace

LowEnergyCentralServer::LowEnergyCentralServer(
    bt::gap::Adapter::WeakPtr adapter,
    fidl::InterfaceRequest<Central> request,
    bt::gatt::GATT::WeakPtr gatt,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    async_dispatcher_t* dispatcher)
    : AdapterServerBase(std::move(adapter), this, std::move(request)),
      gatt_(std::move(gatt)),
      wake_lease_provider_(wake_lease_provider),
      requesting_scan_deprecated_(false),
      dispatcher_(dispatcher),
      weak_self_(this) {
  PW_CHECK(gatt_.is_alive());
}

LowEnergyCentralServer::~LowEnergyCentralServer() {
  if (scan_instance_) {
    scan_instance_->Close(ZX_OK);
    scan_instance_.reset();
  }
}

std::optional<bt::gap::LowEnergyConnectionHandle*>
LowEnergyCentralServer::FindConnectionForTesting(bt::PeerId identifier) {
  auto conn_iter = connections_deprecated_.find(identifier);
  if (conn_iter != connections_deprecated_.end()) {
    return conn_iter->second.get();
  }
  return std::nullopt;
}

LowEnergyCentralServer::ScanResultWatcherServer::ScanResultWatcherServer(
    bt::gap::Adapter::WeakPtr adapter,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher> watcher,
    fit::callback<void()> error_cb)
    : ServerBase(this, std::move(watcher)),
      adapter_(std::move(adapter)),
      error_callback_(std::move(error_cb)) {
  set_error_handler([this](auto) {
    bt_log(DEBUG, "fidl", "ScanResultWatcher client closed, stopping scan");
    PW_CHECK(error_callback_);
    error_callback_();
  });
}

void LowEnergyCentralServer::ScanResultWatcherServer::Close(
    zx_status_t epitaph) {
  binding()->Close(epitaph);
}

void LowEnergyCentralServer::ScanResultWatcherServer::AddPeers(
    std::unordered_set<bt::PeerId> peers) {
  while (!peers.empty() &&
         updated_peers_.size() < kMaxPendingScanResultWatcherPeers) {
    updated_peers_.insert(peers.extract(peers.begin()));
  }

  if (!peers.empty()) {
    bt_log(
        WARN,
        "fidl",
        "Maximum pending peers (%zu) reached, dropping %zu peers from results",
        kMaxPendingScanResultWatcherPeers,
        peers.size());
  }

  MaybeSendPeers();
}

void LowEnergyCentralServer::ScanResultWatcherServer::Watch(
    WatchCallback callback) {
  bt_log(TRACE, "fidl", "%s", __FUNCTION__);
  if (watch_callback_) {
    bt_log(WARN,
           "fidl",
           "%s: called before previous call completed",
           __FUNCTION__);
    Close(ZX_ERR_CANCELED);
    PW_CHECK(error_callback_);
    error_callback_();
    return;
  }
  watch_callback_ = std::move(callback);
  MaybeSendPeers();
}

void LowEnergyCentralServer::ScanResultWatcherServer::MaybeSendPeers() {
  if (updated_peers_.empty() || !watch_callback_) {
    return;
  }

  // Send as many peers as will fit in the channel.
  const size_t kVectorOverhead =
      sizeof(fidl_message_header_t) + sizeof(fidl_vector_t);
  const size_t kMaxBytes = ZX_CHANNEL_MAX_MSG_BYTES - kVectorOverhead;
  size_t bytes_used = 0;
  std::vector<fble::Peer> peers;
  while (!updated_peers_.empty()) {
    bt::PeerId peer_id = *updated_peers_.begin();
    bt::gap::Peer* peer = adapter_->peer_cache()->FindById(peer_id);
    if (!peer) {
      // The peer has been removed from the peer cache since it was queued, so
      // the stale peer ID should not be sent to the client.
      updated_peers_.erase(peer_id);
      continue;
    }

    fble::Peer fidl_peer = fidl_helpers::PeerToFidlLe(*peer);
    measure_fble::Size peer_size = measure_fble::Measure(fidl_peer);
    PW_CHECK(peer_size.num_handles == 0,
             "Expected fuchsia.bluetooth.le/Peer to not have handles, but "
             "%zu handles found",
             peer_size.num_handles);
    bytes_used += peer_size.num_bytes;
    if (bytes_used > kMaxBytes) {
      // Don't remove the peer that exceeded the size limit. It will be sent in
      // the next batch.
      break;
    }

    updated_peers_.erase(peer_id);
    peers.emplace_back(std::move(fidl_peer));
  }

  // It is possible that all queued peers were stale, so there is nothing to
  // send.
  if (peers.empty()) {
    return;
  }

  watch_callback_(std::move(peers));
}

LowEnergyCentralServer::ScanInstance::ScanInstance(
    bt::gap::Adapter::WeakPtr adapter,
    LowEnergyCentralServer* central_server,
    std::vector<fuchsia::bluetooth::le::Filter> fidl_filters,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher> watcher,
    ScanCallback cb)
    : result_watcher_(adapter,
                      std::move(watcher),
                      /*error_cb=*/
                      [this] {
                        Close(ZX_OK);
                        central_server_->ClearScan();
                      }),
      scan_complete_callback_(std::move(cb)),
      central_server_(central_server),
      adapter_(std::move(adapter)),
      weak_self_(this) {
  std::transform(fidl_filters.begin(),
                 fidl_filters.end(),
                 std::back_inserter(filters_),
                 fidl_helpers::DiscoveryFilterFromFidl);

  // Send all current peers in peer cache that match filters.
  std::unordered_set<bt::PeerId> initial_peers;
  adapter_->peer_cache()->ForEach([&](const bt::gap::Peer& peer) {
    initial_peers.emplace(peer.identifier());
  });
  FilterAndAddPeers(std::move(initial_peers));

  // Subscribe to updated peers.
  peer_updated_callback_id_ = adapter_->peer_cache()->add_peer_updated_callback(
      [this](const bt::gap::Peer& peer) {
        FilterAndAddPeers({peer.identifier()});
      });

  auto self = weak_self_.GetWeakPtr();
  adapter_->le()->StartDiscovery(
      /*active=*/true, filters_, [self](auto session) {
        if (!self.is_alive()) {
          bt_log(
              TRACE, "fidl", "ignoring LE discovery session for canceled Scan");
          return;
        }

        if (!session) {
          bt_log(WARN, "fidl", "failed to start LE discovery session");
          self->Close(ZX_ERR_INTERNAL);
          self->central_server_->ClearScan();
          return;
        }

        session->set_error_callback([self] {
          if (!self.is_alive()) {
            bt_log(TRACE,
                   "fidl",
                   "ignoring LE discovery session error for canceled Scan");
            return;
          }

          bt_log(DEBUG,
                 "fidl",
                 "canceling Scan due to LE discovery session error");
          self->Close(ZX_ERR_INTERNAL);
          self->central_server_->ClearScan();
        });

        self->scan_session_ = std::move(session);
      });
}

LowEnergyCentralServer::ScanInstance::~ScanInstance() {
  // If this scan instance has not already been closed with a more specific
  // status, close with an error status.
  Close(ZX_ERR_INTERNAL);
  adapter_->peer_cache()->remove_peer_updated_callback(
      peer_updated_callback_id_);
}

void LowEnergyCentralServer::ScanInstance::Close(zx_status_t status) {
  if (scan_complete_callback_) {
    result_watcher_.Close(status);
    scan_complete_callback_();
  }
}

void LowEnergyCentralServer::ScanInstance::FilterAndAddPeers(
    std::unordered_set<bt::PeerId> peers) {
  // Remove peers that don't match any filters.
  for (auto peers_iter = peers.begin(); peers_iter != peers.end();) {
    bt::gap::Peer* peer = adapter_->peer_cache()->FindById(*peers_iter);
    if (!peer || !peer->le()) {
      peers_iter = peers.erase(peers_iter);
      continue;
    }
    bool matches_any = false;
    for (const bt::hci::DiscoveryFilter& filter : filters_) {
      // TODO(fxbug.dev/42111894): Match peer names that are not in advertising
      // data. This might require implementing a new peer filtering class, as
      // DiscoveryFilter only filters advertising data.
      if (filter.Matches(peer->le()->parsed_advertising_data(),
                         peer->connectable(),
                         peer->rssi())) {
        matches_any = true;
        break;
      }
    }
    if (!matches_any) {
      peers_iter = peers.erase(peers_iter);
      continue;
    }
    peers_iter++;
  }

  result_watcher_.AddPeers(std::move(peers));
}

void LowEnergyCentralServer::Scan(
    fuchsia::bluetooth::le::ScanOptions options,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher>
        result_watcher,
    ScanCallback callback) {
  bt_log(DEBUG, "fidl", "%s", __FUNCTION__);

  if (scan_instance_ || requesting_scan_deprecated_ ||
      scan_session_deprecated_) {
    bt_log(INFO, "fidl", "%s: scan already in progress", __FUNCTION__);
    result_watcher.Close(ZX_ERR_ALREADY_EXISTS);
    callback();
    return;
  }

  if (!options.has_filters() || options.filters().empty()) {
    bt_log(INFO, "fidl", "%s: no scan filters specified", __FUNCTION__);
    result_watcher.Close(ZX_ERR_INVALID_ARGS);
    callback();
    return;
  }

  scan_instance_ =
      std::make_unique<ScanInstance>(adapter()->AsWeakPtr(),
                                     this,
                                     std::move(*options.mutable_filters()),
                                     std::move(result_watcher),
                                     std::move(callback));
}

void LowEnergyCentralServer::Connect(
    fuchsia::bluetooth::PeerId id,
    fble::ConnectionOptions options,
    fidl::InterfaceRequest<fble::Connection> request) {
  bt::PeerId peer_id(id.value);
  bt_log(INFO, "fidl", "%s: (peer: %s)", __FUNCTION__, bt_str(peer_id));

  auto conn_iter = connections_.find(peer_id);
  if (conn_iter != connections_.end()) {
    bt_log(
        INFO,
        "fidl",
        "%s: connection %s (peer: %s)",
        __FUNCTION__,
        (conn_iter->second == nullptr ? "request pending" : "already exists"),
        bt_str(peer_id));
    request.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }

  auto self = weak_self_.GetWeakPtr();
  auto conn_cb =
      [self, peer_id, request = std::move(request)](
          bt::gap::Adapter::LowEnergy::ConnectionResult result) mutable {
        if (!self.is_alive())
          return;

        auto conn_iter = self->connections_.find(peer_id);
        PW_CHECK(conn_iter != self->connections_.end());
        PW_CHECK(conn_iter->second == nullptr);

        if (result.is_error()) {
          bt_log(INFO,
                 "fidl",
                 "Connect: failed to connect to peer (peer: %s)",
                 bt_str(peer_id));
          self->connections_.erase(peer_id);
          request.Close(ZX_ERR_NOT_CONNECTED);
          return;
        }

        auto conn_ref = std::move(result).value();
        PW_CHECK(conn_ref);
        PW_CHECK(peer_id == conn_ref->peer_identifier());

        auto closed_cb = [self, peer_id] {
          if (self.is_alive()) {
            self->connections_.erase(peer_id);
          }
        };
        auto server = std::make_unique<LowEnergyConnectionServer>(
            self->adapter(),
            self->gatt_,
            self->wake_lease_provider_,
            std::move(conn_ref),
            request.TakeChannel(),
            std::move(closed_cb));

        PW_CHECK(!conn_iter->second);
        conn_iter->second = std::move(server);
      };

  // An entry for the connection must be created here so that a synchronous call
  // to conn_cb below does not cause conn_cb to treat the connection as
  // cancelled.
  connections_[peer_id] = nullptr;

  adapter()->le()->Connect(
      peer_id, std::move(conn_cb), ConnectionOptionsFromFidl(options));
}

void LowEnergyCentralServer::CreateConnectedIsochronousGroup(
    ::fuchsia::bluetooth::le::CentralCreateConnectedIsochronousGroupRequest
        request,
    CreateConnectedIsochronousGroupCallback callback) {
  using Result = fble::Central_CreateConnectedIsochronousGroup_Result;
  using Response = fble::Central_CreateConnectedIsochronousGroup_Response;

  // TODO: https://pwbug.dev/311641762 - Rename INVALID_CIS_PARAMETERS to
  // INVALID_PARAMETERS when we move to the next FIDL API version.
  auto report_invalid_parameters = [&request, &callback](bool close_cig) {
    if (close_cig) {
      request.mutable_cig()->Close(ZX_ERR_INVALID_ARGS);
    }
    Result fidl_result;
    fidl_result.set_err(fble::CreateCigError::INVALID_CIS_PARAMETERS);
    callback(std::move(fidl_result));
  };

  if (!request.has_cig()) {
    bt_log(WARN, "fidl", "%s: No CIG channel to connect.", __FUNCTION__);
    report_invalid_parameters(/*close_cig=*/false);
    return;
  }

  if (!request.has_cig_parameters() ||
      !request.has_cis_requested_parameters()) {
    bt_log(WARN,
           "fidl",
           "%s: Create CIG missing required parameters.",
           __FUNCTION__);
    report_invalid_parameters(/*close_cig=*/true);
    return;
  }

  auto cig_params = fidl_helpers::FidlToCigParams(request.cig_parameters());
  if (!cig_params.has_value()) {
    bt_log(WARN, "fidl", "%s: Invalid CIG parameters.", __FUNCTION__);
    report_invalid_parameters(/*close_cig=*/true);
    return;
  }

  std::vector<bt::iso::CigCisParams> cig_cis_params;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;

  for (auto& cis_param : *request.mutable_cis_requested_parameters()) {
    if (!cis_param.has_cis_id() || !cis_param.has_connection_stream()) {
      bt_log(WARN,
             "fidl",
             "%s: CIS parameter missing CIS ID or connection stream.",
             __FUNCTION__);
      report_invalid_parameters(/*close_cig=*/true);
      return;
    }

    auto cis_id = cis_param.cis_id();
    auto stream_server = std::make_unique<IsoStreamServer>(
        std::move(*cis_param.mutable_connection_stream()), [] {});

    auto stream_server_weak = stream_server->GetWeakPtr();
    auto next_cig_cis_params = fidl_helpers::FidlToCigCisParams(
        cis_param,
        [server = std::move(stream_server_weak)](
            auto status, auto stream, const auto& params) {
          if (!server.is_alive()) {
            return;
          }
          if (status == pw::bluetooth::emboss::StatusCode::SUCCESS &&
              stream.has_value() && params.has_value()) {
            server->OnStreamEstablishmentSuccess(std::move(*stream),
                                                 std::move(*params));
            return;
          }
          server->OnStreamEstablishmentFailed(status);
        });

    if (!next_cig_cis_params.has_value()) {
      bt_log(WARN,
             "fidl",
             "%s: Invalid CIS parameters (CIS ID: %u).",
             __FUNCTION__,
             cis_id);
      report_invalid_parameters(/*close_cig=*/true);
      return;
    }

    cig_cis_params.emplace_back(std::move(*next_cig_cis_params));
    auto [_, success] =
        stream_servers.try_emplace(cis_id, std::move(stream_server));

    if (!success) {
      bt_log(WARN,
             "fidl",
             "%s: Duplicate CIS ID provided (%u)",
             __FUNCTION__,
             cis_id);
      report_invalid_parameters(/*close_cig=*/true);
      return;
    }
  }

  std::vector<bt::PeerId> expected_peers;
  if (request.cig_parameters().has_expected_peers()) {
    for (const auto& peer_id : request.cig_parameters().expected_peers()) {
      expected_peers.push_back(bt::PeerId(peer_id.value));
    }
  }

  auto self = weak_self_.GetWeakPtr();
  auto server = std::make_unique<ConnectedIsochronousGroupServer>(
      adapter(),
      std::move(*request.mutable_cig()),
      /*iso_group=*/bt::iso::IsoGroup::WeakPtr(),
      std::move(stream_servers),
      [self](auto& cig_server, auto status) {
        if (self.is_alive()) {
          auto cig_id = cig_server.cig_id();
          if (cig_id.has_value()) {
            self->cig_servers_.erase(*cig_id);
          }
        }
      });

  adapter()->le()->CreateCig(
      std::move(*cig_params),
      std::move(cig_cis_params),
      [self, server = std::move(server), callback = std::move(callback)](
          auto result) mutable {
        if (!self.is_alive()) {
          return;
        }

        // Check if the channel was closed by the client during CIG creation.
        if (!server->is_bound()) {
          bt_log(INFO, "fidl", "CIG channel closed during CIG creation");
          return;
        }

        Result fidl_result;
        if (!result.has_value()) {
          bt_log(WARN,
                 "fidl",
                 "%s: failed to create CIG: %s",
                 __FUNCTION__,
                 bt::HostErrorToString(result.error()).c_str());
          server->Close(ZX_ERR_INTERNAL);
          fidl_result.set_err(HostErrorToCreateCigError(result.error()));
          callback(std::move(fidl_result));
          return;
        }

        server->set_iso_group(result.value());

        auto cig_id = result.value()->id();
        self->cig_servers_.try_emplace(cig_id, std::move(server));

        Response response{};
        response.set_cig_id(cig_id);
        fidl_result.set_response(std::move(response));

        bt_log(INFO, "fidl", "Responding to callback, CIG %d created.", cig_id);
        callback(std::move(fidl_result));
      },
      [self](auto& cig) {
        if (self.is_alive()) {
          self->cig_servers_.erase(cig.id());
        }
      },
      std::move(expected_peers));
}

void LowEnergyCentralServer::GetPeripherals(
    ::fidl::VectorPtr<::std::string> service_uuids,
    GetPeripheralsCallback callback) {
  bt_log(ERROR, "fidl", "GetPeripherals() not implemented");
}

void LowEnergyCentralServer::GetPeripheral(::std::string identifier,
                                           GetPeripheralCallback callback) {
  bt_log(ERROR, "fidl", "GetPeripheral() not implemented");
}

void LowEnergyCentralServer::StartScan(ScanFilterPtr filter,
                                       StartScanCallback callback) {
  bt_log(DEBUG, "fidl", "%s", __FUNCTION__);

  if (requesting_scan_deprecated_) {
    bt_log(DEBUG, "fidl", "%s: scan request already in progress", __FUNCTION__);
    callback(fidl_helpers::NewFidlError(ErrorCode::IN_PROGRESS,
                                        "Scan request in progress"));
    return;
  }

  if (filter && !fidl_helpers::IsScanFilterValid(*filter)) {
    bt_log(WARN, "fidl", "%s: invalid scan filter given", __FUNCTION__);
    callback(fidl_helpers::NewFidlError(ErrorCode::INVALID_ARGUMENTS,
                                        "ScanFilter contains an invalid UUID"));
    return;
  }

  std::vector<bt::hci::DiscoveryFilter> discovery_filters;
  if (filter) {
    bt::hci::DiscoveryFilter discovery_filter;
    fidl_helpers::PopulateDiscoveryFilter(*filter, &discovery_filter);
    discovery_filters.emplace_back(discovery_filter);
  }

  requesting_scan_deprecated_ = true;
  adapter()->le()->StartDiscovery(
      /*active=*/true,
      discovery_filters,
      [self = weak_self_.GetWeakPtr(),
       filter = std::move(filter),
       callback = std::move(callback),
       func = __FUNCTION__](auto session) {
        if (!self.is_alive())
          return;

        self->requesting_scan_deprecated_ = false;

        if (!session) {
          bt_log(
              WARN, "fidl", "%s: failed to start LE discovery session", func);
          callback(fidl_helpers::NewFidlError(
              ErrorCode::FAILED, "Failed to start discovery session"));
          return;
        }

        session->SetResultCallback([self](const auto& peer) {
          if (self.is_alive())
            self->OnScanResult(peer);
        });

        session->set_error_callback([self] {
          if (self.is_alive()) {
            // Clean up the session and notify the delegate.
            self->StopScan();
          }
        });

        self->scan_session_deprecated_ = std::move(session);
        self->NotifyScanStateChanged(true);
        callback(Status());
      });
}

void LowEnergyCentralServer::StopScan() {
  bt_log(DEBUG, "fidl", "StopScan()");

  if (!scan_session_deprecated_) {
    bt_log(DEBUG,
           "fidl",
           "%s: no active discovery session; nothing to do",
           __FUNCTION__);
    return;
  }

  scan_session_deprecated_ = nullptr;
  NotifyScanStateChanged(false);
}

void LowEnergyCentralServer::ConnectPeripheral(
    ::std::string identifier,
    fuchsia::bluetooth::le::ConnectionOptions connection_options,
    ::fidl::InterfaceRequest<Client> client_request,
    ConnectPeripheralCallback callback) {
  bt_log(INFO, "fidl", "%s: (peer: %s)", __FUNCTION__, identifier.c_str());

  auto peer_id = fidl_helpers::PeerIdFromString(identifier);
  if (!peer_id.has_value()) {
    bt_log(WARN,
           "fidl",
           "%s: invalid peer id : %s",
           __FUNCTION__,
           identifier.c_str());
    callback(fidl_helpers::NewFidlError(ErrorCode::INVALID_ARGUMENTS,
                                        "invalid peer ID"));
    return;
  }

  auto iter = connections_deprecated_.find(*peer_id);
  if (iter != connections_deprecated_.end()) {
    if (iter->second) {
      bt_log(INFO,
             "fidl",
             "%s: already connected to %s",
             __FUNCTION__,
             bt_str(*peer_id));
      callback(fidl_helpers::NewFidlError(
          ErrorCode::ALREADY, "Already connected to requested peer"));
    } else {
      bt_log(INFO,
             "fidl",
             "%s: connect request pending (peer: %s)",
             __FUNCTION__,
             bt_str(*peer_id));
      callback(fidl_helpers::NewFidlError(ErrorCode::IN_PROGRESS,
                                          "Connect request pending"));
    }
    return;
  }

  auto self = weak_self_.GetWeakPtr();
  auto conn_cb = [self,
                  callback = callback.share(),
                  peer_id = *peer_id,
                  request = std::move(client_request),
                  func = __FUNCTION__](auto result) mutable {
    if (!self.is_alive())
      return;

    auto iter = self->connections_deprecated_.find(peer_id);
    if (iter == self->connections_deprecated_.end()) {
      bt_log(
          INFO,
          "fidl",
          "%s: connect request canceled during connection procedure (peer: %s)",
          func,
          bt_str(peer_id));
      auto error = fidl_helpers::NewFidlError(ErrorCode::FAILED,
                                              "Connect request canceled");
      callback(std::move(error));
      return;
    }

    if (result.is_error()) {
      bt_log(INFO,
             "fidl",
             "%s: failed to connect to peer (peer: %s)",
             func,
             bt_str(peer_id));
      self->connections_deprecated_.erase(peer_id);
      callback(fidl_helpers::ResultToFidlDeprecated(
          bt::ToResult(result.error_value()), "failed to connect"));
      return;
    }

    auto conn_ref = std::move(result).value();
    PW_CHECK(conn_ref);
    PW_CHECK(peer_id == conn_ref->peer_identifier());

    if (self->gatt_client_servers_.find(peer_id) !=
        self->gatt_client_servers_.end()) {
      bt_log(WARN,
             "fidl",
             "only 1 gatt.Client FIDL handle allowed per peer (%s)",
             bt_str(peer_id));
      // The handle owned by |request| will be closed.
      return;
    }

    auto server = std::make_unique<GattClientServer>(
        peer_id, self->gatt_, std::move(request));
    server->set_error_handler([self, peer_id](zx_status_t status) {
      if (self.is_alive()) {
        bt_log(DEBUG, "bt-host", "GATT client disconnected");
        self->gatt_client_servers_.erase(peer_id);
      }
    });
    self->gatt_client_servers_.emplace(peer_id, std::move(server));

    conn_ref->set_closed_callback([self, peer_id] {
      if (self.is_alive() &&
          self->connections_deprecated_.erase(peer_id) != 0) {
        bt_log(INFO,
               "fidl",
               "peripheral connection closed (peer: %s)",
               bt_str(peer_id));
        self->gatt_client_servers_.erase(peer_id);
        self->NotifyPeripheralDisconnected(peer_id);
      }
    });

    PW_CHECK(!iter->second);
    iter->second = std::move(conn_ref);
    callback(Status());
  };

  // An entry for the connection must be created here so that a synchronous call
  // to conn_cb below does not cause conn_cb to treat the connection as
  // cancelled.
  connections_deprecated_[*peer_id] = nullptr;

  adapter()->le()->Connect(*peer_id,
                           std::move(conn_cb),
                           ConnectionOptionsFromFidl(connection_options));
}

void LowEnergyCentralServer::DisconnectPeripheral(
    ::std::string identifier, DisconnectPeripheralCallback callback) {
  auto peer_id = fidl_helpers::PeerIdFromString(identifier);
  if (!peer_id.has_value()) {
    bt_log(WARN,
           "fidl",
           "%s: invalid peer id : %s",
           __FUNCTION__,
           identifier.c_str());
    callback(fidl_helpers::NewFidlError(ErrorCode::INVALID_ARGUMENTS,
                                        "invalid peer ID"));
    return;
  }

  auto iter = connections_deprecated_.find(*peer_id);
  if (iter == connections_deprecated_.end()) {
    bt_log(INFO,
           "fidl",
           "%s: client not connected to peer (peer: %s)",
           __FUNCTION__,
           identifier.c_str());
    callback(Status());
    return;
  }

  // If a request to this peer is pending then the request will be canceled.
  bool was_pending = !iter->second;
  connections_deprecated_.erase(iter);

  if (was_pending) {
    bt_log(INFO,
           "fidl",
           "%s: canceling connection request (peer: %s)",
           __FUNCTION__,
           bt_str(*peer_id));
  } else {
    gatt_client_servers_.erase(*peer_id);
    NotifyPeripheralDisconnected(*peer_id);
  }

  callback(Status());
}

void LowEnergyCentralServer::SyncToPeriodicAdvertising(
    fble::CentralSyncToPeriodicAdvertisingRequest request) {
  if (!request.has_sync()) {
    bt_log(WARN, "fidl", "%s: request missing sync field", __FUNCTION__);
    return;
  }
  fidl::InterfaceRequest<fble::PeriodicAdvertisingSync> sync =
      std::move(*request.mutable_sync());

  if (!request.has_peer_id()) {
    bt_log(WARN, "fidl", "%s: request missing peer_id field", __FUNCTION__);
    sync.Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  bt::PeerId peer_id(request.peer_id().value);

  if (!request.has_advertising_sid()) {
    bt_log(WARN,
           "fidl",
           "%s: request missing advertising_sid field",
           __FUNCTION__);
    sync.Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  uint8_t advertising_sid = request.advertising_sid();

  bt::gap::Adapter::LowEnergy::SyncOptions options{.filter_duplicates = true};
  if (request.has_config() && request.config().has_filter_duplicates()) {
    options.filter_duplicates = request.config().filter_duplicates();
  }

  // Convert the legacy FIDL binding fidl::InterfaceRequest into a new FIDL
  // binding fidl::ServerEnd by converting it into a raw channel.
  fidl::ServerEnd<::fuchsia_bluetooth_le::PeriodicAdvertisingSync> server_end(
      sync.TakeChannel());

  size_t server_id = next_server_id_++;
  auto closed_cb = [this, server_id]() {
    periodic_advertising_sync_servers_.erase(server_id);
  };
  std::unique_ptr<PeriodicAdvertisingSyncServer> server =
      PeriodicAdvertisingSyncServer::Create(dispatcher_,
                                            std::move(server_end),
                                            adapter()->AsWeakPtr(),
                                            peer_id,
                                            advertising_sid,
                                            options,
                                            closed_cb);
  if (!server) {
    // The PeriodicAdvertisingSyncServer will have already notified the client.
    return;
  }

  auto [_, inserted] = periodic_advertising_sync_servers_.try_emplace(
      server_id, std::move(server));
  PW_CHECK(inserted);
}

void LowEnergyCentralServer::ListenL2cap(
    fble::ChannelListenerRegistryListenL2capRequest request,
    ListenL2capCallback callback) {
  // TODO(fxbug.dev/42178956): Implement ListenL2cap.
  fble::ChannelListenerRegistry_ListenL2cap_Result result;
  callback(std::move(result.set_err(ZX_ERR_NOT_SUPPORTED)));
}

void LowEnergyCentralServer::OnScanResult(const bt::gap::Peer& peer) {
  auto fidl_device = fidl_helpers::NewLERemoteDevice(peer);
  if (!fidl_device) {
    return;
  }

  if (peer.rssi() != bt::hci_spec::kRSSIInvalid) {
    fidl_device->rssi = std::make_unique<Int8>();
    fidl_device->rssi->value = peer.rssi();
  }

  binding()->events().OnDeviceDiscovered(std::move(*fidl_device));
}

void LowEnergyCentralServer::NotifyScanStateChanged(bool scanning) {
  binding()->events().OnScanStateChanged(scanning);
}

void LowEnergyCentralServer::NotifyPeripheralDisconnected(bt::PeerId peer_id) {
  binding()->events().OnPeripheralDisconnected(peer_id.ToString());
}

}  // namespace bthost
