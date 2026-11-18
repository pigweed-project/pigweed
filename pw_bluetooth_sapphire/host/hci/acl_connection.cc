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

#include "pw_bluetooth_sapphire/internal/host/hci/acl_connection.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {

namespace {

template <CommandChannel::EventCallbackResult (
    AclConnection::*EventHandlerMethod)(const EventPacket&)>
CommandChannel::EventCallback BindEventHandler(
    const WeakPtr<AclConnection>& conn) {
  return [conn](const EventPacket& event) {
    if (conn.is_alive()) {
      return (conn.get().*EventHandlerMethod)(event);
    }
    return CommandChannel::EventCallbackResult::kRemove;
  };
}

}  // namespace

AclConnection::AclConnection(hci_spec::ConnectionHandle handle,
                             const DeviceAddress& local_address,
                             const DeviceAddress& peer_address,
                             pw::bluetooth::emboss::ConnectionRole role,
                             const Transport::WeakPtr& hci,
                             pw::async::Dispatcher& dispatcher)
    : Connection(handle,
                 hci,
                 dispatcher,
                 [handle, hci] {
                   AclConnection::OnDisconnectionComplete(handle, hci);
                 }),
      role_(role),
      local_address_(local_address),
      peer_address_(peer_address),
      weak_self_(this) {
  auto self = weak_self_.GetWeakPtr();
  enc_change_id_ = hci->command_channel()->AddEventHandler(
      hci_spec::kEncryptionChangeEventCode,
      BindEventHandler<&AclConnection::OnEncryptionChangeEvent>(self));
  enc_key_refresh_cmpl_id_ = hci->command_channel()->AddEventHandler(
      hci_spec::kEncryptionKeyRefreshCompleteEventCode,
      BindEventHandler<&AclConnection::OnEncryptionKeyRefreshCompleteEvent>(
          self));
}

AclConnection::~AclConnection() {
  // Unregister HCI event handlers.
  hci()->command_channel()->RemoveEventHandler(enc_change_id_);
  hci()->command_channel()->RemoveEventHandler(enc_key_refresh_cmpl_id_);
}

void AclConnection::OnDisconnectionComplete(hci_spec::ConnectionHandle handle,
                                            const Transport::WeakPtr& hci) {
  if (!hci.is_alive()) {
    return;
  }
  // Notify ACL data channel that packets have been flushed from controller
  // buffer.
  hci->acl_data_channel()->ClearControllerPacketCount(handle);
}

CommandChannel::EventCallbackResult AclConnection::OnEncryptionChangeEvent(
    const EventPacket& event) {
  PW_CHECK(event.event_code() == hci_spec::kEncryptionChangeEventCode);

  auto params =
      event
          .unchecked_view<pw::bluetooth::emboss::EncryptionChangeEventV1View>();
  if (!params.Ok()) {
    bt_log(WARN, "hci", "malformed encryption change event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  hci_spec::ConnectionHandle handle = params.connection_handle().Read();

  // Silently ignore the event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  if (state() != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "encryption change ignored for closed connection");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  Result<> result = event.ToResult();
  const pw::bluetooth::emboss::EncryptionStatus event_encryption_status =
      params.encryption_enabled().Read();
  const bool encryption_enabled =
      event_encryption_status != pw::bluetooth::emboss::EncryptionStatus::OFF;
  if (result.is_ok()) {
    encryption_status_ = event_encryption_status;
  }

  bt_log(
      DEBUG,
      "hci",
      "encryption change (%s) %s",
      result.is_ok() ? (encryption_enabled ? "enabled" : "disabled") : "failed",
      bt_str(result));

  // If BR/EDR Secure Connections is used on this link, verify that enabling
  // encryption uses AES.
  if (result.is_ok() && encryption_enabled && use_secure_connections_ &&
      encryption_status_ !=
          pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR) {
    bt_log(DEBUG,
           "hci",
           "BR/EDR Secure Connection must use AES encryption. Closing "
           "connection...");
    HandleEncryptionStatus(fit::error(Error(HostError::kInsufficientSecurity)),
                           /*key_refreshed=*/false);
    return CommandChannel::EventCallbackResult::kContinue;
  }

  HandleEncryptionStatus(result.is_ok()
                             ? Result<bool>(fit::ok(encryption_enabled))
                             : result.take_error(),
                         /*key_refreshed=*/false);
  return CommandChannel::EventCallbackResult::kContinue;
}

CommandChannel::EventCallbackResult
AclConnection::OnEncryptionKeyRefreshCompleteEvent(const EventPacket& event) {
  PW_CHECK(event.event_code() ==
           hci_spec::kEncryptionKeyRefreshCompleteEventCode);

  const auto params = event.unchecked_view<
      pw::bluetooth::emboss::EncryptionKeyRefreshCompleteEventView>();
  if (!params.Ok()) {
    bt_log(WARN, "hci", "malformed encryption key refresh complete event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  const hci_spec::ConnectionHandle handle = params.connection_handle().Read();

  // Silently ignore this event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  if (state() != Connection::State::kConnected) {
    bt_log(
        DEBUG, "hci", "encryption key refresh ignored for closed connection");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  Result<> status = event.ToResult();
  bt_log(DEBUG, "hci", "encryption key refresh %s", bt_str(status));

  // If BR/EDR Secure Connections is used on this link, verify that a successful
  // Encryption Key Refresh event has AES encryption enabled.
  if (status.is_ok() && use_secure_connections_ &&
      encryption_status_ !=
          pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR) {
    bt_log(DEBUG,
           "hci",
           "BR/EDR Secure Connection key refresh must use AES encryption. "
           "Closing connection...");
    HandleEncryptionStatus(fit::error(Error(HostError::kInsufficientSecurity)),
                           /*key_refreshed=*/true);
    return CommandChannel::EventCallbackResult::kContinue;
  }

  HandleEncryptionStatus(status.is_ok()
                             ? Result<bool>(fit::ok(/*enabled=*/true))
                             : status.take_error(),
                         /*key_refreshed=*/true);

  return CommandChannel::EventCallbackResult::kContinue;
}

}  // namespace bt::hci
