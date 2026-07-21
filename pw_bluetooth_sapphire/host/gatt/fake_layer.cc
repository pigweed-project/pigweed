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

#include "pw_bluetooth_sapphire/internal/host/gatt/fake_layer.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"

namespace bt::gatt::testing {

FakeLayer::TestPeer::TestPeer(pw::async::Dispatcher& pw_dispatcher)
    : fake_client(pw_dispatcher) {}

std::pair<RemoteService::WeakPtr, FakeClient::WeakPtr>
FakeLayer::AddPeerService(PeerId peer_id,
                          const ServiceData& info,
                          bool notify) {
  auto [iter, _] = peers_.try_emplace(peer_id, pw_dispatcher_);
  auto& peer = iter->second;

  PW_CHECK(info.range_start <= info.range_end);
  auto service =
      std::make_unique<RemoteService>(info, peer.fake_client.GetWeakPtr());
  RemoteService::WeakPtr service_weak = service->GetWeakPtr();

  std::vector<att::Handle> removed;
  ServiceList added;
  ServiceList modified;

  auto svc_iter = peer.services.find(info.range_start);
  if (svc_iter != peer.services.end()) {
    if (svc_iter->second->uuid() == info.type) {
      modified.push_back(service_weak);
    } else {
      removed.push_back(svc_iter->second->handle());
      added.push_back(service_weak);
    }

    svc_iter->second->set_service_changed(true);
    peer.services.erase(svc_iter);
  } else {
    added.push_back(service_weak);
  }

  bt_log(DEBUG,
         "gatt",
         "services changed (removed: %zu, added: %zu, modified: %zu)",
         removed.size(),
         added.size(),
         modified.size());

  peer.services.emplace(info.range_start, std::move(service));

  if (notify && remote_service_watchers_.count(peer_id)) {
    remote_service_watchers_[peer_id](removed, added, modified);
  }

  return {service_weak, peer.fake_client.AsFakeWeakPtr()};
}

void FakeLayer::RemovePeerService(PeerId peer_id, att::Handle handle) {
  auto peer_iter = peers_.find(peer_id);
  if (peer_iter == peers_.end()) {
    return;
  }
  auto svc_iter = peer_iter->second.services.find(handle);
  if (svc_iter == peer_iter->second.services.end()) {
    return;
  }
  svc_iter->second->set_service_changed(true);
  peer_iter->second.services.erase(svc_iter);

  if (remote_service_watchers_.count(peer_id)) {
    remote_service_watchers_[peer_id](
        /*removed=*/{handle}, /*added=*/{}, /*modified=*/{});
  }
}

void FakeLayer::AddConnection(PeerId peer_id,
                              std::unique_ptr<Client>,
                              Server::FactoryFunction) {
  peers_.try_emplace(peer_id, pw_dispatcher_);
}

void FakeLayer::RemoveConnection(PeerId peer_id) {
  for (auto& [id, svc] : local_services_) {
    if (svc.disconnect_callback) {
      svc.disconnect_callback(id, peer_id);
    }
  }

  peers_.erase(peer_id);
}

GATT::PeerMtuListenerId FakeLayer::RegisterPeerMtuListener(PeerMtuListener) {
  PW_CRASH("implement fake behavior if needed");
}

bool FakeLayer::UnregisterPeerMtuListener(PeerMtuListenerId) {
  PW_CRASH("implement fake behavior if needed");
}

void FakeLayer::RegisterService(ServicePtr service,
                                ServiceIdCallback callback,
                                ReadHandler read_handler,
                                WriteHandler write_handler,
                                ClientConfigCallback ccc_callback,
                                ClientDisconnectCallback disconnect_callback) {
  if (register_service_fails_) {
    callback(kInvalidId);
    return;
  }

  IdType id = next_local_service_id_++;
  local_services_.try_emplace(id,
                              LocalService{std::move(service),
                                           std::move(read_handler),
                                           std::move(write_handler),
                                           std::move(ccc_callback),
                                           std::move(disconnect_callback),
                                           {}});
  callback(id);
}

void FakeLayer::UnregisterService(IdType service_id) {
  local_services_.erase(service_id);
}

void FakeLayer::SendUpdate(IdType service_id,
                           IdType chrc_id,
                           PeerId peer_id,
                           ::std::vector<uint8_t> value,
                           IndicationCallback indicate_cb) {
  auto iter = local_services_.find(service_id);
  if (iter == local_services_.end()) {
    indicate_cb(fit::error(att::ErrorCode::kInvalidHandle));
    return;
  }
  iter->second.updates.push_back(
      Update{chrc_id, std::move(value), std::move(indicate_cb), peer_id});
}

void FakeLayer::UpdateConnectedPeers(IdType service_id,
                                     IdType chrc_id,
                                     ::std::vector<uint8_t> value,
                                     IndicationCallback indicate_cb) {
  auto iter = local_services_.find(service_id);
  if (iter == local_services_.end()) {
    indicate_cb(fit::error(att::ErrorCode::kInvalidHandle));
    return;
  }
  iter->second.updates.push_back(
      Update{chrc_id, std::move(value), std::move(indicate_cb), std::nullopt});
}

void FakeLayer::SetPersistServiceChangedCCCCallback(
    PersistServiceChangedCCCCallback callback) {
  if (set_persist_service_changed_ccc_cb_cb_) {
    set_persist_service_changed_ccc_cb_cb_();
  }
  persist_service_changed_ccc_cb_ = std::move(callback);
}

void FakeLayer::SetRetrieveServiceChangedCCCCallback(
    RetrieveServiceChangedCCCCallback callback) {
  if (set_retrieve_service_changed_ccc_cb_cb_) {
    set_retrieve_service_changed_ccc_cb_cb_();
  }
  retrieve_service_changed_ccc_cb_ = std::move(callback);
}

void FakeLayer::InitializeClient(PeerId peer_id,
                                 std::vector<UUID> services_to_discover) {
  std::vector<UUID> uuids = std::move(services_to_discover);
  if (initialize_client_cb_) {
    initialize_client_cb_(peer_id, uuids);
  }

  auto iter = peers_.find(peer_id);
  if (iter == peers_.end()) {
    return;
  }

  std::vector<RemoteService::WeakPtr> added;
  if (uuids.empty()) {
    for (auto& svc_pair : iter->second.services) {
      added.push_back(svc_pair.second->GetWeakPtr());
    }
  } else {
    for (auto& svc_pair : iter->second.services) {
      auto uuid_iter =
          std::find_if(uuids.begin(), uuids.end(), [&svc_pair](auto uuid) {
            return svc_pair.second->uuid() == uuid;
          });
      if (uuid_iter != uuids.end()) {
        added.push_back(svc_pair.second->GetWeakPtr());
      }
    }
  }

  if (remote_service_watchers_.count(peer_id)) {
    remote_service_watchers_[peer_id](
        /*removed=*/{}, /*added=*/added, /*modified=*/{});
  }
}

GATT::RemoteServiceWatcherId FakeLayer::RegisterRemoteServiceWatcherForPeer(
    PeerId peer_id, RemoteServiceWatcher watcher) {
  PW_CHECK(remote_service_watchers_.count(peer_id) == 0);
  remote_service_watchers_[peer_id] = std::move(watcher);
  // Use the PeerId as the watcher ID because FakeLayer only needs to support 1
  // watcher per peer.
  return peer_id.value();
}
bool FakeLayer::UnregisterRemoteServiceWatcher(
    RemoteServiceWatcherId watcher_id) {
  bool result = remote_service_watchers_.count(PeerId(watcher_id));
  remote_service_watchers_.erase(PeerId(watcher_id));
  return result;
}

void FakeLayer::ListServices(PeerId peer_id,
                             std::vector<UUID> uuids,
                             ServiceListCallback callback) {
  if (pause_list_services_) {
    return;
  }

  ServiceList services;

  auto iter = peers_.find(peer_id);
  if (iter != peers_.end()) {
    for (auto& svc_pair : iter->second.services) {
      auto pred = [&](const UUID& uuid) {
        return svc_pair.second->uuid() == uuid;
      };
      if (uuids.empty() ||
          std::find_if(uuids.begin(), uuids.end(), pred) != uuids.end()) {
        services.push_back(svc_pair.second->GetWeakPtr());
      }
    }
  }

  callback(list_services_status_, std::move(services));
}

RemoteService::WeakPtr FakeLayer::FindService(PeerId peer_id,
                                              IdType service_id) {
  auto peer_iter = peers_.find(peer_id);
  if (peer_iter == peers_.end()) {
    return RemoteService::WeakPtr();
  }
  auto svc_iter = peer_iter->second.services.find(service_id);
  if (svc_iter == peer_iter->second.services.end()) {
    return RemoteService::WeakPtr();
  }
  return svc_iter->second->GetWeakPtr();
}

void FakeLayer::SetInitializeClientCallback(InitializeClientCallback cb) {
  initialize_client_cb_ = std::move(cb);
}

void FakeLayer::set_list_services_status(att::Result<> status) {
  list_services_status_ = status;
}

void FakeLayer::SetSetPersistServiceChangedCCCCallbackCallback(
    SetPersistServiceChangedCCCCallbackCallback cb) {
  set_persist_service_changed_ccc_cb_cb_ = std::move(cb);
}

void FakeLayer::SetSetRetrieveServiceChangedCCCCallbackCallback(
    SetRetrieveServiceChangedCCCCallbackCallback cb) {
  set_retrieve_service_changed_ccc_cb_cb_ = std::move(cb);
}

void FakeLayer::CallPersistServiceChangedCCCCallback(PeerId peer_id,
                                                     bool notify,
                                                     bool indicate) {
  persist_service_changed_ccc_cb_(peer_id,
                                  {.notify = notify, .indicate = indicate});
}

std::optional<ServiceChangedCCCPersistedData>
FakeLayer::CallRetrieveServiceChangedCCCCallback(PeerId peer_id) {
  return retrieve_service_changed_ccc_cb_(peer_id);
}

}  // namespace bt::gatt::testing
