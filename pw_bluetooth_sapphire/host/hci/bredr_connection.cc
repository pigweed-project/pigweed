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

#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"

#include <pw_assert/check.h>

#include "pw_bluetooth/hci_commands.emb.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {

BrEdrConnection::BrEdrConnection(hci_spec::ConnectionHandle handle,
                                 const DeviceAddress& local_address,
                                 const DeviceAddress& peer_address,
                                 pw::bluetooth::emboss::ConnectionRole role,
                                 const Transport::WeakPtr& hci,
                                 pw::async::Dispatcher& dispatcher)
    : AclConnection(handle, local_address, peer_address, role, hci, dispatcher),
      WeakSelf(this) {
  PW_CHECK(local_address.type() == DeviceAddress::Type::kBREDR);
  PW_CHECK(peer_address.type() == DeviceAddress::Type::kBREDR);
  PW_CHECK(hci.is_alive());
  PW_CHECK(hci->acl_data_channel());
}

void BrEdrConnection::EnsureAllPacketTypesEnabled() {
  auto cmd = CommandPacket::New<
      pw::bluetooth::emboss::ChangeConnectionPacketTypeCommandWriter>(
      static_cast<hci_spec::OpCode>(
          pw::bluetooth::emboss::OpCode::CHANGE_CONNECTION_PACKET_TYPE));
  auto params = cmd.view_t();
  params.connection_handle().Write(handle());
  params.packet_type().BackingStorage().WriteUInt(
      hci_spec::kEnableAllPacketTypes);

  auto self = GetWeakPtr();
  auto event_cb = [self, handle = handle()](auto, const EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }
    Result<> result = event.ToResult();
    if (bt_is_error(
            result,
            ERROR,
            "hci-bredr",
            "failed to send Change Connection Packet Type command for %#.04x",
            handle)) {
      // Log error
    }
  };

  auto cmd_result = hci()->command_channel()->SendCommand(
      std::move(cmd), std::move(event_cb), hci_spec::kCommandStatusEventCode);
  if (!cmd_result.ok()) {
    bt_log(
        WARN,
        "hci-bredr",
        "failed to send Change Connection Packet Type command for %#.04x: %s",
        handle(),
        cmd_result.status().str());
  }
}

bool BrEdrConnection::StartEncryption() {
  if (state() != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "connection closed; cannot start encryption");
    return false;
  }

  PW_CHECK(ltk().has_value() == ltk_type_.has_value());
  if (!ltk().has_value()) {
    bt_log(
        DEBUG,
        "hci",
        "connection link key type has not been set; not starting encryption");
    return false;
  }

  auto cmd = CommandPacket::New<
      pw::bluetooth::emboss::SetConnectionEncryptionCommandWriter>(
      hci_spec::kSetConnectionEncryption);
  auto params = cmd.view_t();
  params.connection_handle().Write(handle());
  params.encryption_enable().Write(
      pw::bluetooth::emboss::GenericEnableParam::ENABLE);

  auto self = GetWeakPtr();
  auto event_cb = [self, handle = handle()](auto, const EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }

    Result<> result = event.ToResult();
    if (bt_is_error(result,
                    ERROR,
                    "hci-bredr",
                    "could not set encryption on link %#.04x",
                    handle)) {
      if (self->encryption_change_callback()) {
        self->encryption_change_callback()(result.take_error());
      }
      return;
    }
    bt_log(DEBUG, "hci-bredr", "requested encryption start on %#.04x", handle);
  };

  if (!hci().is_alive()) {
    return false;
  }
  return hci()
      ->command_channel()
      ->SendCommand(std::move(cmd),
                    std::move(event_cb),
                    hci_spec::kCommandStatusEventCode)
      .ok();
}

void BrEdrConnection::HandleEncryptionStatus(Result<bool> result,
                                             bool key_refreshed) {
  bool enabled = result.is_ok() && result.value() && !key_refreshed;
  if (enabled) {
    ValidateEncryptionKeySize([self = GetWeakPtr()](Result<> key_valid_status) {
      if (self.is_alive()) {
        self->HandleEncryptionStatusValidated(
            key_valid_status.is_ok() ? Result<bool>(fit::ok(true))
                                     : key_valid_status.take_error());
      }
    });
    return;
  }
  HandleEncryptionStatusValidated(result);
}

void BrEdrConnection::HandleEncryptionStatusValidated(Result<bool> result) {
  // Core Spec Vol 3, Part C, 5.2.2.1.1 and 5.2.2.2.1 mention disconnecting the
  // link after pairing failures (supported by TS GAP/SEC/SEM/BV-10-C), but do
  // not specify actions to take after encryption failures. We'll choose to
  // disconnect ACL links after encryption failure.
  if (result.is_error()) {
    Disconnect(pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);
  }

  if (!encryption_change_callback()) {
    bt_log(DEBUG,
           "hci",
           "%#.4x: no encryption status callback assigned",
           handle());
    return;
  }
  encryption_change_callback()(result);
}

void BrEdrConnection::ValidateEncryptionKeySize(
    hci::ResultFunction<> key_size_validity_cb) {
  PW_CHECK(state() == Connection::State::kConnected);

  auto cmd = CommandPacket::New<
      pw::bluetooth::emboss::ReadEncryptionKeySizeCommandWriter>(
      hci_spec::kReadEncryptionKeySize);
  cmd.view_t().connection_handle().Write(handle());

  auto event_cb = [self = GetWeakPtr(),
                   valid_cb = std::move(key_size_validity_cb)](
                      auto, const EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }

    Result<> result = event.ToResult();
    if (!bt_is_error(result,
                     ERROR,
                     "hci",
                     "Could not read ACL encryption key size on %#.4x",
                     self->handle())) {
      const auto return_params =
          event.view<pw::bluetooth::emboss::
                         ReadEncryptionKeySizeCommandCompleteEventView>();
      uint8_t key_size = return_params.key_size().Read();
      bt_log(TRACE,
             "hci",
             "%#.4x: encryption key size %hhu",
             self->handle(),
             key_size);

      if (key_size < hci_spec::kMinEncryptionKeySize) {
        bt_log(WARN,
               "hci",
               "%#.4x: encryption key size %hhu insufficient",
               self->handle(),
               key_size);
        result = ToResult(HostError::kInsufficientSecurity);
      }
    }
    valid_cb(result);
  };
  hci()
      ->command_channel()
      ->SendCommand(std::move(cmd), std::move(event_cb))
      .IgnoreError();
}

}  // namespace bt::hci
