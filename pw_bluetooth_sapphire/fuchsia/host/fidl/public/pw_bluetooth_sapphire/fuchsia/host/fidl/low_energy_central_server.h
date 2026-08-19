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

#pragma once

#include <fuchsia/bluetooth/le/cpp/fidl.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "lib/fidl/cpp/binding.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/connected_isochronous_group_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt_client_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/iso_stream_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_connection_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/periodic_advertising_sync_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/server_base.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"

namespace bthost {

// Implements the low_energy::Central FIDL interface.
class LowEnergyCentralServer
    : public AdapterServerBase<fuchsia::bluetooth::le::Central> {
 public:
  // The maximum number of peers that will be queued for a
  // ScanResultWatcher.Watch call. This hard limit prevents unbounded memory
  // usage for unresponsive clients. The value is mostly arbitrary, as queued
  // `PeerId`s are small and peak memory usage, occurring when creating a vector
  // of FIDL `le.Peer`s, is limited by the size of the FIDL channel.
  constexpr static const size_t kMaxPendingScanResultWatcherPeers = 100;

  LowEnergyCentralServer(
      bt::gap::Adapter::WeakPtr adapter,
      ::fidl::InterfaceRequest<fuchsia::bluetooth::le::Central> request,
      bt::gatt::GATT::WeakPtr gatt,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      async_dispatcher_t* dispatcher);
  ~LowEnergyCentralServer() override;

  // Returns the connection pointer in the connections_deprecated_ map, if it
  // exists. The pointer will be nullptr if a request is pending. Should only be
  // used for testing.
  std::optional<bt::gap::LowEnergyConnectionHandle*> FindConnectionForTesting(
      bt::PeerId identifier);

  size_t CigServersCountForTesting() const { return cig_servers_.size(); }

 private:
  class ScanResultWatcherServer
      : public ServerBase<fuchsia::bluetooth::le::ScanResultWatcher> {
   public:
    using WatchCallbackOnce =
        fit::callback<void(std::vector<fuchsia::bluetooth::le::Peer>)>;

    // `error_cb` will be called when the client closes the protocol.
    ScanResultWatcherServer(
        bt::gap::Adapter::WeakPtr adapter,
        fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher>
            watcher,
        fit::callback<void()> error_cb);
    ~ScanResultWatcherServer() override = default;

    // Closes the protocol and sends `epitaph` as the epitaph. Idempotent.
    void Close(zx_status_t epitaph);

    // Queue `peers` to be sent in response to `Watch()`.
    void AddPeers(std::unordered_set<bt::PeerId> peers);

    // fuchsia::bluetooth::le::ScanResultWatcher overrides:
    void Watch(WatchCallback callback) override;

   private:
    // If the client has a pending `Watch()`, send the maximum number of peers
    // that will fit in the channel.
    void MaybeSendPeers();

    bt::gap::Adapter::WeakPtr adapter_;
    std::unordered_set<bt::PeerId> updated_peers_;
    WatchCallbackOnce watch_callback_ = nullptr;
    fit::callback<void()> error_callback_;
  };

  // ScanInstance represents a call to `Scan` that has not stopped yet.
  class ScanInstance {
   public:
    using ScanCompleteCallback = fit::callback<void()>;

    ScanInstance(
        bt::gap::Adapter::WeakPtr adapter,
        LowEnergyCentralServer* central_server,
        std::vector<fuchsia::bluetooth::le::Filter> filters,
        fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher>
            watcher,
        ScanCallback cb);
    ~ScanInstance();
    // Closes the ScanResultWatcher protocol with the epitaph `status` and sends
    // an empty response to `Scan`. Idempotent.
    void Close(zx_status_t status);

    // Queue peers to be sent to the client via `ScanResultWatcher.Watch`.
    // `peers` will be filtered by the client's `ScanOptions` filters before
    // being sent.
    void FilterAndAddPeers(std::unordered_set<bt::PeerId> peers);

   private:
    std::unique_ptr<bt::gap::LowEnergyDiscoverySession> scan_session_;
    ScanResultWatcherServer result_watcher_;
    // Callback used to send an empty response to the client's `Scan()` call.
    ScanCompleteCallback scan_complete_callback_;
    bt::gap::PeerCache::CallbackId peer_updated_callback_id_;
    // The filters specified in `ScanOptions`.
    std::vector<bt::hci::DiscoveryFilter> filters_;
    LowEnergyCentralServer* central_server_;
    bt::gap::Adapter::WeakPtr adapter_;
    WeakSelf<ScanInstance> weak_self_;
  };

  // fuchsia::bluetooth::le::Central overrides:
  void Scan(fuchsia::bluetooth::le::ScanOptions options,
            fidl::InterfaceRequest<fuchsia::bluetooth::le::ScanResultWatcher>
                result_watcher,
            ScanCallback callback) override;
  void Connect(fuchsia::bluetooth::PeerId id,
               fuchsia::bluetooth::le::ConnectionOptions options,
               fidl::InterfaceRequest<::fuchsia::bluetooth::le::Connection>
                   request) override;

  void GetPeripherals(::fidl::VectorPtr<::std::string> service_uuids,
                      GetPeripheralsCallback callback) override;
  void GetPeripheral(::std::string identifier,
                     GetPeripheralCallback callback) override;
  void StartScan(fuchsia::bluetooth::le::ScanFilterPtr filter,
                 StartScanCallback callback) override;
  void StopScan() override;
  void ConnectPeripheral(
      ::std::string identifier,
      fuchsia::bluetooth::le::ConnectionOptions connection_options,
      ::fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> client_request,
      ConnectPeripheralCallback callback) override;
  void DisconnectPeripheral(::std::string identifier,
                            DisconnectPeripheralCallback callback) override;

  void CreateConnectedIsochronousGroup(
      ::fuchsia::bluetooth::le::CentralCreateConnectedIsochronousGroupRequest
          request,
      CreateConnectedIsochronousGroupCallback callback) override;

  void SyncToPeriodicAdvertising(
      ::fuchsia::bluetooth::le::CentralSyncToPeriodicAdvertisingRequest request)
      override;

  // fuchsia::bluetooth::le::ChannelListenerRegistry overrides:
  void ListenL2cap(
      fuchsia::bluetooth::le::ChannelListenerRegistryListenL2capRequest request,
      ListenL2capCallback callback) override;

  // Called by |scan_session_| when a device is discovered.
  void OnScanResult(const bt::gap::Peer& peer);

  // Notifies the delegate that the scan state for this Central has changed.
  void NotifyScanStateChanged(bool scanning);

  // Notifies the delegate that the device with the given identifier has been
  // disconnected.
  void NotifyPeripheralDisconnected(bt::PeerId peer_id);

  void ClearScan() { scan_instance_.reset(); }

  // GATT is used to construct GattClientServers upon connection.
  bt::gatt::GATT::WeakPtr gatt_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  // Stores active GATT client FIDL servers. Only 1 client server per peer may
  // exist.
  std::unordered_map<bt::PeerId, std::unique_ptr<GattClientServer>>
      gatt_client_servers_;

  // The currently active LE discovery session. This is initialized when a
  // client requests to perform a scan.
  bool requesting_scan_deprecated_;
  std::unique_ptr<bt::gap::LowEnergyDiscoverySession> scan_session_deprecated_;

  std::unique_ptr<ScanInstance> scan_instance_;

  // This client's connection references. A client can hold a connection to
  // multiple peers. Each key is a peer identifier. Each value is
  //   a. nullptr, if a connect request to this device is currently pending.
  //   b. a valid reference if this Central is holding a connection reference to
  //   this device.
  std::unordered_map<bt::PeerId, std::unique_ptr<LowEnergyConnectionServer>>
      connections_;
  std::unordered_map<bt::PeerId,
                     std::unique_ptr<bt::gap::LowEnergyConnectionHandle>>
      connections_deprecated_;

  std::unordered_map<size_t, std::unique_ptr<PeriodicAdvertisingSyncServer>>
      periodic_advertising_sync_servers_;

  size_t next_server_id_ = 0;

  async_dispatcher_t* dispatcher_;

  std::unordered_map<bt::hci_spec::CigIdentifier,
                     std::unique_ptr<ConnectedIsochronousGroupServer>>
      cig_servers_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<LowEnergyCentralServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyCentralServer);
};

}  // namespace bthost
