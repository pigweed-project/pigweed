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

#pragma once

#include <fuchsia/bluetooth/le/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/result.h>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/iso_stream_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/server_base.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"

namespace bthost {

class ConnectedIsochronousGroupServer final
    : public AdapterServerBase<
          fuchsia::bluetooth::le::ConnectedIsochronousGroup> {
 public:
  using OnCloseCallback =
      pw::Function<void(ConnectedIsochronousGroupServer&, zx_status_t)>;

  explicit ConnectedIsochronousGroupServer(
      bt::gap::Adapter::WeakPtr adapter,
      fidl::InterfaceRequest<fuchsia::bluetooth::le::ConnectedIsochronousGroup>
          request,
      bt::iso::IsoGroup::WeakPtr iso_group,
      std::unordered_map<bt::hci_spec::CisIdentifier,
                         std::unique_ptr<IsoStreamServer>> stream_servers,
      OnCloseCallback on_close_cb);

  bool is_bound() { return binding()->is_bound(); }
  void Close(zx_status_t epitaph = ZX_OK);

  using WeakPtr = WeakSelf<ConnectedIsochronousGroupServer>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

  std::optional<bt::hci_spec::CigIdentifier> cig_id();

  void OnCisEstablished(bt::hci_spec::CisIdentifier cis_id,
                        pw::bluetooth::emboss::StatusCode status);

 private:
  void OnClose(zx_status_t status);

  // fuchsia::bluetooth::le::ConnectedIsochronousGroup overrides:
  void EstablishStreams(
      fuchsia::bluetooth::le::ConnectedIsochronousGroupEstablishStreamsRequest
          request,
      EstablishStreamsCallback callback) override;
  void Remove() override { Close(ZX_OK); }
  void handle_unknown_method(uint64_t ordinal, bool has_response) override;

  struct PendingEstablishment {
    // Callback to respond to the FIDL client.
    EstablishStreamsCallback callback;

    // The set of all CIS IDs requested in this batch, used to identify which
    // streams to close if establishment fails.
    std::unordered_set<bt::hci_spec::CisIdentifier> requested_ids;

    // The set of CIS IDs we are still waiting to be established.
    std::unordered_set<bt::hci_spec::CisIdentifier> pending_cis_ids;
  };
  std::optional<PendingEstablishment> pending_establishment_;

  OnCloseCallback on_close_cb_;
  bt::iso::IsoGroup::WeakPtr iso_group_;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      iso_stream_servers_;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<bt::gap::LowEnergyConnectionHandle>>
      acl_connection_handles_;

  WeakSelf<ConnectedIsochronousGroupServer> weak_self_;

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(ConnectedIsochronousGroupServer);
};

}  // namespace bthost
