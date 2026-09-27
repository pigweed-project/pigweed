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

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <cinttypes>

#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"
#include "pw_string/string_builder.h"

namespace bt::hci {

LowEnergyConnection::LowEnergyConnection(
    hci_spec::ConnectionHandle handle,
    const DeviceAddress& local_address,
    const DeviceAddress& peer_address,
    const hci_spec::LEConnectionParameters& params,
    pw::bluetooth::emboss::ConnectionRole role,
    const Transport::WeakPtr& hci,
    pw::async::Dispatcher& dispatcher)
    : AclConnection(handle, local_address, peer_address, role, hci, dispatcher),
      WeakSelf(this),
      parameters_(params) {
  PW_CHECK(local_address.type() != DeviceAddress::Type::kBREDR);
  PW_CHECK(peer_address.type() != DeviceAddress::Type::kBREDR);
  PW_CHECK(hci.is_alive());

  le_ltk_request_id_ = hci->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLELongTermKeyRequestSubeventCode,
      fit::bind_member<&LowEnergyConnection::OnLELongTermKeyRequestEvent>(
          this));
}

LowEnergyConnection::~LowEnergyConnection() {
  // Unregister HCI event handlers.
  if (hci().is_alive()) {
    hci()->command_channel()->RemoveEventHandler(le_ltk_request_id_);
  }
}

bool LowEnergyConnection::StartEncryption() {
  if (state() != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "connection closed; cannot start encryption");
    return false;
  }
  if (role() != pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
    bt_log(DEBUG, "hci", "only the central can start encryption");
    return false;
  }
  if (!ltk().has_value()) {
    bt_log(DEBUG, "hci", "connection has no LTK; cannot start encryption");
    return false;
  }

  auto cmd = CommandPacket::New<
      pw::bluetooth::emboss::LEEnableEncryptionCommandWriter>(
      hci_spec::kLEStartEncryption);
  auto params = cmd.view_t();
  params.connection_handle().Write(handle());
  params.random_number().Write(ltk()->rand());
  params.encrypted_diversifier().Write(ltk()->ediv());
  params.long_term_key().CopyFrom(
      pw::bluetooth::emboss::LinkKeyView(&ltk()->value()));

  auto event_cb = [self = GetWeakPtr(), handle = handle()](
                      auto, const EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }

    Result<> result = event.ToResult();
    if (bt_is_error(result,
                    ERROR,
                    "hci-le",
                    "could not set encryption on link %#.04x",
                    handle)) {
      if (self->encryption_change_callback()) {
        self->encryption_change_callback()(result.take_error());
      }
      return;
    }
    bt_log(DEBUG, "hci-le", "requested encryption start on %#.04x", handle);
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

void LowEnergyConnection::HandleEncryptionStatus(Result<bool> result,
                                                 bool /*key_refreshed*/) {
  // "On an authentication failure, the connection shall be automatically
  // disconnected by the Link Layer." (HCI_LE_Start_Encryption, Vol 2, Part E,
  // 7.8.24). We make sure of this by telling the controller to disconnect.
  // We also disconnect if a peer successfully disables encryption
  // (!result.value()) on an established LE link: upper layers (L2CAP, GAP) do
  // not re-evaluate channel security on encryption change, so allowing the link
  // to continue would expose previously-authorized channels over plaintext.
  if (state() == Connection::State::kConnected &&
      (result.is_error() || !result.value())) {
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

CommandChannel::EventCallbackResult
LowEnergyConnection::OnLELongTermKeyRequestEvent(const EventPacket& event) {
  auto view = event.unchecked_view<
      pw::bluetooth::emboss::LELongTermKeyRequestSubeventView>();
  if (!view.IsComplete()) {
    bt_log(WARN, "hci", "malformed LE LTK request event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  hci_spec::ConnectionHandle handle = view.connection_handle().Read();

  // Silently ignore the event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  uint64_t rand = view.random_number().Read();
  uint16_t ediv = view.encrypted_diversifier().Read();

  bt_log(DEBUG,
         "hci",
         "LE LTK request - ediv: %#.4x, rand: %#.16" PRIx64,
         ediv,
         rand);

  if (!hci().is_alive()) {
    return CommandChannel::EventCallbackResult::kRemove;
  }

  auto status_cb = [](auto, const EventPacket& status_event) {
    HCI_IS_ERROR(
        status_event, TRACE, "hci-le", "failed to reply to LTK request");
  };

  // TODO(fxbug.dev/388607971): The LTK may be stale if BR/EDR cross-transport
  // key derivation was performed. Maybe move this method to
  // sm::SecurityManager.
  if (ltk() && ltk()->rand() == rand && ltk()->ediv() == ediv) {
    auto cmd = CommandPacket::New<
        pw::bluetooth::emboss::LELongTermKeyRequestReplyCommandWriter>(
        hci_spec::kLELongTermKeyRequestReply);
    auto cmd_view = cmd.view_t();
    cmd_view.connection_handle().Write(handle);
    cmd_view.long_term_key().CopyFrom(
        pw::bluetooth::emboss::LinkKeyView(&ltk()->value()));
    hci()
        ->command_channel()
        ->SendCommand(cmd, std::move(status_cb))
        .IgnoreError();
  } else {
    bt_log(DEBUG, "hci-le", "LTK request rejected");

    auto cmd = CommandPacket::New<
        pw::bluetooth::emboss::LELongTermKeyRequestNegativeReplyCommandWriter>(
        hci_spec::kLELongTermKeyRequestNegativeReply);
    auto cmd_view = cmd.view_t();
    cmd_view.connection_handle().Write(handle);
    hci()
        ->command_channel()
        ->SendCommand(cmd, std::move(status_cb))
        .IgnoreError();
  }

  return CommandChannel::EventCallbackResult::kContinue;
}

std::string LowEnergyConnection::ToString() const {
  pw::StringBuffer<256> buffer;
  buffer << "[HCI LE connection (handle: ";
  buffer << handle();

  buffer << ", role: ";
  if (role() == pw::bluetooth::emboss::ConnectionRole::CENTRAL) {
    buffer << "central";
  } else {
    buffer << "peripheral";
  }

  buffer << ", " << parameters_.ToString();
  buffer << ")]";
  return buffer.c_str();
}

}  // namespace bt::hci
