// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci/periodic_advertising_synchronizer.h"

namespace bt::gap {

// RAII handle with shared ownership of a sync. When the last handle for a sync
// is destroyed, the sync is canceled.
class PeriodicAdvertisingSyncHandle final {
 public:
  PeriodicAdvertisingSyncHandle(hci::SyncId sync_id,
                                pw::Callback<void()> on_release)
      : sync_id_(sync_id), on_release_(std::move(on_release)) {}
  PeriodicAdvertisingSyncHandle(PeriodicAdvertisingSyncHandle&& other);
  PeriodicAdvertisingSyncHandle& operator=(
      PeriodicAdvertisingSyncHandle&& other);
  ~PeriodicAdvertisingSyncHandle() { Cancel(); }

  void Cancel() {
    if (on_release_) {
      on_release_();
    }
  }

  hci::SyncId id() const { return sync_id_; }

 private:
  void Move(PeriodicAdvertisingSyncHandle& other);

  hci::SyncId sync_id_;
  pw::Callback<void()> on_release_;
};

struct PeriodicAdvertisingReport {
  AdvertisingData data;
  int8_t rssi;
  // Present when the controller supports v2 of the advertising report event.
  std::optional<uint16_t> event_counter;
};

// PeriodicAdvertisingSyncManager wraps a `hci::PeriodicAdvertisingSynchronizer`
// and:
// * multiplixes multiple clients synchronizing to the same periodic advertising
// train
// * starts LE scans while synchronization is pending
// * converts between PeerIds and addresses
//
// Clients implement `Delegate` to be notified of periodic advertising train
// events.
class PeriodicAdvertisingSyncManager final
    : private hci::PeriodicAdvertisingSynchronizer::Delegate {
 public:
  using SyncOptions = hci::PeriodicAdvertisingSynchronizer::SyncOptions;
  using BroadcastIsochronousGroupInfo =
      hci::PeriodicAdvertisingSynchronizer::BroadcastIsochronousGroupInfo;

  // The parameters of a newly established synchronization.
  struct SyncParameters {
    PeerId peer_id;
    uint16_t advertising_sid;
    uint16_t interval;
    pw::bluetooth::emboss::LEPhy phy;
    uint8_t subevents_count;
  };

  // Delegate implemented by clients of PeriodicAdvertisingSyncManager.
  class Delegate {
   public:
    virtual ~Delegate() = default;

    // Called when a synchronization has been successfully established.
    virtual void OnSyncEstablished(hci::SyncId id,
                                   SyncParameters parameters) = 0;

    // Called when synchronization fails or is lost due to a synchronization
    // timeout.
    virtual void OnSyncLost(hci::SyncId id, hci::Error error) = 0;

    // Called when an advertising  report for sync `id` is received.
    virtual void OnAdvertisingReport(
        hci::SyncId id, const PeriodicAdvertisingReport& report) = 0;

    // Called when a BIGInfo report for sync `id` is received.
    virtual void OnBigInfoReport(
        hci::SyncId id, const BroadcastIsochronousGroupInfo& report) = 0;
  };

  PeriodicAdvertisingSyncManager(
      hci::Transport::WeakPtr transport,
      PeerCache& peer_cache,
      LowEnergyDiscoveryManager::WeakPtr discovery_manager,
      pw::async::Dispatcher& dispatcher);

  // Establish synchronization to a periodic advertising train transmitted by
  // `peer` with the SID `advertising_sid`. Synchronous errors will be returned
  // immediately, and asynchronous errors will be delivered via
  // `Delegate::OnSyncLost`. Upon successful synchronization,
  // `Delegate::OnSyncEstablished` will be called.
  hci::Result<PeriodicAdvertisingSyncHandle> CreateSync(PeerId peer,
                                                        uint8_t advertising_sid,
                                                        SyncOptions options,
                                                        Delegate& delegate);

  // Transfers a periodic advertising synchronization to a connected peer (PAST
  // Sender).
  //
  // @param id The ID of the synchronization to transfer.
  // @param connection The connection to transfer the synchronization over.
  // @param service_data Application-specific data to transfer with the sync.
  // @param callback The callback to be invoked with the result of the
  // transfer.
  void TransferSync(hci::SyncId id,
                    hci_spec::ConnectionHandle connection,
                    uint16_t service_data,
                    pw::Callback<void(hci::Result<>)> callback);

 private:
  enum class SyncState : uint8_t { kPending, kEstablished };

  struct Sync {
    SyncState state;
    PeerId peer_id;
    uint8_t advertising_sid;
    hci::PeriodicAdvertisingSync hci_sync;
    SyncOptions options;
    std::optional<SyncParameters> parameters;
    // The mapped value is the number of clients using the delegate (usually 1).
    std::unordered_map<Delegate*, uint16_t> delegates;
  };

  // Empty variant state meaning discovery has been requested but has not yet
  // started.
  struct StartingDiscovery {};

  // PeriodicAdvertisingSynchronizer overrides:
  void OnSyncEstablished(
      hci::SyncId id,
      hci::PeriodicAdvertisingSynchronizer::SyncParameters parameters) override;
  void OnSyncLost(hci::SyncId id, hci::Error error) override;
  void OnAdvertisingReport(
      hci::SyncId id,
      hci::PeriodicAdvertisingSynchronizer::PeriodicAdvertisingReport&& report)
      override;
  void OnBigInfoReport(hci::SyncId id,
                       BroadcastIsochronousGroupInfo&& report) override;

  // Creates a handle for an existing `sync`.
  hci::Result<PeriodicAdvertisingSyncHandle> AddSyncRef(hci::SyncId sync_id,
                                                        Sync& sync,
                                                        SyncOptions options,
                                                        Delegate& delegate);

  // Called when a PeriodicAdvertisingSyncHandle is destroyed.
  void OnHandleRelease(hci::SyncId sync_id, Delegate* delegate);

  // Starts scanning if there are any pending syncs, otherwise stops
  // scanning.
  void MaybeUpdateDiscoveryState();

  PeerCache& peer_cache_;

  LowEnergyDiscoveryManager::WeakPtr discovery_manager_;

  std::variant<std::monostate,
               StartingDiscovery,
               std::unique_ptr<LowEnergyDiscoverySession>>
      discovery_session_;

  hci::PeriodicAdvertisingSynchronizer synchronizer_;
  std::unordered_map<hci::SyncId, Sync> syncs_;

  pw::async::HeapDispatcher heap_dispatcher_;

  WeakSelf<PeriodicAdvertisingSyncManager> weak_self_{this};
};

}  // namespace bt::gap
