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

#include "pw_bluetooth_sapphire/internal/host/gap/periodic_advertising_sync_manager.h"

using SyncId = bt::hci::SyncId;

namespace bt::gap {

PeriodicAdvertisingSyncHandle::PeriodicAdvertisingSyncHandle(
    PeriodicAdvertisingSyncHandle&& other) {
  Move(other);
}

PeriodicAdvertisingSyncHandle& PeriodicAdvertisingSyncHandle::operator=(
    PeriodicAdvertisingSyncHandle&& other) {
  Cancel();
  Move(other);
  return *this;
}

void PeriodicAdvertisingSyncHandle::Move(PeriodicAdvertisingSyncHandle& other) {
  sync_id_ = other.sync_id_;
  on_release_ = std::move(other.on_release_);
}

PeriodicAdvertisingSyncManager::PeriodicAdvertisingSyncManager(
    hci::Transport::WeakPtr transport,
    PeerCache& peer_cache,
    LowEnergyDiscoveryManager::WeakPtr discovery_manager,
    pw::async::Dispatcher& dispatcher)
    : peer_cache_(peer_cache),
      discovery_manager_(std::move(discovery_manager)),
      synchronizer_(std::move(transport)),
      heap_dispatcher_(dispatcher) {}

hci::Result<PeriodicAdvertisingSyncHandle>
PeriodicAdvertisingSyncManager::CreateSync(PeerId peer_id,
                                           uint8_t advertising_sid,
                                           SyncOptions options,
                                           Delegate& delegate) {
  Peer* peer = peer_cache_.FindById(peer_id);
  if (!peer || !peer->le()) {
    return fit::error(Error(HostError::kInvalidParameters));
  }

  for (auto& [sync_id, sync] : syncs_) {
    if (sync.peer_id == peer_id && sync.advertising_sid == advertising_sid) {
      return AddSyncRef(sync_id, sync, options, delegate);
    }
  }

  DeviceAddress address = peer->address();

  // TODO: https://fxbug.dev/42102158 - hci::PeriodicAdvertisingSynchronizer
  // requires the address to be an LE type. If the address is BR/EDR and the
  // peer is dual mode, we need to fix the type. This won't be necessary once we
  // remove the type field.
  if (address.IsBrEdr()) {
    address = {DeviceAddress::Type::kLEPublic, address.value()};
  }

  hci::Result<hci::PeriodicAdvertisingSync> sync_result =
      synchronizer_.CreateSync(address, advertising_sid, options, *this);
  if (sync_result.is_error()) {
    bt_log(DEBUG,
           "gap",
           "CreateSync error: %s",
           bt_str(sync_result.error_value()));
    return sync_result.take_error();
  }

  hci::SyncId sync_id = sync_result->id();
  Sync sync{.state = SyncState::kPending,
            .peer_id = peer_id,
            .advertising_sid = advertising_sid,
            .hci_sync = std::move(sync_result.value()),
            .options = options,
            .parameters = std::nullopt,
            .delegates = {{&delegate, 1}}};
  auto [_, inserted] = syncs_.try_emplace(sync_id, std::move(sync));
  PW_CHECK(inserted);

  PeriodicAdvertisingSyncHandle handle(
      sync_id,
      [self = weak_self_.GetWeakPtr(), sync_id, delegate_ptr = &delegate]() {
        if (self.is_alive()) {
          self->OnHandleRelease(sync_id, delegate_ptr);
        }
      });

  MaybeUpdateDiscoveryState();

  return fit::ok(std::move(handle));
}

void PeriodicAdvertisingSyncManager::TransferSync(
    hci::SyncId id,
    hci_spec::ConnectionHandle connection,
    uint16_t service_data,
    pw::Callback<void(hci::Result<>)> callback) {
  synchronizer_.TransferSync(id, connection, service_data, std::move(callback));
}

void PeriodicAdvertisingSyncManager::OnSyncEstablished(
    hci::SyncId sync_id,
    hci::PeriodicAdvertisingSynchronizer::SyncParameters parameters) {
  auto sync_iter = syncs_.find(sync_id);
  if (sync_iter == syncs_.end()) {
    return;
  }

  PW_CHECK(sync_iter->second.state == SyncState::kPending);
  sync_iter->second.state = SyncState::kEstablished;
  MaybeUpdateDiscoveryState();

  SyncParameters params{.peer_id = sync_iter->second.peer_id,
                        .advertising_sid = parameters.advertising_sid,
                        .interval = parameters.interval,
                        .phy = parameters.phy,
                        .subevents_count = parameters.subevents_count};
  sync_iter->second.parameters = params;

  for (auto [delegate, _] : sync_iter->second.delegates) {
    delegate->OnSyncEstablished(sync_id, params);
  }
}

void PeriodicAdvertisingSyncManager::OnSyncLost(hci::SyncId sync_id,
                                                hci::Error error) {
  auto sync_node = syncs_.extract(sync_id);
  if (sync_node.empty()) {
    return;
  }

  for (auto [delegate, _] : sync_node.mapped().delegates) {
    delegate->OnSyncLost(sync_id, error);
  }

  MaybeUpdateDiscoveryState();
}

void PeriodicAdvertisingSyncManager::OnAdvertisingReport(
    hci::SyncId sync_id,
    hci::PeriodicAdvertisingSynchronizer::PeriodicAdvertisingReport&& report) {
  auto sync_iter = syncs_.find(sync_id);
  if (sync_iter == syncs_.end()) {
    return;
  }
  AdvertisingData::ParseResult result = AdvertisingData::FromBytes(report.data);
  if (result.is_error()) {
    bt_log(WARN,
           "hci",
           "failed to parse periodic advertising data: %s",
           AdvertisingData::ParseErrorToString(result.error_value()).c_str());
    return;
  }
  PeriodicAdvertisingReport report_out;
  report_out.data = std::move(result.value());
  report_out.rssi = report.rssi;
  report_out.event_counter = report.event_counter;

  for (auto [delegate, _] : sync_iter->second.delegates) {
    delegate->OnAdvertisingReport(sync_id, report_out);
  }
}

void PeriodicAdvertisingSyncManager::OnBigInfoReport(
    hci::SyncId sync_id, BroadcastIsochronousGroupInfo&& report) {
  auto sync_iter = syncs_.find(sync_id);
  if (sync_iter == syncs_.end()) {
    return;
  }

  for (auto [delegate, _] : sync_iter->second.delegates) {
    delegate->OnBigInfoReport(sync_id, report);
  }
}

hci::Result<PeriodicAdvertisingSyncHandle>
PeriodicAdvertisingSyncManager::AddSyncRef(hci::SyncId sync_id,
                                           Sync& sync,
                                           SyncOptions options,
                                           Delegate& delegate) {
  // Insert the delegate and increase its ref count.
  ++sync.delegates[&delegate];

  if (options.filter_duplicates != sync.options.filter_duplicates) {
    // TODO: https://fxbug.dev/309014342 - Maybe restart sync if SyncOptions
    // conflict.
    bt_log(INFO,
           "hci",
           "requested periodic advertising SyncOptions conflict with existing "
           "sync");
  }

  PeriodicAdvertisingSyncHandle handle(
      sync_id,
      [self = weak_self_.GetWeakPtr(), sync_id, delegate_ptr = &delegate]() {
        if (self.is_alive()) {
          self->OnHandleRelease(sync_id, delegate_ptr);
        }
      });

  // Post OnSyncEstablished if sync is already established.
  if (sync.state == SyncState::kEstablished) {
    pw::Status post_status = heap_dispatcher_.Post(
        [self = weak_self_.GetWeakPtr(), sync_id, delegate_ptr = &delegate](
            pw::async::Context, pw::Status status) {
          if (!status.ok() || !self.is_alive()) {
            return;
          }
          // The sync or delegate could have been removed since task was
          // posted.
          auto sync_iter = self->syncs_.find(sync_id);
          if (sync_iter == self->syncs_.end()) {
            return;
          }
          auto& delegates = sync_iter->second.delegates;
          auto delegate_iter = delegates.find(delegate_ptr);
          if (delegate_iter == delegates.end()) {
            return;
          }
          PW_CHECK(sync_iter->second.parameters.has_value());
          delegate_ptr->OnSyncEstablished(sync_id,
                                          sync_iter->second.parameters.value());
        });
    PW_CHECK(post_status.ok());
  }

  return fit::ok(std::move(handle));
}

void PeriodicAdvertisingSyncManager::OnHandleRelease(hci::SyncId sync_id,
                                                     Delegate* delegate) {
  auto sync_iter = syncs_.find(sync_id);
  if (sync_iter == syncs_.end()) {
    return;
  }
  auto& delegates = sync_iter->second.delegates;
  auto delegate_iter = delegates.find(delegate);
  if (delegate_iter == delegates.end()) {
    return;
  }

  if (--delegate_iter->second == 0) {
    delegates.erase(delegate_iter);
    if (delegates.empty()) {
      syncs_.erase(sync_iter);
      MaybeUpdateDiscoveryState();
    }
  }
}

void PeriodicAdvertisingSyncManager::MaybeUpdateDiscoveryState() {
  auto pending_iter = std::find_if(syncs_.begin(), syncs_.end(), [](auto& s) {
    return s.second.state == SyncState::kPending;
  });

  if (pending_iter == syncs_.end()) {
    discovery_session_ = std::monostate();
    return;
  }

  if (!std::holds_alternative<std::monostate>(discovery_session_)) {
    return;
  }

  // The scanning filter policy is ignored by the periodic sync establishment
  // filter policy (Core Spec v6.1, Vol 6, Part B, Sec 4.3.5). So, we use an
  // arbitrary strict filter to prevent advertising reports from being delivered
  // to the host when controller filtering is enabled.
  hci::DiscoveryFilter filter;
  filter.set_name_substring("periodic_adv_filter");

  discovery_session_ = StartingDiscovery();
  discovery_manager_->StartDiscovery(
      /*active=*/false,
      /*filters=*/{std::move(filter)},
      [self = weak_self_.GetWeakPtr()](
          std::unique_ptr<LowEnergyDiscoverySession> session) {
        if (!self.is_alive()) {
          return;
        }
        self->discovery_session_ = std::move(session);
        self->MaybeUpdateDiscoveryState();
      });
}

}  // namespace bt::gap
