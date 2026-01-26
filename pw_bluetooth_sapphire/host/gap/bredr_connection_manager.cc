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

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>
#include <pw_string/string_builder.h>

#include "pw_bluetooth_sapphire/internal/host/common/expiring_set.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspectable.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_request.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_interrogator.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/legacy_pairing_state.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/sequential_command_runner.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/autosniff.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

namespace bt::gap {

using ConnectionState = Peer::ConnectionState;

namespace {

const char* const kInspectRequestsNodeName = "connection_requests";
const char* const kInspectRequestNodeNamePrefix = "request_";
const char* const kInspectSecurityModeName = "security_mode";
const char* const kInspectConnectionsNodeName = "connections";
const char* const kInspectConnectionNodeNamePrefix = "connection_";
const char* const kInspectLastDisconnectedListName = "last_disconnected";
const char* const kInspectLastDisconnectedItemDurationPropertyName =
    "duration_s";
const char* const kInspectLastDisconnectedItemPeerPropertyName = "peer_id";
const char* const kInspectTimestampPropertyName = "@time";
const char* const kInspectOutgoingNodeName = "outgoing";
const char* const kInspectIncomingNodeName = "incoming";
const char* const kInspectConnectionAttemptsNodeName = "connection_attempts";
const char* const kInspectSuccessfulConnectionsNodeName =
    "successful_connections";
const char* const kInspectFailedConnectionsNodeName = "failed_connections";
const char* const kInspectInterrogationCompleteCountNodeName =
    "interrogation_complete_count";
const char* const kInspectLocalApiRequestCountNodeName =
    "disconnect_local_api_request_count";
const char* const kInspectInterrogationFailedCountNodeName =
    "disconnect_interrogation_failed_count";
const char* const kInspectPairingFailedCountNodeName =
    "disconnect_pairing_failed_count";
const char* const kInspectAclLinkErrorCountNodeName =
    "disconnect_acl_link_error_count";
const char* const kInspectPeerDisconnectionCountNodeName =
    "disconnect_peer_disconnection_count";

std::string ReasonAsString(DisconnectReason reason) {
  switch (reason) {
    case DisconnectReason::kApiRequest:
      return "ApiRequest";
    case DisconnectReason::kInterrogationFailed:
      return "InterrogationFailed";
    case DisconnectReason::kPairingFailed:
      return "PairingFailed";
    case DisconnectReason::kAclLinkError:
      return "AclLinkError";
    case DisconnectReason::kPeerDisconnection:
      return "PeerDisconnection";
    default:
      return "<Unknown Reason>";
  }
}

// This procedure can continue to operate independently of the existence of an
// BrEdrConnectionManager instance, which will begin to disable Page Scan as it
// shuts down.
void SetPageScanEnabled(bool enabled,
                        hci::Transport::WeakPtr hci,
                        hci::ResultFunction<> cb) {
  PW_DCHECK(cb);
  auto read_enable = hci::CommandPacket::New<
      pw::bluetooth::emboss::ReadScanEnableCommandWriter>(
      hci_spec::kReadScanEnable);
  auto finish_enable_cb = [enabled, hci, finish_cb = std::move(cb)](
                              auto, const hci::EventPacket& event) mutable {
    if (HCI_IS_ERROR(event, WARN, "gap-bredr", "read scan enable failed")) {
      finish_cb(event.ToResult());
      return;
    }

    const auto params = event.view<
        pw::bluetooth::emboss::ReadScanEnableCommandCompleteEventView>();
    uint8_t scan_type = params.scan_enable().BackingStorage().ReadUInt();
    if (enabled) {
      scan_type |= static_cast<uint8_t>(hci_spec::ScanEnableBit::kPage);
    } else {
      scan_type &= ~static_cast<uint8_t>(hci_spec::ScanEnableBit::kPage);
    }

    auto write_enable = hci::CommandPacket::New<
        pw::bluetooth::emboss::WriteScanEnableCommandWriter>(
        hci_spec::kWriteScanEnable);
    auto write_enable_view = write_enable.view_t();
    write_enable_view.scan_enable().inquiry().Write(
        scan_type & static_cast<uint8_t>(hci_spec::ScanEnableBit::kInquiry));
    write_enable_view.scan_enable().page().Write(
        scan_type & static_cast<uint8_t>(hci_spec::ScanEnableBit::kPage));
    hci->command_channel()
        ->SendCommand(std::move(write_enable),
                      [callback = std::move(finish_cb)](
                          auto, const hci::EventPacket& response) {
                        callback(response.ToResult());
                      })
        .IgnoreError();
  };
  hci->command_channel()
      ->SendCommand(std::move(read_enable), std::move(finish_enable_cb))
      .IgnoreError();
}

}  // namespace

hci::CommandChannel::EventHandlerId BrEdrConnectionManager::AddEventHandler(
    const hci_spec::EventCode& code, hci::CommandChannel::EventCallback cb) {
  auto self = weak_self_.GetWeakPtr();
  hci::CommandChannel::EventHandlerId event_id = 0;
  event_id = hci_->command_channel()->AddEventHandler(
      code,
      [self, emboss_event_cb = std::move(cb)](const hci::EventPacket& event) {
        if (!self.is_alive()) {
          return hci::CommandChannel::EventCallbackResult::kRemove;
        }
        return emboss_event_cb(event);
      });
  PW_DCHECK(event_id);
  event_handler_ids_.push_back(event_id);
  return event_id;
}

BrEdrConnectionManager::BrEdrConnectionManager(
    hci::Transport::WeakPtr hci,
    PeerCache* peer_cache,
    DeviceAddress local_address,
    hci::LocalAddressDelegate* low_energy_address_delegate,
    l2cap::ChannelManager* l2cap,
    bool use_interlaced_scan,
    bool local_secure_connections_supported,
    bool legacy_pairing_enabled,
    bool controller_remote_public_key_validation_supported,
    sm::BrEdrSecurityManagerFactory security_manager_factory,
    pw::async::Dispatcher& dispatcher)
    : hci_(std::move(hci)),
      cache_(peer_cache),
      local_address_(local_address),
      l2cap_(l2cap),
      deny_incoming_(dispatcher),
      page_scan_interval_(0),
      page_scan_window_(0),
      use_interlaced_scan_(use_interlaced_scan),
      local_secure_connections_supported_(local_secure_connections_supported),
      legacy_pairing_enabled_(legacy_pairing_enabled),
      controller_remote_public_key_validation_supported_(
          controller_remote_public_key_validation_supported),
      security_manager_factory_(std::move(security_manager_factory)),
      low_energy_address_delegate_(low_energy_address_delegate),
      dispatcher_(dispatcher),
      weak_self_(this) {
  PW_DCHECK(hci_.is_alive());
  PW_DCHECK(cache_);
  PW_DCHECK(l2cap_);

  hci_cmd_runner_ = std::make_unique<hci::SequentialCommandRunner>(
      hci_->command_channel()->AsWeakPtr());

  // Register event handlers
  AddEventHandler(
      hci_spec::kAuthenticationCompleteEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnAuthenticationComplete>(
          this));
  AddEventHandler(
      hci_spec::kConnectionCompleteEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnConnectionComplete>(this));
  AddEventHandler(
      hci_spec::kConnectionRequestEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnConnectionRequest>(this));
  AddEventHandler(
      hci_spec::kIOCapabilityRequestEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnIoCapabilityRequest>(this));
  AddEventHandler(
      hci_spec::kIOCapabilityResponseEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnIoCapabilityResponse>(this));
  AddEventHandler(
      hci_spec::kLinkKeyRequestEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnLinkKeyRequest>(this));
  AddEventHandler(
      hci_spec::kLinkKeyNotificationEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnLinkKeyNotification>(this));
  AddEventHandler(
      hci_spec::kSimplePairingCompleteEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnSimplePairingComplete>(this));
  AddEventHandler(
      hci_spec::kUserConfirmationRequestEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnUserConfirmationRequest>(
          this));
  AddEventHandler(
      hci_spec::kUserPasskeyRequestEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnUserPasskeyRequest>(this));
  AddEventHandler(
      hci_spec::kUserPasskeyNotificationEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnUserPasskeyNotification>(
          this));
  AddEventHandler(
      hci_spec::kRoleChangeEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnRoleChange>(this));
  AddEventHandler(
      hci_spec::kPinCodeRequestEventCode,
      fit::bind_member<&BrEdrConnectionManager::OnPinCodeRequest>(this));

  // Set the timeout for outbound connections explicitly to the spec default.
  WritePageTimeout(
      hci_spec::kDefaultPageTimeoutDuration, [](const hci::Result<> status) {
        [[maybe_unused]] bool _ =
            bt_is_error(status, WARN, "gap-bredr", "write page timeout failed");
      });

  // Set variable PIN type for legacy pairing
  WritePinType(pw::bluetooth::emboss::PinType::VARIABLE);
}

BrEdrConnectionManager::~BrEdrConnectionManager() {
  // Disconnect any connections that we're holding.
  connections_.clear();

  if (!hci_.is_alive() || !hci_->command_channel()) {
    return;
  }

  // Cancel the outstanding HCI_Connection_Request if not already cancelled
  if (pending_request_ && pending_request_->Cancel()) {
    SendCreateConnectionCancelCommand(pending_request_->peer_address());
  }

  // Become unconnectable
  SetPageScanEnabled(/*enabled=*/false, hci_, [](const auto) {});

  // Remove all event handlers
  for (auto handler_id : event_handler_ids_) {
    hci_->command_channel()->RemoveEventHandler(handler_id);
  }
}

void BrEdrConnectionManager::SetConnectable(bool connectable,
                                            hci::ResultFunction<> status_cb) {
  auto self = weak_self_.GetWeakPtr();
  if (!connectable) {
    auto not_connectable_cb = [self,
                               cb = std::move(status_cb)](const auto& status) {
      if (self.is_alive()) {
        self->page_scan_interval_ = 0;
        self->page_scan_window_ = 0;
      } else if (status.is_ok()) {
        cb(ToResult(HostError::kFailed));
        return;
      }
      cb(status);
    };
    SetPageScanEnabled(/*enabled=*/false, hci_, std::move(not_connectable_cb));
    return;
  }

  WritePageScanSettings(
      hci_spec::kPageScanR1Interval,
      hci_spec::kPageScanR1Window,
      use_interlaced_scan_,
      [self, cb = std::move(status_cb)](const auto& status) mutable {
        if (bt_is_error(
                status, WARN, "gap-bredr", "Write Page Scan Settings failed")) {
          cb(status);
          return;
        }
        if (!self.is_alive()) {
          cb(ToResult(HostError::kFailed));
          return;
        }
        SetPageScanEnabled(/*enabled=*/true, self->hci_, std::move(cb));
      });
}

void BrEdrConnectionManager::SetPairingDelegate(
    PairingDelegate::WeakPtr delegate) {
  pairing_delegate_ = std::move(delegate);
  for (auto& [handle, connection] : connections_) {
    connection.pairing_state_manager().SetPairingDelegate(pairing_delegate_);
  }
}

PeerId BrEdrConnectionManager::GetPeerId(
    hci_spec::ConnectionHandle handle) const {
  auto it = connections_.find(handle);
  if (it == connections_.end()) {
    return kInvalidPeerId;
  }

  auto* peer = cache_->FindByAddress(it->second.link().peer_address());
  PW_DCHECK(peer, "Couldn't find peer for handle %#.4x", handle);
  return peer->identifier();
}

void BrEdrConnectionManager::Pair(PeerId peer_id,
                                  BrEdrSecurityRequirements security,
                                  hci::ResultFunction<> callback) {
  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "can't pair to peer_id %s: connection not found",
           bt_str(peer_id));
    callback(ToResult(HostError::kNotFound));
    return;
  }

  auto& [handle, connection] = *conn_pair;
  std::optional<std::unique_ptr<l2cap::AutosniffSuppressInterface>>
      autosniff_suppression =
          l2cap_->SuppressAutosniff(handle, "during pairing request");
  auto pairing_callback = [pair_callback = std::move(callback),
                           suppression = std::move(autosniff_suppression)](
                              auto, hci::Result<> status) {
    pair_callback(status);
  };
  connection->pairing_state_manager().InitiatePairing(
      security, std::move(pairing_callback));
}

void BrEdrConnectionManager::OpenL2capChannel(
    PeerId peer_id,
    l2cap::Psm psm,
    BrEdrSecurityRequirements security_reqs,
    l2cap::ChannelParameters params,
    l2cap::ChannelCallback cb) {
  auto pairing_cb = [self = weak_self_.GetWeakPtr(),
                     peer_id,
                     psm,
                     params,
                     callback = std::move(cb)](auto status) mutable {
    bt_log(TRACE,
           "gap-bredr",
           "got pairing status %s, %sreturning socket to %s",
           bt_str(status),
           status.is_ok() ? "" : "not ",
           bt_str(peer_id));
    if (status.is_error() || !self.is_alive()) {
      // Report the failure to the user with a null channel.
      callback(l2cap::Channel::WeakPtr());
      return;
    }

    auto conn_pair = self->FindConnectionById(peer_id);
    if (!conn_pair) {
      bt_log(INFO,
             "gap-bredr",
             "can't open l2cap channel: connection not found (peer: %s)",
             bt_str(peer_id));
      callback(l2cap::Channel::WeakPtr());
      return;
    }
    auto& [handle, connection] = *conn_pair;

    connection->OpenL2capChannel(
        psm, params, [open_cb = std::move(callback)](auto chan) {
          open_cb(std::move(chan));
        });
  };

  Pair(peer_id, security_reqs, std::move(pairing_cb));
}

BrEdrConnectionManager::SearchId BrEdrConnectionManager::AddServiceSearch(
    const UUID& uuid,
    std::unordered_set<sdp::AttributeId> attributes,
    BrEdrConnectionManager::SearchCallback callback) {
  auto on_service_discovered =
      [self = weak_self_.GetWeakPtr(), uuid, client_cb = std::move(callback)](
          PeerId peer_id, auto& service_attributes) {
        if (self.is_alive()) {
          Peer* const peer = self->cache_->FindById(peer_id);
          PW_CHECK(peer);
          peer->MutBrEdr().AddService(uuid);
        }
        client_cb(peer_id, service_attributes);
      };
  SearchId new_id = discoverer_.AddSearch(
      uuid, std::move(attributes), std::move(on_service_discovered));
  for (auto& [handle, connection] : connections_) {
    auto self = weak_self_.GetWeakPtr();
    connection.OpenL2capChannel(
        l2cap::kSDP,
        l2cap::ChannelParameters(),
        [self, peer_id = connection.peer_id(), search_id = new_id](
            auto channel) {
          if (!self.is_alive()) {
            return;
          }
          if (!channel.is_alive()) {
            // Likely interrogation is not complete. Search will be done at end
            // of interrogation.
            bt_log(INFO,
                   "gap",
                   "no l2cap channel for new search (peer: %s)",
                   bt_str(peer_id));
            // Try anyway, maybe there's a channel open
            self->discoverer_.SingleSearch(search_id, peer_id, nullptr);
            return;
          }
          auto client =
              sdp::Client::Create(std::move(channel), self->dispatcher_);
          self->discoverer_.SingleSearch(search_id, peer_id, std::move(client));
        });
  }
  return new_id;
}

bool BrEdrConnectionManager::RemoveServiceSearch(SearchId id) {
  return discoverer_.RemoveSearch(id);
}

std::optional<BrEdrConnectionManager::ScoRequestHandle>
BrEdrConnectionManager::OpenScoConnection(
    PeerId peer_id,
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
        parameters,
    sco::ScoConnectionManager::OpenConnectionCallback callback) {
  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "Can't open SCO connection to unconnected peer (peer: %s)",
           bt_str(peer_id));
    callback(fit::error(HostError::kNotFound));
    return std::nullopt;
  };
  return conn_pair->second->OpenScoConnection(std::move(parameters),
                                              std::move(callback));
}

std::optional<BrEdrConnectionManager::ScoRequestHandle>
BrEdrConnectionManager::AcceptScoConnection(
    PeerId peer_id,
    std::vector<bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
        parameters,
    sco::ScoConnectionManager::AcceptConnectionCallback callback) {
  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "Can't accept SCO connection from unconnected peer (peer: %s)",
           bt_str(peer_id));
    callback(fit::error(HostError::kNotFound));
    return std::nullopt;
  };
  return conn_pair->second->AcceptScoConnection(std::move(parameters),
                                                std::move(callback));
}

bool BrEdrConnectionManager::Disconnect(PeerId peer_id,
                                        DisconnectReason reason) {
  bt_log(INFO,
         "gap-bredr",
         "Disconnect Requested (reason %hhu - %s) (peer: %s)",
         static_cast<unsigned char>(reason),
         ReasonAsString(reason).c_str(),
         bt_str(peer_id));

  // TODO(fxbug.dev/42143836) - If a disconnect request is received when we have
  // a pending connection, we should instead abort the connection, by either:
  //   * removing the request if it has not yet been processed
  //   * sending a cancel command to the controller and waiting for it to be
  //   processed
  //   * sending a cancel command, and if we already complete, then beginning a
  //   disconnect procedure
  if (connection_requests_.find(peer_id) != connection_requests_.end()) {
    bt_log(WARN,
           "gap-bredr",
           "Can't disconnect because it's being connected to (peer: %s)",
           bt_str(peer_id));
    return false;
  }

  auto conn_pair = FindConnectionById(peer_id);
  if (!conn_pair) {
    bt_log(INFO,
           "gap-bredr",
           "No need to disconnect: It is not connected (peer: %s)",
           bt_str(peer_id));
    return true;
  }

  auto [handle, connection] = *conn_pair;

  const DeviceAddress& peer_addr = connection->link().peer_address();
  if (reason == DisconnectReason::kApiRequest) {
    bt_log(DEBUG,
           "gap-bredr",
           "requested disconnect from peer, cooldown for %llds (addr: %s)",
           std::chrono::duration_cast<std::chrono::seconds>(
               kLocalDisconnectCooldownDuration)
               .count(),
           bt_str(peer_addr));
    deny_incoming_.add_until(
        peer_addr, dispatcher_.now() + kLocalDisconnectCooldownDuration);
  }

  CleanUpConnection(
      handle, std::move(connections_.extract(handle).mapped()), reason);
  return true;
}

void BrEdrConnectionManager::SetSecurityMode(BrEdrSecurityMode mode) {
  security_mode_.Set(mode);

  if (mode == BrEdrSecurityMode::SecureConnectionsOnly) {
    // Disconnecting the peer must not be done while iterating through
    // |connections_| as it removes the connection from |connections_|, hence
    // the helper vector.
    std::vector<PeerId> insufficiently_secure_peers;
    for (auto& [_, connection] : connections_) {
      if (connection.security_properties().level() !=
          sm::SecurityLevel::kSecureAuthenticated) {
        insufficiently_secure_peers.push_back(connection.peer_id());
      }
    }
    for (PeerId id : insufficiently_secure_peers) {
      bt_log(WARN,
             "gap-bredr",
             "Peer has insufficient security for Secure Connections Only mode. \
             Closing connection for peer (%s)",
             bt_str(id));
      Disconnect(id, DisconnectReason::kPairingFailed);
    }
  }
  for (auto& [_, connection] : connections_) {
    connection.set_security_mode(mode);
  }
}

void BrEdrConnectionManager::AttachInspect(inspect::Node& parent,
                                           std::string name) {
  inspect_node_ = parent.CreateChild(name);

  security_mode_.AttachInspect(inspect_node_, kInspectSecurityModeName);

  inspect_properties_.connections_node_ =
      inspect_node_.CreateChild(kInspectConnectionsNodeName);
  inspect_properties_.last_disconnected_list.AttachInspect(
      inspect_node_, kInspectLastDisconnectedListName);

  inspect_properties_.requests_node_ =
      inspect_node_.CreateChild(kInspectRequestsNodeName);
  for (auto& [_, req] : connection_requests_) {
    req.AttachInspect(inspect_properties_.requests_node_,
                      inspect_properties_.requests_node_.UniqueName(
                          kInspectRequestNodeNamePrefix));
  }

  inspect_properties_.outgoing_.node_ =
      inspect_node_.CreateChild(kInspectOutgoingNodeName);
  inspect_properties_.outgoing_.connection_attempts_.AttachInspect(
      inspect_properties_.outgoing_.node_, kInspectConnectionAttemptsNodeName);
  inspect_properties_.outgoing_.successful_connections_.AttachInspect(
      inspect_properties_.outgoing_.node_,
      kInspectSuccessfulConnectionsNodeName);
  inspect_properties_.outgoing_.failed_connections_.AttachInspect(
      inspect_properties_.outgoing_.node_, kInspectFailedConnectionsNodeName);

  inspect_properties_.incoming_.node_ =
      inspect_node_.CreateChild(kInspectIncomingNodeName);
  inspect_properties_.incoming_.connection_attempts_.AttachInspect(
      inspect_properties_.incoming_.node_, kInspectConnectionAttemptsNodeName);
  inspect_properties_.incoming_.successful_connections_.AttachInspect(
      inspect_properties_.incoming_.node_,
      kInspectSuccessfulConnectionsNodeName);
  inspect_properties_.incoming_.failed_connections_.AttachInspect(
      inspect_properties_.incoming_.node_, kInspectFailedConnectionsNodeName);

  inspect_properties_.interrogation_complete_count_.AttachInspect(
      inspect_node_, kInspectInterrogationCompleteCountNodeName);

  inspect_properties_.disconnect_local_api_request_count_.AttachInspect(
      inspect_node_, kInspectLocalApiRequestCountNodeName);
  inspect_properties_.disconnect_interrogation_failed_count_.AttachInspect(
      inspect_node_, kInspectInterrogationFailedCountNodeName);
  inspect_properties_.disconnect_pairing_failed_count_.AttachInspect(
      inspect_node_, kInspectPairingFailedCountNodeName);
  inspect_properties_.disconnect_acl_link_error_count_.AttachInspect(
      inspect_node_, kInspectAclLinkErrorCountNodeName);
  inspect_properties_.disconnect_peer_disconnection_count_.AttachInspect(
      inspect_node_, kInspectPeerDisconnectionCountNodeName);
}

void BrEdrConnectionManager::WritePageTimeout(
    pw::chrono::SystemClock::duration page_timeout, hci::ResultFunction<> cb) {
  PW_CHECK(page_timeout >= hci_spec::kMinPageTimeoutDuration);
  PW_CHECK(page_timeout <= hci_spec::kMaxPageTimeoutDuration);

  const int64_t raw_page_timeout =
      page_timeout / hci_spec::kDurationPerPageTimeoutUnit;
  PW_CHECK(raw_page_timeout >=
           static_cast<uint16_t>(pw::bluetooth::emboss::PageTimeout::MIN));
  PW_CHECK(raw_page_timeout <=
           static_cast<uint16_t>(pw::bluetooth::emboss::PageTimeout::MAX));

  auto write_page_timeout_cmd = hci::CommandPacket::New<
      pw::bluetooth::emboss::WritePageTimeoutCommandWriter>(
      hci_spec::kWritePageTimeout);
  auto params = write_page_timeout_cmd.view_t();
  params.page_timeout().Write(raw_page_timeout);

  hci_->command_channel()
      ->SendCommand(
          std::move(write_page_timeout_cmd),
          [callback = std::move(cb)](auto, const hci::EventPacket& event) {
            callback(event.ToResult());
          })
      .IgnoreError();
}

void BrEdrConnectionManager::WritePageScanSettings(uint16_t interval,
                                                   uint16_t window,
                                                   bool interlaced,
                                                   hci::ResultFunction<> cb) {
  auto self = weak_self_.GetWeakPtr();
  if (!hci_cmd_runner_->IsReady()) {
    // TODO(jamuraa): could run the three "settings" commands in parallel and
    // remove the sequence runner.
    cb(ToResult(HostError::kInProgress));
    return;
  }

  auto write_activity = hci::CommandPacket::New<
      pw::bluetooth::emboss::WritePageScanActivityCommandWriter>(
      hci_spec::kWritePageScanActivity);
  auto activity_params = write_activity.view_t();
  activity_params.page_scan_interval().Write(interval);
  activity_params.page_scan_window().Write(window);

  hci_cmd_runner_->QueueCommand(
      std::move(write_activity),
      [self, interval, window](const hci::EventPacket& event) {
        if (!self.is_alive() ||
            HCI_IS_ERROR(
                event, WARN, "gap-bredr", "write page scan activity failed")) {
          return;
        }

        self->page_scan_interval_ = interval;
        self->page_scan_window_ = window;

        bt_log(TRACE, "gap-bredr", "page scan activity updated");
      });

  const pw::bluetooth::emboss::PageScanType scan_type =
      interlaced ? pw::bluetooth::emboss::PageScanType::INTERLACED_SCAN
                 : pw::bluetooth::emboss::PageScanType::STANDARD_SCAN;

  auto write_type = hci::CommandPacket::New<
      pw::bluetooth::emboss::WritePageScanTypeCommandWriter>(
      hci_spec::kWritePageScanType);
  auto type_params = write_type.view_t();
  type_params.page_scan_type().Write(scan_type);

  hci_cmd_runner_->QueueCommand(
      std::move(write_type), [self, scan_type](const hci::EventPacket& event) {
        if (!self.is_alive() ||
            HCI_IS_ERROR(
                event, WARN, "gap-bredr", "write page scan type failed")) {
          return;
        }

        bt_log(TRACE, "gap-bredr", "page scan type updated");
        self->page_scan_type_ = scan_type;
      });

  hci_cmd_runner_->RunCommands(std::move(cb));
}

void BrEdrConnectionManager::WritePinType(
    pw::bluetooth::emboss::PinType pin_type) {
  auto write_pin_type_cmd =
      hci::CommandPacket::New<pw::bluetooth::emboss::WritePinTypeCommandWriter>(
          hci_spec::kWritePinType);
  auto params = write_pin_type_cmd.view_t();
  params.pin_type().Write(pin_type);

  hci_->command_channel()
      ->SendCommand(
          std::move(write_pin_type_cmd),
          [](auto, const hci::EventPacket& event) {
            [[maybe_unused]] bool _ = bt_is_error(
                event.ToResult(), WARN, "gap-bredr", "Write PIN Type failed");
          })
      .IgnoreError();
}

std::optional<BrEdrConnectionRequest*>
BrEdrConnectionManager::FindConnectionRequestById(PeerId peer_id) {
  auto iter = connection_requests_.find(peer_id);
  if (iter == connection_requests_.end()) {
    return std::nullopt;
  }

  return &iter->second;
}

std::optional<std::pair<hci_spec::ConnectionHandle, BrEdrConnection*>>
BrEdrConnectionManager::FindConnectionById(PeerId peer_id) {
  auto it = std::find_if(
      connections_.begin(), connections_.end(), [peer_id](const auto& c) {
        return c.second.peer_id() == peer_id;
      });

  if (it == connections_.end()) {
    return std::nullopt;
  }

  auto& [handle, conn] = *it;
  return std::pair(handle, &conn);
}

std::optional<std::pair<hci_spec::ConnectionHandle, BrEdrConnection*>>
BrEdrConnectionManager::FindConnectionByAddress(
    const DeviceAddressBytes& bd_addr) {
  auto* const peer = cache_->FindByAddress(
      DeviceAddress(DeviceAddress::Type::kBREDR, bd_addr));
  if (!peer) {
    return std::nullopt;
  }

  return FindConnectionById(peer->identifier());
}

Peer* BrEdrConnectionManager::FindOrInitPeer(DeviceAddress addr) {
  Peer* peer = cache_->FindByAddress(addr);
  if (!peer) {
    peer = cache_->NewPeer(addr, /*connectable*/ true);
  }
  return peer;
}

// Build connection state for a new connection and begin interrogation. L2CAP is
// not enabled for this link but pairing is allowed before interrogation
// completes.
void BrEdrConnectionManager::InitializeConnection(
    DeviceAddress addr,
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::ConnectionRole role) {
  auto link = std::make_unique<hci::BrEdrConnection>(
      connection_handle, local_address_, addr, role, hci_);
  Peer* const peer = FindOrInitPeer(addr);
  auto peer_id = peer->identifier();
  bt_log(DEBUG,
         "gap-bredr",
         "Beginning interrogation for peer %s",
         bt_str(peer_id));

  // We should never have more than one link to a given peer
  PW_DCHECK(!FindConnectionById(peer_id));

  // The controller has completed the HCI connection procedure, so the
  // connection request can no longer be failed by a lower layer error. Now tie
  // error reporting of the request to the lifetime of the connection state
  // object (BrEdrConnection RAII).
  auto node = connection_requests_.extract(peer_id);
  auto request = node ? std::optional(std::move(node.mapped())) : std::nullopt;

  const hci_spec::ConnectionHandle handle = link->handle();
  auto send_auth_request_cb = [this, handle]() {
    this->SendAuthenticationRequested(handle, [handle](auto status) {
      bt_is_error(status,
                  WARN,
                  "gap-bredr",
                  "authentication requested command failed for %#.4x",
                  handle);
    });
  };
  auto disconnect_cb = [this, handle, peer_id] {
    bt_log(WARN,
           "gap-bredr",
           "Error occurred during pairing (handle %#.4x)",
           handle);
    Disconnect(peer_id, DisconnectReason::kPairingFailed);
  };
  auto on_peer_disconnect_cb = [this, connection = link.get()] {
    OnPeerDisconnect(connection);
  };

  // Create the BrEdrConnection object and place into |connections_| map
  auto [conn_iter, success] = connections_.try_emplace(
      handle,
      peer->GetWeakPtr(),
      std::move(link),
      std::move(send_auth_request_cb),
      std::move(disconnect_cb),
      std::move(on_peer_disconnect_cb),
      l2cap_,
      hci_,
      std::move(request),
      low_energy_address_delegate_,
      controller_remote_public_key_validation_supported_,
      security_manager_factory_,
      dispatcher_);
  PW_CHECK(success);

  BrEdrConnection& connection = conn_iter->second;
  connection.AttachInspect(inspect_properties_.connections_node_,
                           inspect_properties_.connections_node_.UniqueName(
                               kInspectConnectionNodeNamePrefix));

  // Interrogate this peer to find out its version/capabilities.
  connection.Interrogate(
      [this, peer_weak_ptr = peer->GetWeakPtr(), handle](hci::Result<> result) {
        if (bt_is_error(result,
                        WARN,
                        "gap-bredr",
                        "interrogation failed, dropping connection (peer: %s, "
                        "handle: %#.4x)",
                        bt_str(peer_weak_ptr->identifier()),
                        handle)) {
          // If this connection was locally requested, requester(s) are notified
          // by the disconnection.
          Disconnect(peer_weak_ptr->identifier(),
                     DisconnectReason::kInterrogationFailed);
          return;
        }
        bt_log(INFO,
               "gap-bredr",
               "interrogation complete (peer: %s, handle: %#.4x)",
               bt_str(peer_weak_ptr->identifier()),
               handle);
        CompleteConnectionSetup(peer_weak_ptr, handle);
      });

  // If this was our in-flight request, close it
  if (pending_request_ && addr == pending_request_->peer_address()) {
    pending_request_.reset();
  }

  TryCreateNextConnection();
}

// Finish connection setup after a successful interrogation.
void BrEdrConnectionManager::CompleteConnectionSetup(
    Peer::WeakPtr peer, hci_spec::ConnectionHandle handle) {
  auto self = weak_self_.GetWeakPtr();
  auto peer_id = peer->identifier();

  auto connections_iter = connections_.find(handle);
  if (connections_iter == connections_.end()) {
    bt_log(WARN,
           "gap-bredr",
           "Connection to complete not found (peer: %s, handle: %#.4x)",
           bt_str(peer_id),
           handle);
    return;
  }
  BrEdrConnection& conn_state = connections_iter->second;
  if (conn_state.peer_id() != peer->identifier()) {
    bt_log(WARN,
           "gap-bredr",
           "Connection switched peers! (now to %s), ignoring interrogation "
           "result (peer: %s, "
           "handle: %#.4x)",
           bt_str(conn_state.peer_id()),
           bt_str(peer_id),
           handle);
    return;
  }

  // Now that interrogation has successfully completed, check if the peer's
  // feature bits indicate SSP support. If not, use LegacyPairingState to
  // perform pairing if legacy pairing is enabled.
  PairingStateType pairing_type = PairingStateType::kSecureSimplePairing;
  if (!peer->IsSecureSimplePairingSupported()) {
    if (!legacy_pairing_enabled_) {
      bt_log(WARN,
             "gap-bredr",
             "Peer %s does not support SSP but legacy pairing is not enabled "
             "so pairing cannot occur",
             bt_str(peer_id));
      return;
    }
    pairing_type = PairingStateType::kLegacyPairing;
  }
  conn_state.CreateOrUpdatePairingState(
      pairing_type, pairing_delegate_, security_mode());

  WeakPtr<hci::BrEdrConnection> const connection =
      conn_state.link().GetWeakPtr();

  auto error_handler =
      [self, peer_id, connection_weak_ptr = connection->GetWeakPtr(), handle] {
        if (!self.is_alive() || !connection_weak_ptr.is_alive()) {
          return;
        }
        bt_log(
            WARN,
            "gap-bredr",
            "Link error received, closing connection (peer: %s, handle: %#.4x)",
            bt_str(peer_id),
            handle);
        self->Disconnect(peer_id, DisconnectReason::kAclLinkError);
      };

  // TODO(fxbug.dev/42113313): Implement this callback as a call to
  // InitiatePairing().
  auto security_callback = [peer_id](hci_spec::ConnectionHandle conn_handle,
                                     sm::SecurityLevel,
                                     auto cb) {
    bt_log(INFO,
           "gap-bredr",
           "Ignoring security upgrade request; not implemented (peer: %s, "
           "handle: %#.4x)",
           bt_str(peer_id),
           conn_handle);
    cb(ToResult(HostError::kNotSupported));
  };

  // Register with L2CAP to handle services on the ACL signaling channel and get
  // the SMP channel.
  l2cap_->AddACLConnection(
      handle,
      connection->role(),
      error_handler,
      std::move(security_callback),
      [self, handle](l2cap::ChannelManager::BrEdrFixedChannels channels) {
        if (!self.is_alive()) {
          return;
        }
        auto conn = self->connections_.find(handle);
        if (conn == self->connections_.end()) {
          return;
        }
        conn->second.SetSecurityManagerChannel(std::move(channels.smp));
      });

  // Remove from the denylist if we successfully connect.
  deny_incoming_.remove(peer->address());

  inspect_properties_.interrogation_complete_count_.Add(1);

  if (discoverer_.search_count()) {
    l2cap_->OpenL2capChannel(
        handle,
        l2cap::kSDP,
        l2cap::ChannelParameters(),
        [self, peer_id](auto channel) {
          if (!self.is_alive()) {
            return;
          }
          if (!channel.is_alive()) {
            bt_log(ERROR,
                   "gap",
                   "failed to create l2cap channel for SDP (peer: %s)",
                   bt_str(peer_id));
            return;
          }
          auto client =
              sdp::Client::Create(std::move(channel), self->dispatcher_);
          self->discoverer_.StartServiceDiscovery(peer_id, std::move(client));
        });
  }

  conn_state.OnInterrogationComplete();
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnAuthenticationComplete(
    const hci::EventPacket& event) {
  PW_DCHECK(event.event_code() == hci_spec::kAuthenticationCompleteEventCode);
  auto params =
      event.view<pw::bluetooth::emboss::AuthenticationCompleteEventView>();
  hci_spec::ConnectionHandle connection_handle =
      params.connection_handle().Read();
  pw::bluetooth::emboss::StatusCode status = params.status().Read();

  auto iter = connections_.find(connection_handle);
  if (iter == connections_.end()) {
    bt_log(INFO,
           "gap-bredr",
           "ignoring authentication complete (status: %s) for unknown "
           "connection handle %#.04x",
           bt_str(ToResult(status)),
           connection_handle);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  iter->second.pairing_state_manager().OnAuthenticationComplete(status);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

bool BrEdrConnectionManager::ExistsIncomingRequest(PeerId id) {
  auto request = connection_requests_.find(id);
  return (request != connection_requests_.end() &&
          request->second.HasIncoming());
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnConnectionRequest(const hci::EventPacket& event) {
  auto params = event.view<pw::bluetooth::emboss::ConnectionRequestEventView>();
  const DeviceAddress addr(DeviceAddress::Type::kBREDR,
                           DeviceAddressBytes(params.bd_addr()));
  const pw::bluetooth::emboss::LinkType link_type = params.link_type().Read();
  const DeviceClass device_class(
      params.class_of_device().BackingStorage().ReadUInt());

  if (link_type != pw::bluetooth::emboss::LinkType::ACL) {
    HandleNonAclConnectionRequest(addr, link_type);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  if (deny_incoming_.contains(addr)) {
    bt_log(INFO,
           "gap-bredr",
           "rejecting incoming from peer (addr: %s) on cooldown",
           bt_str(addr));
    SendRejectConnectionRequest(
        addr,
        pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // Initialize the peer if it doesn't exist, to ensure we have allocated a
  // PeerId Do this after checking the denylist to avoid adding denied peers to
  // cache.
  auto peer = FindOrInitPeer(addr);
  auto peer_id = peer->identifier();

  // In case of concurrent incoming requests from the same peer, reject all but
  // the first
  if (ExistsIncomingRequest(peer_id)) {
    bt_log(WARN,
           "gap-bredr",
           "rejecting duplicate incoming connection request (peer: %s, addr: "
           "%s, link_type: %s, "
           "class: %s)",
           bt_str(peer_id),
           bt_str(addr),
           hci_spec::LinkTypeToString(params.link_type().Read()),
           bt_str(device_class));
    SendRejectConnectionRequest(
        addr,
        pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // If we happen to be already connected (for example, if our outgoing raced,
  // or we received duplicate requests), we reject the request with
  // 'ConnectionAlreadyExists'
  if (FindConnectionById(peer_id)) {
    bt_log(WARN,
           "gap-bredr",
           "rejecting incoming connection request; already connected (peer: "
           "%s, addr: %s)",
           bt_str(peer_id),
           bt_str(addr));
    SendRejectConnectionRequest(
        addr, pw::bluetooth::emboss::StatusCode::CONNECTION_ALREADY_EXISTS);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // Accept the connection, performing a role switch. We receive a Connection
  // Complete event when the connection is complete, and finish the link then.
  bt_log(INFO,
         "gap-bredr",
         "accepting incoming connection (peer: %s, addr: %s, link_type: %s, "
         "class: %s)",
         bt_str(peer_id),
         bt_str(addr),
         hci_spec::LinkTypeToString(link_type),
         bt_str(device_class));

  peer->MutBrEdr().SetIncomingRequest(params);

  // Register that we're in the middle of an incoming request for this peer -
  // create a new request if one doesn't already exist
  auto [request, _] = connection_requests_.try_emplace(
      peer_id,
      dispatcher_,
      addr,
      peer_id,
      peer->MutBrEdr().RegisterInitializingConnection());

  inspect_properties_.incoming_.connection_attempts_.Add(1);

  request->second.BeginIncoming();
  request->second.AttachInspect(inspect_properties_.requests_node_,
                                inspect_properties_.requests_node_.UniqueName(
                                    kInspectRequestNodeNamePrefix));

  SendAcceptConnectionRequest(
      addr.value(),
      [addr, self = weak_self_.GetWeakPtr(), peer_id](auto status) {
        if (self.is_alive() && status.is_error()) {
          self->CompleteRequest(peer_id, addr, status, /*handle=*/0);
        }
      });
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnConnectionComplete(const hci::EventPacket& event) {
  auto params =
      event.view<pw::bluetooth::emboss::ConnectionCompleteEventView>();
  if (params.link_type().Read() != pw::bluetooth::emboss::LinkType::ACL) {
    // Only ACL links are processed
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // Initialize the peer if it doesn't exist, to ensure we have allocated a
  // PeerId (we should usually have a peer by this point)
  DeviceAddress addr(DeviceAddress::Type::kBREDR,
                     DeviceAddressBytes(params.bd_addr()));
  auto peer = FindOrInitPeer(addr);

  CompleteRequest(peer->identifier(),
                  addr,
                  event.ToResult(),
                  params.connection_handle().Read());
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

// A request for a connection - from an upstream client _or_ a remote peer -
// completed, successfully or not. This may be due to a ConnectionComplete event
// being received, or due to a CommandStatus response being received in response
// to a CreateConnection command
void BrEdrConnectionManager::CompleteRequest(
    PeerId peer_id,
    DeviceAddress address,
    hci::Result<> status,
    hci_spec::ConnectionHandle handle) {
  auto req_iter = connection_requests_.find(peer_id);
  if (req_iter == connection_requests_.end()) {
    // Prevent logspam for rejected during cooldown.
    if (deny_incoming_.contains(address)) {
      return;
    }
    // This could potentially happen if the peer expired from the peer cache
    // during the connection procedure
    bt_log(INFO,
           "gap-bredr",
           "ConnectionComplete received with no known request (status: %s, "
           "peer: %s, addr: %s, "
           "handle: %#.4x)",
           bt_str(status),
           bt_str(peer_id),
           bt_str(address),
           handle);
    return;
  }
  auto& request = req_iter->second;

  bool completes_outgoing_request =
      pending_request_ && pending_request_->peer_address() == address;
  bool has_incoming_request =
      !completes_outgoing_request && request.HasIncoming();
  bool failed = status.is_error();

  const char* direction = completes_outgoing_request ? "outgoing" : "incoming";
  const char* result = status.is_ok() ? "complete" : "error";
  pw::bluetooth::emboss::ConnectionRole role =
      completes_outgoing_request
          ? pw::bluetooth::emboss::ConnectionRole::CENTRAL
          : pw::bluetooth::emboss::ConnectionRole::PERIPHERAL;
  if (request.role_change()) {
    role = request.role_change().value();
  }

  bt_log(INFO,
         "gap-bredr",
         "%s connection %s (status: %s, role: %s, peer: %s, addr: %s, handle: "
         "%#.4x)",
         direction,
         result,
         bt_str(status),
         role == pw::bluetooth::emboss::ConnectionRole::CENTRAL ? "central"
                                                                : "peripheral",
         bt_str(peer_id),
         bt_str(address),
         handle);

  if (completes_outgoing_request) {
    // Determine the modified status in case of cancellation or timeout
    status = pending_request_->CompleteRequest(status);
    pending_request_.reset();
  } else {
    // This incoming connection arrived while we're trying to make an outgoing
    // connection; not an impossible coincidence but log it in case it's
    // interesting.
    // TODO(fxbug.dev/42173957): Added to investigate timing and can be removed
    // if it adds no value
    if (pending_request_) {
      bt_log(
          INFO,
          "gap-bredr",
          "doesn't complete pending outgoing connection to peer %s (addr: %s)",
          bt_str(pending_request_->peer_id()),
          bt_str(pending_request_->peer_address()));
    }
    // If this was an incoming attempt, clear it
    request.CompleteIncoming();
  }

  if (failed) {
    if (request.HasIncoming() ||
        (!completes_outgoing_request && request.AwaitingOutgoing())) {
      // This request failed, but we're still waiting on either:
      // * an in-progress incoming request or
      // * to attempt our own outgoing request
      // Therefore we don't notify yet - instead take no action, and wait until
      // we finish those steps before completing the request and notifying
      // callbacks
      TryCreateNextConnection();
      return;
    }
    if (completes_outgoing_request && connection_requests_.size() == 1 &&
        request.ShouldRetry(status.error_value())) {
      bt_log(INFO,
             "gap-bredr",
             "no pending connection requests to other peers, so %sretrying "
             "outbound connection (peer: "
             "%s, addr: %s)",
             request.HasIncoming()
                 ? "waiting for inbound request completion before potentially "
                 : "",
             bt_str(peer_id),
             bt_str(address));
      // By not erasing |request| from |connection_requests_|, even if
      // TryCreateNextConnection does not directly retry because there's an
      // inbound request to the same peer, the retry will happen if the inbound
      // request completes unusuccessfully.
      TryCreateNextConnection();
      return;
    }
    if (completes_outgoing_request) {
      inspect_properties_.outgoing_.failed_connections_.Add(1);
    } else if (has_incoming_request) {
      inspect_properties_.incoming_.failed_connections_.Add(1);
    }
    request.NotifyCallbacks(status, [] { return nullptr; });
    connection_requests_.erase(req_iter);
  } else {
    if (completes_outgoing_request) {
      inspect_properties_.outgoing_.successful_connections_.Add(1);
    } else if (has_incoming_request) {
      inspect_properties_.incoming_.successful_connections_.Add(1);
    }
    // Callbacks will be notified when interrogation completes
    InitializeConnection(address, handle, role);
  }

  TryCreateNextConnection();
}

void BrEdrConnectionManager::OnPeerDisconnect(
    const hci::Connection* connection) {
  auto handle = connection->handle();

  auto it = connections_.find(handle);
  if (it == connections_.end()) {
    bt_log(WARN,
           "gap-bredr",
           "disconnect from unknown connection handle %#.4x",
           handle);
    return;
  }

  auto conn = std::move(it->second);
  connections_.erase(it);

  bt_log(INFO,
         "gap-bredr",
         "peer disconnected (peer: %s, handle: %#.4x)",
         bt_str(conn.peer_id()),
         handle);
  CleanUpConnection(
      handle, std::move(conn), DisconnectReason::kPeerDisconnection);
}

void BrEdrConnectionManager::CleanUpConnection(
    hci_spec::ConnectionHandle handle,
    BrEdrConnection conn,
    DisconnectReason reason) {
  l2cap_->RemoveConnection(handle);
  RecordDisconnectInspect(conn, reason);
  // |conn| is destroyed when it goes out of scope.
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnIoCapabilityRequest(const hci::EventPacket& event) {
  const auto params =
      event.view<pw::bluetooth::emboss::IoCapabilityRequestEventView>();
  const DeviceAddressBytes addr(params.bd_addr());

  auto conn_pair = FindConnectionByAddress(addr);
  if (!conn_pair) {
    bt_log(ERROR,
           "gap-bredr",
           "got %s for unconnected addr %s",
           __func__,
           bt_str(addr));
    SendIoCapabilityRequestNegativeReply(
        addr, pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  auto [handle, conn_ptr] = *conn_pair;

  // If we receive an HCI_IO_Capability_Request event before interrogation is
  // complete, there will be no pairing state object so we need to create it now
  conn_ptr->CreateOrUpdatePairingState(PairingStateType::kSecureSimplePairing,
                                       pairing_delegate_,
                                       security_mode());

  auto reply = conn_ptr->pairing_state_manager().OnIoCapabilityRequest();

  if (!reply) {
    SendIoCapabilityRequestNegativeReply(
        addr, pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  const pw::bluetooth::emboss::IoCapability io_capability = *reply;

  // TODO(fxbug.dev/42138242): Add OOB status from PeerCache.
  const pw::bluetooth::emboss::OobDataPresent oob_data_present =
      pw::bluetooth::emboss::OobDataPresent::NOT_PRESENT;

  // TODO(fxbug.dev/42075714): Determine this based on the service requirements.
  const pw::bluetooth::emboss::AuthenticationRequirements auth_requirements =
      io_capability == pw::bluetooth::emboss::IoCapability::NO_INPUT_NO_OUTPUT
          ? pw::bluetooth::emboss::AuthenticationRequirements::GENERAL_BONDING
          : pw::bluetooth::emboss::AuthenticationRequirements::
                MITM_GENERAL_BONDING;  // inclusive-language: ignore

  SendIoCapabilityRequestReply(
      addr, io_capability, oob_data_present, auth_requirements);
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnIoCapabilityResponse(const hci::EventPacket& event) {
  const auto params =
      event.view<pw::bluetooth::emboss::IoCapabilityResponseEventView>();
  const DeviceAddressBytes addr(params.bd_addr());

  auto conn_pair = FindConnectionByAddress(addr);
  if (!conn_pair) {
    bt_log(INFO,
           "gap-bredr",
           "got %s for unconnected addr %s",
           __func__,
           bt_str(addr));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // If we receive an HCI_IO_Capability_Response event before interrogation is
  // complete, there will be no pairing state object so we need to create it
  // now. If we previously created a pairing state object because there was
  // already an HCI_IO_Capability_Request event, then this method will no-op.
  conn_pair->second->CreateOrUpdatePairingState(
      PairingStateType::kSecureSimplePairing,
      pairing_delegate_,
      security_mode());

  conn_pair->second->pairing_state_manager().OnIoCapabilityResponse(
      params.io_capability().Read());
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnLinkKeyRequest(const hci::EventPacket& event) {
  const auto params =
      event.view<pw::bluetooth::emboss::LinkKeyRequestEventView>();
  const DeviceAddress addr(DeviceAddress::Type::kBREDR,
                           DeviceAddressBytes(params.bd_addr()));
  Peer* peer = cache_->FindByAddress(addr);
  if (!peer) {
    bt_log(WARN, "gap-bredr", "no peer with address %s found", bt_str(addr));
    SendLinkKeyRequestNegativeReply(addr.value());
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  PeerId peer_id = peer->identifier();

  std::optional<hci_spec::LinkKey> link_key = std::nullopt;

  std::optional<BrEdrConnectionRequest*> connection_req =
      FindConnectionRequestById(peer_id);
  if (!connection_req.has_value()) {
    // The ACL connection is complete, so either generate a new link key if this
    // is a new connection, or try to get the current link key (if it is valid)
    auto conn_pair = FindConnectionById(peer_id);
    if (!conn_pair) {
      bt_log(WARN,
             "gap-bredr",
             "Can't find current connection or connection request for peer %s",
             bt_str(peer_id));
      SendLinkKeyRequestNegativeReply(addr.value());
      return hci::CommandChannel::EventCallbackResult::kContinue;
    }
    auto& [handle, conn] = *conn_pair;

    // TODO(fxbug.dev/355466957): Pairing state type is unknown if a
    // link key request is received after ACL connection is complete but before
    // interrogation is complete. Default to using SSP for now.
    if (!conn->interrogation_complete()) {
      conn->CreateOrUpdatePairingState(PairingStateType::kSecureSimplePairing,
                                       pairing_delegate_,
                                       security_mode());
    }

    link_key = conn->pairing_state_manager().OnLinkKeyRequest();
  } else {
    // Legacy Pairing may occur before the ACL connection between two devices is
    // complete. If a link key is requested during connection setup, a
    // HCI_Link_Key_Request event may be received prior to the
    // HCI_Connection_Complete event (so no connection object will exist yet)
    // (Core Spec v5.4, Vol 2, Part F, 3.1).

    bool outgoing_connection = connection_req.value()->AwaitingOutgoing();

    // The HCI link is not yet established, so |link|, |auth_cb|, and
    // |status_cb| are not created yet. After the connection is complete, they
    // are initialized in |PairingStateManager|'s constructor.
    std::unique_ptr<LegacyPairingState> legacy_pairing_state =
        std::make_unique<LegacyPairingState>(peer->GetWeakPtr(),
                                             pairing_delegate_,
                                             outgoing_connection,
                                             &dispatcher_);

    connection_req.value()->set_legacy_pairing_state(
        std::move(legacy_pairing_state));

    link_key =
        connection_req.value()->legacy_pairing_state()->OnLinkKeyRequest();
  }

  // If there is no valid link key, we start the pairing process (exchange IO
  // capabilities for SSP, request PIN code for legacy pairing)
  if (!link_key.has_value()) {
    SendLinkKeyRequestNegativeReply(addr.value());
  } else {
    SendLinkKeyRequestReply(addr.value(), link_key.value());
  }
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnLinkKeyNotification(const hci::EventPacket& event) {
  const auto params =
      event.view<pw::bluetooth::emboss::LinkKeyNotificationEventView>();

  const DeviceAddress addr(DeviceAddress::Type::kBREDR,
                           DeviceAddressBytes(params.bd_addr()));
  pw::bluetooth::emboss::KeyType key_type = params.key_type().Read();

  auto* peer = cache_->FindByAddress(addr);
  if (!peer) {
    bt_log(WARN,
           "gap-bredr",
           "no known peer with address %s found; link key not stored (key "
           "type: %u)",
           bt_str(addr),
           static_cast<uint8_t>(key_type));
    cache_->LogBrEdrBondingEvent(false);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  if (!legacy_pairing_enabled_ &&
      key_type == pw::bluetooth::emboss::KeyType::COMBINATION) {
    bt_log(WARN,
           "gap-bredr",
           "Got %u key type in link key notification for peer %s but legacy "
           "pairing is not enabled",
           static_cast<uint8_t>(key_type),
           bt_str(peer->identifier()));
    cache_->LogBrEdrBondingEvent(false);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  bt_log(INFO,
         "gap-bredr",
         "got link key notification (key type: %u, peer: %s)",
         static_cast<uint8_t>(key_type),
         bt_str(peer->identifier()));

  sm::SecurityProperties sec_props;
  if (key_type == pw::bluetooth::emboss::KeyType::CHANGED_COMBINATION_KEY) {
    if (!peer->bredr() || !peer->bredr()->bonded()) {
      bt_log(WARN,
             "gap-bredr",
             "can't update link key of unbonded peer %s",
             bt_str(peer->identifier()));
      cache_->LogBrEdrBondingEvent(false);
      return hci::CommandChannel::EventCallbackResult::kContinue;
    }

    // Reuse current properties
    PW_DCHECK(peer->bredr()->link_key());
    sec_props = peer->bredr()->link_key()->security();
    key_type =
        static_cast<pw::bluetooth::emboss::KeyType>(sec_props.GetLinkKeyType());
  } else {
    sec_props =
        sm::SecurityProperties(static_cast<hci_spec::LinkKeyType>(key_type));
  }

  auto peer_id = peer->identifier();

  UInt128 key_value;
  ::emboss::support::ReadWriteContiguousBuffer(&key_value)
      .CopyFrom(params.link_key().value().BackingStorage(), key_value.size());

  hci_spec::LinkKey hci_key(key_value, 0, 0);
  sm::LTK key(sec_props, hci_key);

  auto handle = FindConnectionById(peer_id);
  if (!handle) {
    std::optional<BrEdrConnectionRequest*> request =
        FindConnectionRequestById(peer_id);
    if (!request) {
      bt_log(WARN,
             "gap-bredr",
             "Can't find current connection or connection request for peer %s",
             bt_str(peer_id));
    } else {
      // The connection request's legacy pairing state object must exist at this
      // point since we created it in the request's constructor.
      PW_CHECK(request.value()->legacy_pairing_state());
      request.value()->legacy_pairing_state()->OnLinkKeyNotification(
          key_value, static_cast<hci_spec::LinkKeyType>(key_type));
    }
  } else {
    handle->second->link().set_link_key(
        hci_key, static_cast<hci_spec::LinkKeyType>(key_type));
    handle->second->pairing_state_manager().OnLinkKeyNotification(
        key_value,
        static_cast<hci_spec::LinkKeyType>(key_type),
        local_secure_connections_supported_);
  }

  if (cache_->StoreBrEdrBond(addr, key)) {
    cache_->LogBrEdrBondingEvent(true);
  } else {
    cache_->LogBrEdrBondingEvent(false);
    bt_log(ERROR,
           "gap-bredr",
           "failed to cache bonding data (peer: %s)",
           bt_str(peer_id));
  }
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnSimplePairingComplete(
    const hci::EventPacket& event_packet) {
  auto params =
      event_packet
          .view<pw::bluetooth::emboss::SimplePairingCompleteEventView>();
  DeviceAddressBytes bd_addr = DeviceAddressBytes(params.bd_addr());

  auto conn_pair = FindConnectionByAddress(bd_addr);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "got Simple Pairing Complete (status: %s) for unconnected addr %s",
           bt_str(ToResult(params.status().Read())),
           bt_str(bd_addr));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  conn_pair->second->pairing_state_manager().OnSimplePairingComplete(
      params.status().Read());
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnUserConfirmationRequest(
    const hci::EventPacket& event_packet) {
  auto params =
      event_packet
          .view<pw::bluetooth::emboss::UserConfirmationRequestEventView>();
  DeviceAddressBytes bd_addr = DeviceAddressBytes(params.bd_addr());

  auto conn_pair = FindConnectionByAddress(bd_addr);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "got %s for unconnected addr %s",
           __func__,
           bt_str(bd_addr));
    SendUserConfirmationRequestNegativeReply(bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto confirm_cb = [self = weak_self_.GetWeakPtr(), bd_addr](bool confirm) {
    if (!self.is_alive()) {
      return;
    }

    if (confirm) {
      self->SendUserConfirmationRequestReply(bd_addr);
    } else {
      self->SendUserConfirmationRequestNegativeReply(bd_addr);
    }
  };
  conn_pair->second->pairing_state_manager().OnUserConfirmationRequest(
      params.numeric_value().Read(), std::move(confirm_cb));
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnUserPasskeyRequest(
    const hci::EventPacket& event_packet) {
  auto params =
      event_packet.view<pw::bluetooth::emboss::UserPasskeyRequestEventView>();
  DeviceAddressBytes bd_addr = DeviceAddressBytes(params.bd_addr());

  auto conn_pair = FindConnectionByAddress(bd_addr);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "got %s for unconnected addr %s",
           __func__,
           bt_str(bd_addr));
    SendUserPasskeyRequestNegativeReply(bd_addr);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto passkey_cb = [self = weak_self_.GetWeakPtr(),
                     bd_addr](std::optional<uint32_t> passkey) {
    if (!self.is_alive()) {
      return;
    }

    if (passkey) {
      self->SendUserPasskeyRequestReply(bd_addr, *passkey);
    } else {
      self->SendUserPasskeyRequestNegativeReply(bd_addr);
    }
  };
  conn_pair->second->pairing_state_manager().OnUserPasskeyRequest(
      std::move(passkey_cb));
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnUserPasskeyNotification(
    const hci::EventPacket& event_packet) {
  auto params =
      event_packet
          .view<pw::bluetooth::emboss::UserPasskeyNotificationEventView>();
  DeviceAddressBytes bd_addr = DeviceAddressBytes(params.bd_addr());

  auto conn_pair = FindConnectionByAddress(bd_addr);
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "got %s for unconnected addr %s",
           __func__,
           bt_str(bd_addr));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  conn_pair->second->pairing_state_manager().OnUserPasskeyNotification(
      params.passkey().Read());
  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult BrEdrConnectionManager::OnRoleChange(
    const hci::EventPacket& event) {
  const auto params = event.view<pw::bluetooth::emboss::RoleChangeEventView>();
  const DeviceAddress address(DeviceAddress::Type::kBREDR,
                              DeviceAddressBytes(params.bd_addr()));
  Peer* peer = cache_->FindByAddress(address);
  if (!peer) {
    bt_log(WARN,
           "gap-bredr",
           "got %s for unknown peer (address: %s)",
           __func__,
           bt_str(address));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }
  PeerId peer_id = peer->identifier();

  const pw::bluetooth::emboss::ConnectionRole new_role = params.role().Read();

  // When a role change is requested in the HCI_Accept_Connection_Request
  // command, a HCI_Role_Change event may be received prior to the
  // HCI_Connection_Complete event (so no connection object will exist yet)
  // (Core Spec v5.2, Vol 2, Part F, Sec 3.1).
  auto request_iter = connection_requests_.find(peer_id);
  if (request_iter != connection_requests_.end()) {
    request_iter->second.set_role_change(new_role);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto conn_pair = FindConnectionByAddress(address.value());
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "got %s for unconnected peer %s",
           __func__,
           bt_str(peer_id));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  if (HCI_IS_ERROR(event,
                   WARN,
                   "gap-bredr",
                   "role change failed and remains %s (peer: %s)",
                   hci_spec::ConnectionRoleToString(new_role).c_str(),
                   bt_str(peer_id))) {
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  bt_log(DEBUG,
         "gap-bredr",
         "role changed to %s (peer: %s)",
         hci_spec::ConnectionRoleToString(new_role).c_str(),
         bt_str(peer_id));
  conn_pair->second->link().set_role(new_role);

  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
BrEdrConnectionManager::OnPinCodeRequest(const hci::EventPacket& event) {
  const auto params =
      event.view<pw::bluetooth::emboss::PinCodeRequestEventView>();
  const DeviceAddress addr(DeviceAddress::Type::kBREDR,
                           DeviceAddressBytes(params.bd_addr()));
  Peer* peer = cache_->FindByAddress(addr);
  if (!peer) {
    bt_log(WARN, "gap-bredr", "no peer with address %s found", bt_str(addr));
    SendPinCodeRequestNegativeReply(addr.value());
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  PeerId peer_id = peer->identifier();

  auto pin_code_cb =
      [this, self = weak_self_.GetWeakPtr(), addr_as_bytes = addr.value()](
          std::optional<uint16_t> pin) {
        if (!self.is_alive()) {
          return;
        }

        if (pin) {
          // TODO(fxbug.dev/348700005): Support exponential backoff
          SendPinCodeRequestReply(addr_as_bytes, pin.value());
        } else {
          SendPinCodeRequestNegativeReply(addr_as_bytes);
        }
      };

  std::optional<BrEdrConnectionRequest*> connection_req =
      FindConnectionRequestById(peer_id);
  if (!connection_req.has_value()) {
    // The ACL connection is complete so get the PIN code
    auto conn_pair = FindConnectionByAddress(addr.value());
    if (!conn_pair.has_value()) {
      bt_log(WARN,
             "gap-bredr",
             "Can't find current connection or connection request for peer %s",
             bt_str(addr));
      SendPinCodeRequestNegativeReply(addr.value());
      return hci::CommandChannel::EventCallbackResult::kContinue;
    }
    auto [handle, conn_ptr] = *conn_pair;

    if (conn_ptr->pairing_state_manager().secure_simple_pairing_state()) {
      // TODO(fxbug.dev/355466957): If a SecureSimplePairingState object
      // already exists for this connection, we have the following edge case: we
      // received an HCI_Link_Key_Request event after ACL connection was
      // complete but before interrogation was complete. We temporarily default
      // to SSP which causes this error when it is actually legacy pairing.

      SendPinCodeRequestNegativeReply(addr.value());
      return hci::CommandChannel::EventCallbackResult::kContinue;
    }

    // If we receive an HCI_PIN_Code_Request event before interrogation is
    // complete, there will be no pairing state object so we need to create it
    // now
    conn_ptr->CreateOrUpdatePairingState(
        PairingStateType::kLegacyPairing, pairing_delegate_, security_mode());

    conn_ptr->pairing_state_manager().OnPinCodeRequest(std::move(pin_code_cb));
  } else {
    // Legacy Pairing may occur before the ACL connection between two devices is
    // complete. If a PIN code is requested during connection setup, a
    // HCI_PIN_Code_Request event may be received prior to the
    // HCI_Connection_Complete event (so no connection object will exist yet)
    // (Core Spec v5.4, Vol 2, Part F, Sec 3.1).

    // If we already created a LegacyPairingState object in |OnLinkKeyRequest|,
    // don't recreate another one.
    if (!connection_req.value()->legacy_pairing_state()) {
      bool outgoing_connection = connection_req.value()->AwaitingOutgoing();

      // The HCI link is not yet established, so |link|, |auth_cb|, and
      // |status_cb| are not created yet. After the connection is complete, they
      // are initialized in |PairingStateManager|'s constructor.
      std::unique_ptr<LegacyPairingState> legacy_pairing_state =
          std::make_unique<LegacyPairingState>(peer->GetWeakPtr(),
                                               pairing_delegate_,
                                               outgoing_connection,
                                               &dispatcher_);

      connection_req.value()->set_legacy_pairing_state(
          std::move(legacy_pairing_state));
    }

    connection_req.value()->legacy_pairing_state()->OnPinCodeRequest(
        std::move(pin_code_cb));
  }

  return hci::CommandChannel::EventCallbackResult::kContinue;
}

void BrEdrConnectionManager::HandleNonAclConnectionRequest(
    const DeviceAddress& addr, pw::bluetooth::emboss::LinkType link_type) {
  PW_DCHECK(link_type != pw::bluetooth::emboss::LinkType::ACL);

  // Initialize the peer if it doesn't exist, to ensure we have allocated a
  // PeerId
  auto peer = FindOrInitPeer(addr);
  auto peer_id = peer->identifier();

  if (link_type != pw::bluetooth::emboss::LinkType::SCO &&
      link_type != pw::bluetooth::emboss::LinkType::ESCO) {
    bt_log(WARN,
           "gap-bredr",
           "reject unsupported connection type %s (peer: %s, addr: %s)",
           hci_spec::LinkTypeToString(link_type),
           bt_str(peer_id),
           bt_str(addr));
    SendRejectConnectionRequest(
        addr,
        pw::bluetooth::emboss::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
    return;
  }

  auto conn_pair = FindConnectionByAddress(addr.value());
  if (!conn_pair) {
    bt_log(WARN,
           "gap-bredr",
           "rejecting (e)SCO connection request for peer that is not connected "
           "(peer: %s, addr: %s)",
           bt_str(peer_id),
           bt_str(addr));
    SendRejectSynchronousRequest(
        addr,
        pw::bluetooth::emboss::StatusCode::UNACCEPTABLE_CONNECTION_PARAMETERS);
    return;
  }

  // The ScoConnectionManager owned by the BrEdrConnection will respond.
  bt_log(DEBUG,
         "gap-bredr",
         "SCO request ignored, handled by ScoConnectionManager (peer: %s, "
         "addr: %s)",
         bt_str(peer_id),
         bt_str(addr));
}

bool BrEdrConnectionManager::Connect(
    PeerId peer_id, ConnectResultCallback on_connection_result) {
  Peer* peer = cache_->FindById(peer_id);
  if (!peer) {
    bt_log(WARN,
           "gap-bredr",
           "%s: peer not found (peer: %s)",
           __func__,
           bt_str(peer_id));
    return false;
  }

  if (peer->technology() == TechnologyType::kLowEnergy) {
    bt_log(
        ERROR, "gap-bredr", "peer does not support BrEdr: %s", bt_str(*peer));
    return false;
  }

  // Succeed immediately or after interrogation if there is already an active
  // connection.
  auto conn_pair = FindConnectionById(peer_id);
  if (conn_pair) {
    conn_pair->second->AddRequestCallback(std::move(on_connection_result));
    return true;
  }

  // Actively trying to connect to this peer means we can remove it from the
  // denylist.
  deny_incoming_.remove(peer->address());

  // If we are already waiting to connect to |peer_id| then we store
  // |on_connection_result| to be processed after the connection attempt
  // completes (in either success of failure).
  auto pending_iter = connection_requests_.find(peer_id);
  if (pending_iter != connection_requests_.end()) {
    pending_iter->second.AddCallback(std::move(on_connection_result));
    return true;
  }
  // If we are not already connected or pending, initiate a new connection
  auto [request_iter, _] = connection_requests_.try_emplace(
      peer_id,
      dispatcher_,
      peer->address(),
      peer_id,
      peer->MutBrEdr().RegisterInitializingConnection(),
      std::move(on_connection_result));
  request_iter->second.AttachInspect(
      inspect_properties_.requests_node_,
      inspect_properties_.requests_node_.UniqueName(
          kInspectRequestNodeNamePrefix));

  TryCreateNextConnection();

  return true;
}

void BrEdrConnectionManager::TryCreateNextConnection() {
  // There can only be one outstanding BrEdr CreateConnection request at a time
  if (pending_request_) {
    return;
  }

  // Find next outgoing BR/EDR request if available
  auto is_bredr_and_outgoing = [this](const auto& key_val) {
    PeerId peer_id = key_val.first;
    const BrEdrConnectionRequest* request = &key_val.second;
    const Peer* peer = cache_->FindById(peer_id);
    return peer->bredr() && !request->HasIncoming();
  };

  auto it = std::find_if(connection_requests_.begin(),
                         connection_requests_.end(),
                         is_bredr_and_outgoing);
  if (it == connection_requests_.end()) {
    return;
  }

  PeerId peer_id = it->first;
  BrEdrConnectionRequest& request = it->second;
  const Peer* peer = cache_->FindById(peer_id);

  auto self = weak_self_.GetWeakPtr();
  auto on_failure = [self, addr = request.address()](hci::Result<> status,
                                                     PeerId id) {
    if (self.is_alive() && status.is_error()) {
      self->CompleteRequest(id, addr, status, /*handle=*/0);
    }
  };
  auto on_timeout = [self] {
    if (self.is_alive()) {
      self->OnRequestTimeout();
    }
  };

  const std::optional<uint16_t> clock_offset = peer->bredr()->clock_offset();
  const std::optional<pw::bluetooth::emboss::PageScanRepetitionMode>
      page_scan_repetition_mode = peer->bredr()->page_scan_repetition_mode();
  pending_request_ =
      request.CreateHciConnectionRequest(hci_->command_channel(),
                                         clock_offset,
                                         page_scan_repetition_mode,
                                         std::move(on_timeout),
                                         std::move(on_failure),
                                         dispatcher_);

  inspect_properties_.outgoing_.connection_attempts_.Add(1);
}

void BrEdrConnectionManager::OnRequestTimeout() {
  if (pending_request_) {
    pending_request_->Timeout();
    SendCreateConnectionCancelCommand(pending_request_->peer_address());
  }
}

void BrEdrConnectionManager::SendCreateConnectionCancelCommand(
    DeviceAddress addr) {
  auto cancel = hci::CommandPacket::New<
      pw::bluetooth::emboss::CreateConnectionCancelCommandWriter>(
      hci_spec::kCreateConnectionCancel);
  auto params = cancel.view_t();
  params.bd_addr().CopyFrom(addr.value().view());
  hci_->command_channel()
      ->SendCommand(std::move(cancel),
                    [](auto, const hci::EventPacket& event) {
                      HCI_IS_ERROR(event,
                                   WARN,
                                   "hci-bredr",
                                   "failed to cancel connection request");
                    })
      .IgnoreError();
}

void BrEdrConnectionManager::SendAuthenticationRequested(
    hci_spec::ConnectionHandle handle, hci::ResultFunction<> cb) {
  auto auth_request = hci::CommandPacket::New<
      pw::bluetooth::emboss::AuthenticationRequestedCommandWriter>(
      hci_spec::kAuthenticationRequested);
  auth_request.view_t().connection_handle().Write(handle);

  // Complete on command status because Authentication Complete Event is already
  // registered.
  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [callback = std::move(cb)](auto,
                                            const hci::EventPacket& event) {
      callback(event.ToResult());
    };
  }
  hci_->command_channel()
      ->SendCommand(std::move(auth_request),
                    std::move(command_cb),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

void BrEdrConnectionManager::SendIoCapabilityRequestReply(
    DeviceAddressBytes bd_addr,
    pw::bluetooth::emboss::IoCapability io_capability,
    pw::bluetooth::emboss::OobDataPresent oob_data_present,
    pw::bluetooth::emboss::AuthenticationRequirements auth_requirements,
    hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::IoCapabilityRequestReplyCommandWriter>(
      hci_spec::kIOCapabilityRequestReply);
  auto params = packet.view_t();
  params.bd_addr().CopyFrom(bd_addr.view());
  params.io_capability().Write(io_capability);
  params.oob_data_present().Write(oob_data_present);
  params.authentication_requirements().Write(auth_requirements);
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendIoCapabilityRequestNegativeReply(
    DeviceAddressBytes bd_addr,
    pw::bluetooth::emboss::StatusCode reason,
    hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::IoCapabilityRequestNegativeReplyCommandWriter>(
      hci_spec::kIOCapabilityRequestNegativeReply);
  auto params = packet.view_t();
  params.bd_addr().CopyFrom(bd_addr.view());
  params.reason().Write(reason);
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserConfirmationRequestReply(
    DeviceAddressBytes bd_addr, hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::UserConfirmationRequestReplyCommandWriter>(
      hci_spec::kUserConfirmationRequestReply);
  packet.view_t().bd_addr().CopyFrom(bd_addr.view());
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserConfirmationRequestNegativeReply(
    DeviceAddressBytes bd_addr, hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::UserConfirmationRequestNegativeReplyCommandWriter>(
      hci_spec::kUserConfirmationRequestNegativeReply);
  packet.view_t().bd_addr().CopyFrom(bd_addr.view());
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserPasskeyRequestReply(
    DeviceAddressBytes bd_addr,
    uint32_t numeric_value,
    hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::UserPasskeyRequestReplyCommandWriter>(
      hci_spec::kUserPasskeyRequestReply);
  auto view = packet.view_t();
  view.bd_addr().CopyFrom(bd_addr.view());
  view.numeric_value().Write(numeric_value);
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendUserPasskeyRequestNegativeReply(
    DeviceAddressBytes bd_addr, hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::UserPasskeyRequestNegativeReplyCommandWriter>(
      hci_spec::kUserPasskeyRequestNegativeReply);
  packet.view_t().bd_addr().CopyFrom(bd_addr.view());
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendLinkKeyRequestNegativeReply(
    DeviceAddressBytes bd_addr, hci::ResultFunction<> cb) {
  auto negative_reply = hci::CommandPacket::New<
      pw::bluetooth::emboss::LinkKeyRequestNegativeReplyCommandWriter>(
      hci_spec::kLinkKeyRequestNegativeReply);
  auto negative_reply_params = negative_reply.view_t();
  negative_reply_params.bd_addr().CopyFrom(bd_addr.view());
  SendCommandWithStatusCallback(std::move(negative_reply), std::move(cb));
}

void BrEdrConnectionManager::SendLinkKeyRequestReply(DeviceAddressBytes bd_addr,
                                                     hci_spec::LinkKey link_key,
                                                     hci::ResultFunction<> cb) {
  auto reply = hci::CommandPacket::New<
      pw::bluetooth::emboss::LinkKeyRequestReplyCommandWriter>(
      hci_spec::kLinkKeyRequestReply);
  auto reply_params = reply.view_t();
  reply_params.bd_addr().CopyFrom(bd_addr.view());

  auto link_key_value_view = link_key.view();
  reply_params.link_key().CopyFrom(link_key_value_view);

  SendCommandWithStatusCallback(std::move(reply), std::move(cb));
}

template <typename T>
void BrEdrConnectionManager::SendCommandWithStatusCallback(
    T command_packet, hci::ResultFunction<> cb) {
  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [callback = std::move(cb)](auto,
                                            const hci::EventPacket& event) {
      callback(event.ToResult());
    };
  }
  hci_->command_channel()
      ->SendCommand(std::move(command_packet), std::move(command_cb))
      .IgnoreError();
}

void BrEdrConnectionManager::SendAcceptConnectionRequest(
    DeviceAddressBytes addr, hci::ResultFunction<> cb) {
  auto accept = hci::CommandPacket::New<
      pw::bluetooth::emboss::AcceptConnectionRequestCommandWriter>(
      hci_spec::kAcceptConnectionRequest);
  auto accept_params = accept.view_t();
  accept_params.bd_addr().CopyFrom(addr.view());
  // This role switch preference can fail. A HCI_Role_Change event will be
  // generated if the role switch is successful (Core Spec v5.2, Vol 2, Part F,
  // Sec 3.1).
  accept_params.role().Write(pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [callback = std::move(cb)](auto,
                                            const hci::EventPacket& event) {
      callback(event.ToResult());
    };
  }

  hci_->command_channel()
      ->SendCommand(std::move(accept),
                    std::move(command_cb),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

void BrEdrConnectionManager::SendRejectConnectionRequest(
    DeviceAddress addr,
    pw::bluetooth::emboss::StatusCode reason,
    hci::ResultFunction<> cb) {
  auto reject = hci::CommandPacket::New<
      pw::bluetooth::emboss::RejectConnectionRequestCommandWriter>(
      hci_spec::kRejectConnectionRequest);
  auto reject_params = reject.view_t();
  reject_params.bd_addr().CopyFrom(addr.value().view());
  reject_params.reason().Write(reason);

  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [callback = std::move(cb)](auto,
                                            const hci::EventPacket& event) {
      callback(event.ToResult());
    };
  }

  hci_->command_channel()
      ->SendCommand(std::move(reject),
                    std::move(command_cb),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

void BrEdrConnectionManager::SendRejectSynchronousRequest(
    DeviceAddress addr,
    pw::bluetooth::emboss::StatusCode reason,
    hci::ResultFunction<> cb) {
  auto reject = hci::CommandPacket::New<
      pw::bluetooth::emboss::RejectSynchronousConnectionRequestCommandWriter>(
      hci_spec::kRejectSynchronousConnectionRequest);
  auto reject_params = reject.view_t();
  reject_params.bd_addr().CopyFrom(addr.value().view());
  reject_params.reason().Write(reason);

  hci::CommandChannel::CommandCallback command_cb;
  if (cb) {
    command_cb = [callback = std::move(cb)](auto,
                                            const hci::EventPacket& event) {
      callback(event.ToResult());
    };
  }

  hci_->command_channel()
      ->SendCommand(std::move(reject),
                    std::move(command_cb),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

void BrEdrConnectionManager::SendPinCodeRequestReply(DeviceAddressBytes bd_addr,
                                                     uint16_t pin_code,
                                                     hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::PinCodeRequestReplyCommandWriter>(
      hci_spec::kPinCodeRequestReply);
  auto params = packet.view_t();
  params.bd_addr().CopyFrom(bd_addr.view());

  // 4-digit PIN codes are generated
  params.pin_code_length().Write(4);

  MutableBufferView data_view(params.pin_code().BackingStorage().data(),
                              params.pin_code().SizeInBytes());
  data_view.SetToZeros();

  // Convert pin code int to string (4 digits + 1 digit for null character)
  pw::StringBuffer<5> builder;
  builder << pin_code;
  data_view.Write(BufferView(builder));

  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::SendPinCodeRequestNegativeReply(
    DeviceAddressBytes bd_addr, hci::ResultFunction<> cb) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::PinCodeRequestNegativeReplyCommandWriter>(
      hci_spec::kPinCodeRequestNegativeReply);
  auto params = packet.view_t();
  params.bd_addr().CopyFrom(bd_addr.view());
  SendCommandWithStatusCallback(std::move(packet), std::move(cb));
}

void BrEdrConnectionManager::RecordDisconnectInspect(
    const BrEdrConnection& conn, DisconnectReason reason) {
  // Add item to recent disconnections list.
  auto& inspect_item = inspect_properties_.last_disconnected_list.CreateItem();
  inspect_item.node.RecordString(kInspectLastDisconnectedItemPeerPropertyName,
                                 conn.peer_id().ToString());
  uint64_t conn_duration_s =
      std::chrono::duration_cast<std::chrono::seconds>(conn.duration()).count();
  inspect_item.node.RecordUint(kInspectLastDisconnectedItemDurationPropertyName,
                               conn_duration_s);

  int64_t time_ns = dispatcher_.now().time_since_epoch().count();
  inspect_item.node.RecordInt(kInspectTimestampPropertyName, time_ns);

  switch (reason) {
    case DisconnectReason::kApiRequest:
      inspect_properties_.disconnect_local_api_request_count_.Add(1);
      break;
    case DisconnectReason::kInterrogationFailed:
      inspect_properties_.disconnect_interrogation_failed_count_.Add(1);
      break;
    case DisconnectReason::kPairingFailed:
      inspect_properties_.disconnect_pairing_failed_count_.Add(1);
      break;
    case DisconnectReason::kAclLinkError:
      inspect_properties_.disconnect_acl_link_error_count_.Add(1);
      break;
    case DisconnectReason::kPeerDisconnection:
      inspect_properties_.disconnect_peer_disconnection_count_.Add(1);
      break;
    default:
      break;
  }
}

}  // namespace bt::gap
