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

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_discovery_manager.h"

#include <lib/fit/defer.h>
#include <lib/stdcompat/functional.h>
#include <pw_assert/check.h>
#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bluetooth/hci_events.emb.h>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/supplement_data.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::gap {

namespace {

// Make an existing peer connectable, or add a connectable peer if one does not
// already exist.
Peer* AddOrUpdateConnectablePeer(PeerCache* cache, const DeviceAddress& addr) {
  Peer* peer = cache->FindByAddress(addr);
  if (!peer) {
    peer = cache->NewPeer(addr, /*connectable=*/true);
  } else {
    peer->set_connectable(true);
  }
  PW_CHECK(peer);
  return peer;
}

std::unordered_set<Peer*> ProcessInquiryResultEvent(
    PeerCache* cache,
    const pw::bluetooth::emboss::InquiryResultWithRssiEventView& event) {
  bt_log(TRACE, "gap-bredr", "inquiry result received");
  std::unordered_set<Peer*> updated;
  auto responses = event.responses();
  for (auto response : responses) {
    DeviceAddress addr(DeviceAddress::Type::kBREDR,
                       DeviceAddressBytes(response.bd_addr()));
    Peer* peer = AddOrUpdateConnectablePeer(cache, addr);
    peer->MutBrEdr().SetInquiryData(response);
    updated.insert(peer);
  }
  return updated;
}

}  // namespace

BrEdrDiscoverySession::BrEdrDiscoverySession(
    BrEdrDiscoveryManager::WeakPtr manager)
    : manager_(std::move(manager)) {}

BrEdrDiscoverySession::~BrEdrDiscoverySession() {
  manager_->RemoveDiscoverySession(this);
}

void BrEdrDiscoverySession::NotifyDiscoveryResult(const Peer& peer) const {
  if (peer_found_callback_) {
    peer_found_callback_(peer);
  }
}

void BrEdrDiscoverySession::NotifyError() const {
  if (error_callback_) {
    error_callback_();
  }
}

BrEdrDiscoverableSession::BrEdrDiscoverableSession(
    BrEdrDiscoveryManager::WeakPtr manager)
    : manager_(std::move(manager)) {}

BrEdrDiscoverableSession::~BrEdrDiscoverableSession() {
  manager_->RemoveDiscoverableSession(this);
}

BrEdrDiscoveryManager::BrEdrDiscoveryManager(
    pw::async::Dispatcher& pw_dispatcher,
    hci::CommandChannel::WeakPtr cmd,
    pw::bluetooth::emboss::InquiryMode mode,
    PeerCache* peer_cache)
    : cmd_(std::move(cmd)),
      dispatcher_(pw_dispatcher),
      cache_(peer_cache),
      result_handler_id_(0u),
      desired_inquiry_mode_(mode),
      current_inquiry_mode_(pw::bluetooth::emboss::InquiryMode::STANDARD),
      weak_self_(this) {
  PW_DCHECK(cache_);
  PW_DCHECK(cmd_.is_alive());

  result_handler_id_ = cmd_->AddEventHandler(
      hci_spec::kInquiryResultEventCode,
      fit::bind_member<&BrEdrDiscoveryManager::InquiryResult>(this));
  PW_DCHECK(result_handler_id_);
  rssi_handler_id_ = cmd_->AddEventHandler(
      hci_spec::kInquiryResultWithRSSIEventCode,
      cpp20::bind_front(&BrEdrDiscoveryManager::InquiryResultWithRssi, this));
  PW_DCHECK(rssi_handler_id_);
  eir_handler_id_ = cmd_->AddEventHandler(
      hci_spec::kExtendedInquiryResultEventCode,
      cpp20::bind_front(&BrEdrDiscoveryManager::ExtendedInquiryResult, this));
  PW_DCHECK(eir_handler_id_);

  // Set the Inquiry Scan Settings
  WriteInquiryScanSettings(
      kInquiryScanInterval, kInquiryScanWindow, /*interlaced=*/true);
}

BrEdrDiscoveryManager::~BrEdrDiscoveryManager() {
  cmd_->RemoveEventHandler(eir_handler_id_);
  cmd_->RemoveEventHandler(rssi_handler_id_);
  cmd_->RemoveEventHandler(result_handler_id_);
  InvalidateDiscoverySessions();
}

void BrEdrDiscoveryManager::RequestDiscovery(DiscoveryCallback callback) {
  PW_DCHECK(callback);

  bt_log(INFO, "gap-bredr", "RequestDiscovery");

  // If we're already waiting on a callback, then scanning is already starting.
  // Queue this to create a session when the scanning starts.
  if (!pending_discovery_.empty()) {
    bt_log(DEBUG, "gap-bredr", "discovery starting, add to pending");
    pending_discovery_.push(std::move(callback));
    return;
  }

  // If we're already scanning, just add a session.
  if (!discovering_.empty() || !zombie_discovering_.empty()) {
    bt_log(DEBUG, "gap-bredr", "add to active sessions");
    auto session = AddDiscoverySession();
    callback(fit::ok(), std::move(session));
    return;
  }

  pending_discovery_.push(std::move(callback));
  MaybeStartInquiry();
}

// Starts the inquiry procedure if any sessions exist or are waiting to start.
void BrEdrDiscoveryManager::MaybeStartInquiry() {
  if (pending_discovery_.empty() && discovering_.empty()) {
    bt_log(DEBUG, "gap-bredr", "no sessions, not starting inquiry");
    return;
  }

  bt_log(TRACE, "gap-bredr", "starting inquiry");

  auto self = weak_self_.GetWeakPtr();
  if (desired_inquiry_mode_ != current_inquiry_mode_) {
    auto packet = hci::CommandPacket::New<
        pw::bluetooth::emboss::WriteInquiryModeCommandWriter>(
        hci_spec::kWriteInquiryMode);
    packet.view_t().inquiry_mode().Write(desired_inquiry_mode_);
    cmd_->SendCommand(
            std::move(packet),
            [self, mode = desired_inquiry_mode_](
                auto /*unused*/, const hci::EventPacket& event) {
              if (!self.is_alive()) {
                return;
              }

              if (!HCI_IS_ERROR(
                      event, ERROR, "gap-bredr", "write inquiry mode failed")) {
                self->current_inquiry_mode_ = mode;
              }
            })
        .IgnoreError();
  }

  auto inquiry =
      hci::CommandPacket::New<pw::bluetooth::emboss::InquiryCommandWriter>(
          hci_spec::kInquiry);
  auto view = inquiry.view_t();
  view.lap().Write(pw::bluetooth::emboss::InquiryAccessCode::GIAC);
  view.inquiry_length().Write(kInquiryLengthDefault);
  view.num_responses().Write(0);

  cmd_->SendExclusiveCommand(
          std::move(inquiry),
          [self](auto, const hci::EventPacket& event) {
            if (!self.is_alive()) {
              return;
            }
            auto status = event.ToResult();
            if (bt_is_error(status, WARN, "gap-bredr", "inquiry error")) {
              // Failure of some kind, signal error to the sessions.
              self->InvalidateDiscoverySessions();

              // Fallthrough for callback to pending sessions.
            }

            // Resolve the request if the controller sent back a Command
            // Complete or Status event.
            // TODO(fxbug.dev/42062242): Make it impossible for Command Complete
            // to happen here and remove handling for it.
            if (event.event_code() == hci_spec::kCommandStatusEventCode ||
                event.event_code() == hci_spec::kCommandCompleteEventCode) {
              // Inquiry started, make sessions for our waiting callbacks.
              while (!self->pending_discovery_.empty()) {
                auto callback = std::move(self->pending_discovery_.front());
                self->pending_discovery_.pop();
                callback(
                    status,
                    (status.is_ok() ? self->AddDiscoverySession() : nullptr));
              }
              return;
            }

            PW_DCHECK(event.event_code() ==
                      hci_spec::kInquiryCompleteEventCode);
            self->zombie_discovering_.clear();

            if (bt_is_error(status, TRACE, "gap", "inquiry complete error")) {
              return;
            }

            // We've stopped scanning because we timed out.
            bt_log(TRACE, "gap-bredr", "inquiry complete, restart");
            self->MaybeStartInquiry();
          },
          hci_spec::kInquiryCompleteEventCode,
          {hci_spec::kRemoteNameRequest})
      .IgnoreError();
}

// Stops the inquiry procedure.
void BrEdrDiscoveryManager::StopInquiry() {
  PW_DCHECK(result_handler_id_);
  bt_log(TRACE, "gap-bredr", "cancelling inquiry");

  const hci::CommandPacket inq_cancel =
      hci::CommandPacket::New<pw::bluetooth::emboss::InquiryCancelCommandView>(
          hci_spec::kInquiryCancel);
  cmd_->SendCommand(std::move(inq_cancel),
                    [](int64_t, const hci::EventPacket& event) {
                      // Warn if the command failed.
                      HCI_IS_ERROR(
                          event, WARN, "gap-bredr", "inquiry cancel failed");
                    })
      .IgnoreError();
}

hci::CommandChannel::EventCallbackResult BrEdrDiscoveryManager::InquiryResult(
    const hci::EventPacket& event) {
  PW_DCHECK(event.event_code() == hci_spec::kInquiryResultEventCode);
  std::unordered_set<Peer*> peers;

  auto view = event.view<pw::bluetooth::emboss::InquiryResultEventView>();
  for (int i = 0; i < view.num_responses().Read(); i++) {
    const auto response = view.responses()[i];
    DeviceAddress addr(DeviceAddress::Type::kBREDR,
                       DeviceAddressBytes{response.bd_addr()});
    Peer* peer = AddOrUpdateConnectablePeer(cache_, addr);
    peer->MutBrEdr().SetInquiryData(response);
    peers.insert(peer);
  }

  NotifyPeersUpdated(peers);

  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrDiscoveryManager::InquiryResultWithRssi(const hci::EventPacket& event) {
  std::unordered_set<Peer*> peers = ProcessInquiryResultEvent(
      cache_,
      event.view<pw::bluetooth::emboss::InquiryResultWithRssiEventView>());
  NotifyPeersUpdated(peers);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrDiscoveryManager::ExtendedInquiryResult(const hci::EventPacket& event) {
  bt_log(TRACE, "gap-bredr", "ExtendedInquiryResult received");
  const auto result =
      event.view<pw::bluetooth::emboss::ExtendedInquiryResultEventView>();

  DeviceAddress addr(DeviceAddress::Type::kBREDR,
                     DeviceAddressBytes(result.bd_addr()));
  Peer* peer = AddOrUpdateConnectablePeer(cache_, addr);
  peer->MutBrEdr().SetInquiryData(result);

  NotifyPeersUpdated({peer});
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

void BrEdrDiscoveryManager::UpdateEIRResponseData(
    std::string name, hci::ResultFunction<> callback) {
  DataType name_type = DataType::kCompleteLocalName;
  size_t name_size = name.size();
  if (name.size() >= (hci_spec::kExtendedInquiryResponseMaxNameBytes)) {
    name_type = DataType::kShortenedLocalName;
    name_size = hci_spec::kExtendedInquiryResponseMaxNameBytes;
  }
  auto self = weak_self_.GetWeakPtr();

  auto write_eir = hci::CommandPacket::New<
      pw::bluetooth::emboss::WriteExtendedInquiryResponseCommandWriter>(
      hci_spec::kWriteExtendedInquiryResponse);
  auto write_eir_params = write_eir.view_t();
  write_eir_params.fec_required().Write(0x00);

  // Create MutableBufferView of BackingStorage
  unsigned char* eir_data =
      write_eir_params.extended_inquiry_response().BackingStorage().data();
  MutableBufferView eir_response_buf =
      MutableBufferView(eir_data, hci_spec::kExtendedInquiryResponseBytes);
  eir_response_buf.Fill(0);
  eir_response_buf[0] = name_size + 1;
  eir_response_buf[1] = static_cast<uint8_t>(name_type);
  eir_response_buf.mutable_view(2).Write(
      reinterpret_cast<const uint8_t*>(name.data()), name_size);

  self->cmd_
      ->SendCommand(
          std::move(write_eir),
          [self, local_name = std::move(name), cb = std::move(callback)](
              auto, const hci::EventPacket& event) mutable {
            if (!HCI_IS_ERROR(event, WARN, "gap", "write EIR failed")) {
              self->local_name_ = std::move(local_name);
            }
            cb(event.ToResult());
          })
      .IgnoreError();
}

void BrEdrDiscoveryManager::UpdateLocalName(std::string name,
                                            hci::ResultFunction<> callback) {
  auto self = weak_self_.GetWeakPtr();

  auto write_name = hci::CommandPacket::New<
      pw::bluetooth::emboss::WriteLocalNameCommandWriter>(
      hci_spec::kWriteLocalName);
  auto write_name_view = write_name.view_t();
  auto local_name = write_name_view.local_name().BackingStorage();
  size_t name_size = std::min(name.size(), hci_spec::kMaxNameLength);

  // Use ContiguousBuffer instead of constructing LocalName view in case of
  // invalid view being created when name is not large enough for the view
  auto name_buf = emboss::support::ReadOnlyContiguousBuffer(&name);
  local_name.CopyFrom(name_buf, name_size);

  cmd_->SendCommand(
          std::move(write_name),
          [self, name_as_str = std::move(name), cb = std::move(callback)](
              auto, const hci::EventPacket& event) mutable {
            if (HCI_IS_ERROR(event, WARN, "gap", "set local name failed")) {
              cb(event.ToResult());
              return;
            }
            // If the WriteLocalName command was successful, update the extended
            // inquiry data.
            self->UpdateEIRResponseData(std::move(name_as_str), std::move(cb));
          })
      .IgnoreError();
}

void BrEdrDiscoveryManager::AttachInspect(inspect::Node& parent,
                                          std::string name) {
  auto node = parent.CreateChild(name);
  inspect_properties_.Initialize(std::move(node));
  UpdateInspectProperties();
}

void BrEdrDiscoveryManager::InspectProperties::Initialize(
    inspect::Node new_node) {
  discoverable_sessions = new_node.CreateUint("discoverable_sessions", 0);
  pending_discoverable_sessions =
      new_node.CreateUint("pending_discoverable", 0);
  discoverable_sessions_count =
      new_node.CreateUint("discoverable_sessions_count", 0);
  last_discoverable_length_sec =
      new_node.CreateUint("last_discoverable_length_sec", 0);

  discovery_sessions = new_node.CreateUint("discovery_sessions", 0);
  last_discovery_length_sec =
      new_node.CreateUint("last_discovery_length_sec", 0);
  discovery_sessions_count = new_node.CreateUint("discovery_sessions_count", 0);

  discoverable_started_time.reset();
  inquiry_started_time.reset();

  node = std::move(new_node);
}

void BrEdrDiscoveryManager::InspectProperties::Update(
    size_t discoverable_count,
    size_t pending_discoverable_count,
    size_t discovery_count,
    pw::chrono::SystemClock::time_point now) {
  if (!node) {
    return;
  }

  if (!discoverable_started_time.has_value() && discoverable_count != 0) {
    discoverable_started_time.emplace(now);
  } else if (discoverable_started_time.has_value() && discoverable_count == 0) {
    discoverable_sessions_count.Add(1);
    pw::chrono::SystemClock::duration length =
        now - discoverable_started_time.value();
    last_discoverable_length_sec.Set(
        std::chrono::duration_cast<std::chrono::seconds>(length).count());
    discoverable_started_time.reset();
  }

  if (!inquiry_started_time.has_value() && discovery_count != 0) {
    inquiry_started_time.emplace(now);
  } else if (inquiry_started_time.has_value() && discovery_count == 0) {
    discovery_sessions_count.Add(1);
    pw::chrono::SystemClock::duration length =
        now - inquiry_started_time.value();
    last_discovery_length_sec.Set(
        std::chrono::duration_cast<std::chrono::seconds>(length).count());
    inquiry_started_time.reset();
  }

  discoverable_sessions.Set(discoverable_count);
  pending_discoverable_sessions.Set(pending_discoverable_count);
  discovery_sessions.Set(discovery_count);
}

void BrEdrDiscoveryManager::UpdateInspectProperties() {
  inspect_properties_.Update(discoverable_.size(),
                             pending_discoverable_.size(),
                             discovering_.size(),
                             dispatcher_.now());
}

void BrEdrDiscoveryManager::NotifyPeersUpdated(
    const std::unordered_set<Peer*>& peers) {
  for (Peer* peer : peers) {
    if (!peer->name()) {
      RequestPeerName(peer->identifier());
    }
    for (const auto& session : discovering_) {
      session->NotifyDiscoveryResult(*peer);
    }
  }
}

void BrEdrDiscoveryManager::RequestPeerName(PeerId id) {
  if (requesting_names_.count(id)) {
    bt_log(TRACE, "gap-bredr", "already requesting name for %s", bt_str(id));
    return;
  }
  if (requesting_names_.size() >= kMaxPendingNameRequests) {
    bt_log(DEBUG,
           "gap-bredr",
           "name-request backlog full (%zu); dropping request for %s",
           requesting_names_.size(),
           bt_str(id));
    return;
  }
  Peer* peer = cache_->FindById(id);
  if (!peer) {
    bt_log(
        WARN, "gap-bredr", "cannot request name, unknown peer: %s", bt_str(id));
    return;
  }
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::RemoteNameRequestCommandWriter>(
      hci_spec::kRemoteNameRequest);
  auto params = packet.view_t();
  PW_DCHECK(peer->bredr());
  PW_DCHECK(peer->bredr()->page_scan_repetition_mode());
  params.bd_addr().CopyFrom(peer->address().value().view());
  params.page_scan_repetition_mode().Write(
      *(peer->bredr()->page_scan_repetition_mode()));
  if (peer->bredr()->clock_offset()) {
    params.clock_offset().valid().Write(true);
    uint16_t offset = peer->bredr()->clock_offset().value();
    params.clock_offset().clock_offset().Write(offset);
  }

  auto cb = [id, self = weak_self_.GetWeakPtr()](
                auto, const hci::EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }
    if (HCI_IS_ERROR(event, TRACE, "gap-bredr", "remote name request failed")) {
      self->requesting_names_.erase(id);
      return;
    }

    if (event.event_code() == hci_spec::kCommandStatusEventCode) {
      return;
    }

    PW_DCHECK(event.event_code() ==
              hci_spec::kRemoteNameRequestCompleteEventCode);

    self->requesting_names_.erase(id);
    Peer* const cached_peer = self->cache_->FindById(id);
    if (!cached_peer) {
      return;
    }

    auto event_view =
        event.view<pw::bluetooth::emboss::RemoteNameRequestCompleteEventView>();
    emboss::support::ReadOnlyContiguousBuffer name =
        event_view.remote_name().BackingStorage();
    const unsigned char* name_end = std::find(name.begin(), name.end(), '\0');
    std::string name_string(reinterpret_cast<const char*>(name.begin()),
                            reinterpret_cast<const char*>(name_end));
    cached_peer->RegisterName(std::move(name_string),
                              Peer::NameSource::kNameDiscoveryProcedure);
  };

  auto cmd_id =
      cmd_->SendExclusiveCommand(std::move(packet),
                                 std::move(cb),
                                 hci_spec::kRemoteNameRequestCompleteEventCode,
                                 {hci_spec::kInquiry});
  if (cmd_id.ok()) {
    requesting_names_.insert(id);
  }
}

void BrEdrDiscoveryManager::RequestDiscoverable(DiscoverableCallback callback) {
  PW_DCHECK(callback);

  auto self = weak_self_.GetWeakPtr();
  auto result_cb = [self, cb = callback.share()](const hci::Result<>& result) {
    cb(result, (result.is_ok() ? self->AddDiscoverableSession() : nullptr));
  };

  auto update_inspect =
      fit::defer([self]() { self->UpdateInspectProperties(); });

  if (!pending_discoverable_.empty()) {
    pending_discoverable_.push(std::move(result_cb));
    bt_log(INFO,
           "gap-bredr",
           "discoverable mode starting: %zu pending",
           pending_discoverable_.size());
    return;
  }

  // If we're already discoverable, just add a session.
  if (!discoverable_.empty()) {
    result_cb(fit::ok());
    return;
  }

  pending_discoverable_.push(std::move(result_cb));
  SetInquiryScan();
}

void BrEdrDiscoveryManager::SetInquiryScan() {
  bool enable = !discoverable_.empty() || !pending_discoverable_.empty();
  bt_log(INFO,
         "gap-bredr",
         "%sabling inquiry scan: %zu sessions, %zu pending",
         (enable ? "en" : "dis"),
         discoverable_.size(),
         pending_discoverable_.size());

  auto self = weak_self_.GetWeakPtr();
  auto scan_enable_cb = [self](auto, const hci::EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }

    auto status = event.ToResult();
    auto resolve_pending = fit::defer([self, &status]() {
      while (!self->pending_discoverable_.empty()) {
        auto cb = std::move(self->pending_discoverable_.front());
        self->pending_discoverable_.pop();
        cb(status);
      }
    });

    if (bt_is_error(status, WARN, "gap-bredr", "read scan enable failed")) {
      return;
    }

    bool enabling =
        !self->discoverable_.empty() || !self->pending_discoverable_.empty();
    const auto params = event.view<
        pw::bluetooth::emboss::ReadScanEnableCommandCompleteEventView>();
    uint8_t scan_type = params.scan_enable().BackingStorage().ReadUInt();
    bool enabled =
        scan_type & static_cast<uint8_t>(hci_spec::ScanEnableBit::kInquiry);

    if (enabling == enabled) {
      bt_log(INFO,
             "gap-bredr",
             "inquiry scan already %s",
             (enabling ? "enabled" : "disabled"));
      return;
    }

    if (enabling) {
      scan_type |= static_cast<uint8_t>(hci_spec::ScanEnableBit::kInquiry);
    } else {
      scan_type &= ~static_cast<uint8_t>(hci_spec::ScanEnableBit::kInquiry);
    }

    auto write_enable = hci::CommandPacket::New<
        pw::bluetooth::emboss::WriteScanEnableCommandWriter>(
        hci_spec::kWriteScanEnable);
    auto write_enable_view = write_enable.view_t();
    write_enable_view.scan_enable().inquiry().Write(
        scan_type & static_cast<uint8_t>(hci_spec::ScanEnableBit::kInquiry));
    write_enable_view.scan_enable().page().Write(
        scan_type & static_cast<uint8_t>(hci_spec::ScanEnableBit::kPage));
    resolve_pending.cancel();
    self->cmd_
        ->SendCommand(
            std::move(write_enable),
            [self](auto, const hci::EventPacket& response) {
              if (!self.is_alive()) {
                return;
              }

              // Warn if the command failed
              HCI_IS_ERROR(
                  response, WARN, "gap-bredr", "write scan enable failed");

              while (!self->pending_discoverable_.empty()) {
                auto cb = std::move(self->pending_discoverable_.front());
                self->pending_discoverable_.pop();
                cb(response.ToResult());
              }
              self->UpdateInspectProperties();
            })
        .IgnoreError();
  };

  auto read_enable = hci::CommandPacket::New<
      pw::bluetooth::emboss::ReadScanEnableCommandWriter>(
      hci_spec::kReadScanEnable);
  cmd_->SendCommand(std::move(read_enable), std::move(scan_enable_cb))
      .IgnoreError();
}

void BrEdrDiscoveryManager::WriteInquiryScanSettings(uint16_t interval,
                                                     uint16_t window,
                                                     bool interlaced) {
  // TODO(jamuraa): add a callback for success or failure?
  auto write_activity = hci::CommandPacket::New<
      pw::bluetooth::emboss::WriteInquiryScanActivityCommandWriter>(
      hci_spec::kWriteInquiryScanActivity);
  auto activity_params = write_activity.view_t();
  activity_params.inquiry_scan_interval().Write(interval);
  activity_params.inquiry_scan_window().Write(window);

  cmd_->SendCommand(std::move(write_activity),
                    [](auto, const hci::EventPacket& event) {
                      if (HCI_IS_ERROR(event,
                                       WARN,
                                       "gap-bredr",
                                       "write inquiry scan activity failed")) {
                        return;
                      }
                      bt_log(
                          TRACE, "gap-bredr", "inquiry scan activity updated");
                    })
      .IgnoreError();

  auto write_type = hci::CommandPacket::New<
      pw::bluetooth::emboss::WriteInquiryScanTypeCommandWriter>(
      hci_spec::kWriteInquiryScanType);
  auto type_params = write_type.view_t();
  type_params.inquiry_scan_type().Write(
      interlaced ? pw::bluetooth::emboss::InquiryScanType::INTERLACED
                 : pw::bluetooth::emboss::InquiryScanType::STANDARD);

  cmd_->SendCommand(std::move(write_type),
                    [](auto, const hci::EventPacket& event) {
                      if (HCI_IS_ERROR(event,
                                       WARN,
                                       "gap-bredr",
                                       "write inquiry scan type failed")) {
                        return;
                      }
                      bt_log(TRACE, "gap-bredr", "inquiry scan type updated");
                    })
      .IgnoreError();
}

std::unique_ptr<BrEdrDiscoverySession>
BrEdrDiscoveryManager::AddDiscoverySession() {
  bt_log(TRACE, "gap-bredr", "adding discovery session");

  // Cannot use make_unique here since BrEdrDiscoverySession has a private
  // constructor.
  std::unique_ptr<BrEdrDiscoverySession> session(
      new BrEdrDiscoverySession(weak_self_.GetWeakPtr()));
  PW_DCHECK(discovering_.find(session.get()) == discovering_.end());
  discovering_.insert(session.get());
  bt_log(INFO,
         "gap-bredr",
         "new discovery session: %zu sessions active",
         discovering_.size());
  UpdateInspectProperties();
  return session;
}

void BrEdrDiscoveryManager::RemoveDiscoverySession(
    BrEdrDiscoverySession* session) {
  bt_log(TRACE, "gap-bredr", "removing discovery session");

  auto removed = discovering_.erase(session);
  // TODO(fxbug.dev/42145646): Cancel the running inquiry with StopInquiry()
  // instead.
  if (removed) {
    zombie_discovering_.insert(session);
  }
  UpdateInspectProperties();
}

std::unique_ptr<BrEdrDiscoverableSession>
BrEdrDiscoveryManager::AddDiscoverableSession() {
  bt_log(TRACE, "gap-bredr", "adding discoverable session");

  // Cannot use make_unique here since BrEdrDiscoverableSession has a private
  // constructor.
  std::unique_ptr<BrEdrDiscoverableSession> session(
      new BrEdrDiscoverableSession(weak_self_.GetWeakPtr()));
  PW_DCHECK(discoverable_.find(session.get()) == discoverable_.end());
  discoverable_.insert(session.get());
  bt_log(INFO,
         "gap-bredr",
         "new discoverable session: %zu sessions active",
         discoverable_.size());
  return session;
}

void BrEdrDiscoveryManager::RemoveDiscoverableSession(
    BrEdrDiscoverableSession* session) {
  bt_log(DEBUG, "gap-bredr", "removing discoverable session");
  discoverable_.erase(session);
  if (discoverable_.empty()) {
    SetInquiryScan();
  }
  UpdateInspectProperties();
}

void BrEdrDiscoveryManager::InvalidateDiscoverySessions() {
  for (auto session : discovering_) {
    session->NotifyError();
  }
  discovering_.clear();
  UpdateInspectProperties();
}

}  // namespace bt::gap
