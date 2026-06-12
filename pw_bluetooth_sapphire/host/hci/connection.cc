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

#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"

#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>
#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bluetooth/hci_events.emb.h>

#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"
#include "pw_chrono/system_clock.h"
#include "pw_string/string_builder.h"

namespace bt::hci {

Connection::Connection(hci_spec::ConnectionHandle handle,
                       Transport::WeakPtr hci,
                       pw::async::Dispatcher& dispatcher,
                       fit::callback<void()> on_disconnection_complete)
    : handle_(handle),
      conn_state_(State::kConnected),
      hci_(std::move(hci)),
      dispatcher_(dispatcher),
      weak_self_(this) {
  PW_CHECK(hci_.is_alive());

  auto disconn_complete_handler = [self = weak_self_.GetWeakPtr(),
                                   handle,
                                   on_disconnection_complete_cb =
                                       std::move(on_disconnection_complete)](
                                      const EventPacket& event) mutable {
    return Connection::OnDisconnectionComplete(
        self, handle, event, on_disconnection_complete_cb);
  };
  hci_->command_channel()->AddEventHandler(
      hci_spec::kDisconnectionCompleteEventCode,
      std::move(disconn_complete_handler));
}

Connection::~Connection() {
  if (conn_state_ == Connection::State::kConnected) {
    Disconnect(
        pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
  }
}

std::string Connection::ToString() const {
  return bt_lib_cpp_string::StringPrintf("[HCI connection (handle: %#.4x)]",
                                         handle_);
}

CommandChannel::EventCallbackResult Connection::OnDisconnectionComplete(
    const WeakSelf<Connection>::WeakPtr& self,
    hci_spec::ConnectionHandle handle,
    const EventPacket& event,
    fit::callback<void()>& on_disconnection_complete) {
  PW_CHECK(event.event_code() == hci_spec::kDisconnectionCompleteEventCode);

  auto view =
      event.view<pw::bluetooth::emboss::DisconnectionCompleteEventView>();
  if (!view.Ok()) {
    bt_log(WARN, "hci", "malformed disconnection complete event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  const hci_spec::ConnectionHandle event_handle =
      view.connection_handle().Read();

  // Silently ignore this event as it isn't meant for this connection.
  if (event_handle != handle) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  LogDisconnectionComplete(self, handle, event);

  if (self.is_alive()) {
    self->conn_state_ = State::kDisconnected;
  }

  // Peer disconnect. Callback may destroy connection.
  if (self.is_alive() && self->peer_disconnect_callback_) {
    self->peer_disconnect_callback_(self.get(), view.reason().Read());
  }

  // Notify subclasses after peer_disconnect_callback_ has had a chance to clean
  // up higher-level connections.
  if (on_disconnection_complete) {
    on_disconnection_complete();
  }

  return CommandChannel::EventCallbackResult::kRemove;
}

void Connection::LogDisconnectionComplete(
    const WeakSelf<Connection>::WeakPtr& self,
    hci_spec::ConnectionHandle handle,
    const EventPacket& event) {
  auto view =
      event.view<pw::bluetooth::emboss::DisconnectionCompleteEventView>();

  pw::StringBuffer<256> buffer;
  buffer << bt_str(event.ToResult());

  if (self.is_alive()) {
    buffer << ", " << self->ToString();
  } else {
    buffer << ", handle: " << handle;
  }
  buffer << ", reason: " << hci_spec::StatusCodeToString(view.reason().Read());

  buffer << ", last packet: ";
  std::optional<pw::chrono::SystemClock::time_point> last_packet_time;
  if (self.is_alive() && self->hci().is_alive()) {
    if (self->hci()->acl_data_channel()) {
      last_packet_time =
          self->hci()->acl_data_channel()->GetLastPacketTime(handle);
    }
    if (!last_packet_time.has_value() && self->hci()->sco_data_channel()) {
      last_packet_time =
          self->hci()->sco_data_channel()->GetLastPacketTime(handle);
    }
  }

  if (last_packet_time.has_value()) {
    pw::chrono::SystemClock::duration duration =
        self->dispatcher_.now() - *last_packet_time;
    buffer << std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                  .count()
           << " ms ago";
  } else {
    buffer << "never";
  }

  bt_log(INFO, "hci", "disconnection complete (%s)", buffer.c_str());
}

void Connection::Disconnect(pw::bluetooth::emboss::StatusCode reason) {
  PW_CHECK(conn_state_ == Connection::State::kConnected);

  conn_state_ = Connection::State::kWaitingForDisconnectionComplete;

  // Here we send a HCI_Disconnect command without waiting for it to complete.
  auto status_cb = [](auto, const EventPacket& event) {
    PW_DCHECK(event.event_code() == hci_spec::kCommandStatusEventCode);
    HCI_IS_ERROR(event, TRACE, "hci", "ignoring disconnection failure");
  };

  auto disconn =
      CommandPacket::New<pw::bluetooth::emboss::DisconnectCommandWriter>(
          hci_spec::kDisconnect);
  auto params = disconn.view_t();
  params.connection_handle().Write(handle());
  params.reason().Write(reason);

  bt_log(DEBUG,
         "hci",
         "disconnecting connection (handle: %#.4x, reason: %#.2hhx)",
         handle(),
         static_cast<unsigned char>(reason));

  // Send HCI Disconnect.
  hci_->command_channel()
      ->SendCommand(std::move(disconn),
                    std::move(status_cb),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

}  // namespace bt::hci
