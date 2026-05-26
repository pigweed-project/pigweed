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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_stream_manager.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

namespace bt::iso {

hci_spec::ConnectionHandle IsoStreamManager::Connection::handle() const {
  PW_CHECK(is_peripheral());
  return *handle_;
}

IsoStreamManager::IsoStreamManager(
    Connection conn,
    hci::Transport::WeakPtr hci,
    pw::async::Dispatcher& dispatcher,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : conn_(conn),
      hci_(std::move(hci)),
      dispatcher_(dispatcher),
      wake_lease_provider_(wake_lease_provider),
      weak_self_(this) {
  PW_CHECK(hci_.is_alive());
  cmd_ = hci_->command_channel()->AsWeakPtr();
  PW_CHECK(cmd_.is_alive());

  auto weak_self = GetWeakPtr();
  if (conn_.is_peripheral()) {
    cis_request_handler_ = cmd_->AddLEMetaEventHandler(
        hci_spec::kLECISRequestSubeventCode,
        [self = weak_self](const hci::EventPacket& event) {
          if (!self.is_alive()) {
            return hci::CommandChannel::EventCallbackResult::kRemove;
          }
          self->OnCisRequest(event);
          return hci::CommandChannel::EventCallbackResult::kContinue;
        });
  }
}

IsoStreamManager::~IsoStreamManager() {
  if (cmd_.is_alive() && conn_.is_peripheral()) {
    cmd_->RemoveEventHandler(cis_request_handler_);
  }
  if (hci_.is_alive()) {
    hci::IsoDataChannel* iso_data_channel = hci_->iso_data_channel();
    if (iso_data_channel) {
      for (const auto& stream : streams_) {
        hci_spec::ConnectionHandle cis_handle = stream.second->cis_handle();
        bt_log(INFO,
               "iso",
               "unregistering ISO connection for handle %#x",
               cis_handle);
        iso_data_channel->UnregisterConnection(cis_handle);
      }
    }
  }
}

AcceptCisStatus IsoStreamManager::AcceptCis(CigCisIdentifier id,
                                            CisEstablishedCallback cb) {
  bt_log(INFO,
         "iso",
         "IsoStreamManager: preparing to accept incoming connection (CIG: %u, "
         "CIS: %u)",
         id.cig_id(),
         id.cis_id());

  if (conn_.is_central()) {
    return AcceptCisStatus::kNotPeripheral;
  }

  if (accept_handlers_.count(id) != 0) {
    return AcceptCisStatus::kAlreadyExists;
  }

  if (streams_.count(id) != 0) {
    return AcceptCisStatus::kAlreadyExists;
  }

  accept_handlers_[id] = std::move(cb);
  return AcceptCisStatus::kSuccess;
}

IsoStream::WeakPtr IsoStreamManager::CreateCisConfiguration(
    CigCisIdentifier id,
    hci_spec::ConnectionHandle cis_handle,
    CisEstablishedCallback on_established_cb,
    pw::Callback<void()> on_closed_cb) {
  auto cis = IsoStream::Create(id.cig_id(),
                               id.cis_id(),
                               cis_handle,
                               hci_,
                               dispatcher_,
                               std::move(on_established_cb),
                               std::move(on_closed_cb),
                               wake_lease_provider_);
  auto result = cis->GetWeakPtr();
  streams_.emplace(id, std::move(cis));
  return result;
}

void IsoStreamManager::OnCisRequest(const hci::EventPacket& event) {
  PW_CHECK(event.event_code() == hci_spec::kLEMetaEventCode);
  PW_CHECK(conn_.is_peripheral());

  auto event_view =
      event.view<pw::bluetooth::emboss::LECISRequestSubeventView>();
  PW_CHECK(event_view.le_meta_event().subevent_code().Read() ==
           hci_spec::kLECISRequestSubeventCode);

  hci_spec::ConnectionHandle request_handle =
      event_view.acl_connection_handle().Read();
  uint8_t cig_id = event_view.cig_id().Read();
  uint8_t cis_id = event_view.cis_id().Read();
  CigCisIdentifier id(cig_id, cis_id);

  bt_log(INFO,
         "iso",
         "CIS request received for handle 0x%x (CIG: %u, CIS: %u)",
         request_handle,
         cig_id,
         cis_id);

  // Ignore any requests that are not intended for this connection.
  if (request_handle != conn_.handle()) {
    bt_log(DEBUG,
           "iso",
           "ignoring incoming stream request for handle 0x%x (ours: 0x%x)",
           request_handle,
           conn_.handle());
    return;
  }

  // If we are not waiting on this request, reject it
  if (accept_handlers_.count(id) == 0) {
    bt_log(INFO, "iso", "Rejecting incoming request");
    RejectCisRequest(event_view);
    return;
  }

  bt_log(INFO, "iso", "Accepting incoming request");

  // We should not already have an established stream using this same CIG/CIS
  // permutation.
  PW_CHECK(streams_.count(id) == 0, "(cig = %u, cis = %u)", cig_id, cis_id);
  CisEstablishedCallback cb = std::move(accept_handlers_[id]);
  accept_handlers_.erase(id);
  AcceptCisRequest(event_view, std::move(cb));
}

void IsoStreamManager::AcceptCisRequest(
    const pw::bluetooth::emboss::LECISRequestSubeventView& event_view,
    CisEstablishedCallback cb) {
  uint8_t cig_id = event_view.cig_id().Read();
  uint8_t cis_id = event_view.cis_id().Read();
  CigCisIdentifier id(cig_id, cis_id);

  hci_spec::ConnectionHandle cis_handle =
      event_view.cis_connection_handle().Read();

  PW_CHECK(streams_.count(id) == 0);

  auto on_closed_cb = [this, id, cis_handle]() {
    if (hci_.is_alive()) {
      hci::IsoDataChannel* iso_data_channel = hci_->iso_data_channel();
      if (iso_data_channel) {
        bt_log(INFO,
               "iso",
               "unregistering ISO connection for handle %#x",
               cis_handle);
        iso_data_channel->UnregisterConnection(cis_handle);
      }
    }
    streams_.erase(id);
  };

  PW_CHECK(hci_.is_alive(),
           "ISO transport no longer available in AcceptCisRequest (handle %#x)",
           cis_handle);
  streams_[id] = IsoStream::Create(cig_id,
                                   cis_id,
                                   cis_handle,
                                   hci_,
                                   dispatcher_,
                                   std::move(cb),
                                   on_closed_cb,
                                   wake_lease_provider_);

  auto command = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEAcceptCISRequestCommandWriter>(
      hci_spec::kLEAcceptCISRequest);
  auto cmd_view = command.view_t();
  cmd_view.connection_handle().Write(cis_handle);

  auto self = GetWeakPtr();
  auto cmd_complete_cb = [cis_handle, id, self](auto,
                                                const hci::EventPacket& event) {
    bt_log(INFO, "iso", "LE_Accept_CIS_Request command response received");
    if (!self.is_alive()) {
      return;
    }
    if (HCI_IS_ERROR(event,
                     WARN,
                     "bt-iso",
                     "accept CIS request failed for handle %#x",
                     cis_handle)) {
      self->streams_.erase(id);
      return;
    }
    hci::IsoDataChannel* iso_data_channel = self->hci_->iso_data_channel();
    iso_data_channel->RegisterConnection(cis_handle,
                                         self->streams_[id]->GetWeakPtr());
  };

  cmd_->SendCommand(std::move(command), cmd_complete_cb).IgnoreError();
}

void IsoStreamManager::RejectCisRequest(
    const pw::bluetooth::emboss::LECISRequestSubeventView& event_view) {
  hci_spec::ConnectionHandle cis_handle =
      event_view.cis_connection_handle().Read();

  auto command = hci::CommandPacket::New<
      pw::bluetooth::emboss::LERejectCISRequestCommandWriter>(
      hci_spec::kLERejectCISRequest);
  auto cmd_view = command.view_t();
  cmd_view.connection_handle().Write(cis_handle);
  cmd_view.reason().Write(pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR);

  cmd_->SendCommand(std::move(command),
                    [cis_handle](auto, const hci::EventPacket& event) {
                      bt_log(INFO, "iso", "LE_Reject_CIS_Request command sent");
                      HCI_IS_ERROR(event,
                                   ERROR,
                                   "bt-iso",
                                   "reject CIS request failed for handle %#x",
                                   cis_handle);
                    })
      .IgnoreError();
}

}  // namespace bt::iso
