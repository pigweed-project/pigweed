// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/connected_isochronous_group_server.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include <cinttypes>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"

namespace bthost {

ConnectedIsochronousGroupServer::ConnectedIsochronousGroupServer(
    bt::gap::Adapter::WeakPtr adapter,
    fidl::InterfaceRequest<fuchsia::bluetooth::le::ConnectedIsochronousGroup>
        request,
    bt::iso::IsoGroup::WeakPtr iso_group,
    std::unordered_map<bt::hci_spec::CisIdentifier,
                       std::unique_ptr<IsoStreamServer>> stream_servers,
    ConnectedIsochronousGroupServer::OnCloseCallback on_close_cb)
    : AdapterServerBase(adapter, this, std::move(request)),
      on_close_cb_(std::move(on_close_cb)),
      iso_group_(iso_group),
      iso_stream_servers_(std::move(stream_servers)),
      weak_self_(this) {
  set_error_handler([this](zx_status_t status) { OnClose(status); });

  auto self = GetWeakPtr();
  for (auto& [cis_id, stream_server] : iso_stream_servers_) {
    stream_server->set_on_closed_cb([self, cis_id] {
      async::PostTask(async_get_default_dispatcher(), [self, cis_id] {
        if (self.is_alive()) {
          self->iso_stream_servers_.erase(cis_id);
          self->acl_connection_handles_.erase(cis_id);
        }
      });
    });
  }
}

void ConnectedIsochronousGroupServer::Close(zx_status_t epitaph) {
  binding()->Close(epitaph);
  OnClose(epitaph);
}

std::optional<bt::hci_spec::CigIdentifier>
ConnectedIsochronousGroupServer::cig_id() {
  if (iso_group_.is_alive()) {
    return iso_group_->id();
  }
  return std::nullopt;
}

void ConnectedIsochronousGroupServer::OnClose(zx_status_t status) {
  if (on_close_cb_) {
    on_close_cb_(*this, status);
  }
}

void ConnectedIsochronousGroupServer::EstablishStreams(
    fuchsia::bluetooth::le::ConnectedIsochronousGroupEstablishStreamsRequest
        request,
    EstablishStreamsCallback callback) {
  if (pending_establishment_.has_value()) {
    bt_log(WARN,
           "fidl",
           "ConnectedIsochronousGroupServer: EstablishStreams called while "
           "another request is pending. Closing channel.");
    Close(ZX_ERR_BAD_STATE);
    return;
  }

  if (!iso_group_.is_alive()) {
    bt_log(WARN, "fidl", "IsoGroup destroyed before establish call");
    Close(ZX_ERR_INTERNAL);
    return;
  }

  fuchsia::bluetooth::le::ConnectedIsochronousGroup_EstablishStreams_Result
      result{};

  if (!request.has_cis_params() || request.cis_params().empty()) {
    bt_log(WARN,
           "fidl",
           "ConnectedIsochronousGroupServer: EstablishStreams called with "
           "no CIS parameters. Closing channel.");
    Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  std::vector<bt::iso::IsoGroup::CreateCisData> establish_data;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<bt::gap::LowEnergyConnectionHandle>>
      current_handles;
  std::unordered_set<bt::hci_spec::CisIdentifier> requested_ids;

  for (auto& cis_param : request.cis_params()) {
    if (!cis_param.has_cis_id()) {
      bt_log(WARN,
             "fidl",
             "ConnectedIsochronousGroupServer: EstablishStreams parameter "
             "missing CIS ID. Closing channel.");
      Close(ZX_ERR_INVALID_ARGS);
      return;
    }
    if (!cis_param.has_id()) {
      bt_log(WARN,
             "fidl",
             "ConnectedIsochronousGroupServer: EstablishStreams parameter "
             "missing peer ID. Closing channel.");
      Close(ZX_ERR_INVALID_ARGS);
      return;
    }

    auto cis_id = cis_param.cis_id();

    // Check for duplicate CIS ID in the request parameters.
    if (requested_ids.count(cis_id) > 0) {
      bt_log(WARN,
             "fidl",
             "ConnectedIsochronousGroupServer: Duplicate CIS ID %u in "
             "EstablishStreams request",
             cis_id);
      result.set_err(
          fuchsia::bluetooth::le::EstablishStreamsError::DUPLICATE_CIS);
      callback(std::move(result));
      return;
    }
    requested_ids.insert(cis_id);

    // Check if the CIS is already established.
    if (acl_connection_handles_.find(cis_id) != acl_connection_handles_.end()) {
      bt_log(WARN,
             "fidl",
             "ConnectedIsochronousGroupServer: CIS %u already established",
             cis_id);
      result.set_err(fuchsia::bluetooth::le::EstablishStreamsError::
                         CIS_ALREADY_ESTABLISHED);
      callback(std::move(result));
      return;
    }

    // Check if the peer supports the Connected Isochronous Stream (CIS)
    // Peripheral feature.
    bt::gap::Peer* peer =
        adapter()->peer_cache()->FindById(bt::PeerId(cis_param.id().value));
    if (peer && peer->le() && peer->le()->features().has_value()) {
      bool peer_cis_supported =
          (peer->le()->features().value() &
           static_cast<uint64_t>(bt::hci_spec::LESupportedFeature::
                                     kConnectedIsochronousStreamPeripheral));
      if (!peer_cis_supported) {
        bt_log(WARN,
               "fidl",
               "ConnectedIsochronousGroupServer: Peer %" PRIu64
               " does not support CIS",
               cis_param.id().value);
        result.set_err(
            fuchsia::bluetooth::le::EstablishStreamsError::NOT_SUPPORTED);
        callback(std::move(result));
        return;
      }
    }

    if (iso_stream_servers_.find(cis_id) == iso_stream_servers_.end()) {
      bt_log(WARN,
             "fidl",
             "ConnectedIsochronousGroupServer: EstablishStreams called for "
             "untracked CIS %u. Closing channel.",
             cis_id);
      Close(ZX_ERR_INVALID_ARGS);
      return;
    }

    auto ref =
        adapter()->le()->AddConnectionRef(bt::PeerId(cis_param.id().value));
    if (!ref) {
      bt_log(WARN,
             "fidl",
             "ConnectedIsochronousGroupServer: Peer %" PRIu64 " not connected",
             cis_param.id().value);
      result.set_err(
          fuchsia::bluetooth::le::EstablishStreamsError::PEER_NOT_CONNECTED);
      callback(std::move(result));
      return;
    }

    bt::hci_spec::ConnectionHandle acl_handle = ref->handle();
    current_handles.emplace(cis_id, std::move(ref));

    std::optional<pw::bluetooth::emboss::LESleepClockAccuracyRange> sca;
    if (peer && peer->le()) {
      sca = peer->le()->sleep_clock_accuracy();
    }
    establish_data.push_back(bt::iso::IsoGroup::CreateCisData{
        .peer_id = bt::PeerId(cis_param.id().value),
        .cis_id = cis_id,
        .acl_handle = acl_handle,
        .sca = sca,
    });
  }

  pw::Status status = iso_group_->CreateCises(establish_data);
  if (!status.ok()) {
    bt_log(WARN,
           "fidl",
           "%s: Failed to establish CISes: %s",
           __FUNCTION__,
           status.str());
    if (status == pw::Status::OutOfRange()) {
      result.set_err(fuchsia::bluetooth::le::EstablishStreamsError::
                         PEER_PARAMETERS_OUT_OF_RANGE);
      callback(std::move(result));
      return;
    }
    Close(ZX_ERR_INTERNAL);
    return;
  }

  for (auto& [cis_id, handle] : current_handles) {
    acl_connection_handles_.emplace(cis_id, std::move(handle));
  }

  pending_establishment_ = PendingEstablishment{
      .callback = std::move(callback),
      .requested_ids = requested_ids,
      .pending_cis_ids = std::move(requested_ids),
  };

  for (auto cis_id : pending_establishment_->pending_cis_ids) {
    iso_stream_servers_[cis_id]->set_establishment_callback(
        [this, cis_id](pw::bluetooth::emboss::StatusCode status) {
          OnCisEstablished(cis_id, status);
        });
  }
}

void ConnectedIsochronousGroupServer::OnCisEstablished(
    bt::hci_spec::CisIdentifier cis_id,
    pw::bluetooth::emboss::StatusCode status) {
  if (!pending_establishment_.has_value()) {
    return;
  }

  if (status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    bt_log(WARN,
           "fidl",
           "%s: CIS %u failed to establish: %u. Returning NOT_SUPPORTED.",
           __FUNCTION__,
           cis_id,
           static_cast<unsigned>(status));
    fuchsia::bluetooth::le::ConnectedIsochronousGroup_EstablishStreams_Result
        result;
    result.set_err(
        fuchsia::bluetooth::le::EstablishStreamsError::NOT_SUPPORTED);

    auto pending_callback = std::move(pending_establishment_->callback);
    auto ids_to_close = std::move(pending_establishment_->requested_ids);
    pending_establishment_.reset();

    pending_callback(std::move(result));

    // Close all streams that were part of this establishment request
    // asynchronously
    for (auto req_id : ids_to_close) {
      auto it = iso_stream_servers_.find(req_id);
      if (it != iso_stream_servers_.end()) {
        bt_log(WARN,
               "fidl",
               "ConnectedIsochronousGroupServer: Closing stream %u due to "
               "establishment failure",
               req_id);
        async::PostTask(async_get_default_dispatcher(),
                        [server_weak = it->second->GetWeakPtr()]() {
                          if (server_weak.is_alive()) {
                            server_weak->Close(ZX_ERR_INTERNAL);
                          }
                        });
      }
    }
    return;
  }

  pending_establishment_->pending_cis_ids.erase(cis_id);

  if (pending_establishment_->pending_cis_ids.empty()) {
    auto pending = std::move(*pending_establishment_);
    pending_establishment_.reset();

    fuchsia::bluetooth::le::ConnectedIsochronousGroup_EstablishStreams_Result
        result;
    fuchsia::bluetooth::le::ConnectedIsochronousGroup_EstablishStreams_Response
        response;
    result.set_response(std::move(response));
    pending.callback(std::move(result));
  }
}

void ConnectedIsochronousGroupServer::handle_unknown_method(uint64_t ordinal,
                                                            bool has_response) {
  bt_log(WARN,
         "fidl",
         "Received unknown fidl call %#" PRIx64 " (%s responses)",
         ordinal,
         has_response ? "with" : "without");
}

}  // namespace bthost
