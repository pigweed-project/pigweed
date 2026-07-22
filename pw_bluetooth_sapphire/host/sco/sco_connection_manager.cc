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

#include "pw_bluetooth_sapphire/internal/host/sco/sco_connection_manager.h"

#include <pw_assert/check.h>

#include <cinttypes>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci/sco_connection.h"

namespace bt::sco {
namespace {

bool ConnectionParametersSupportScoTransport(
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>& params) {
  return params.view().packet_types().hv1().Read() ||
         params.view().packet_types().hv2().Read() ||
         params.view().packet_types().hv3().Read();
}

bool ConnectionParametersSupportEscoTransport(
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>& params) {
  return params.view().packet_types().ev3().Read() ||
         params.view().packet_types().ev4().Read() ||
         params.view().packet_types().ev5().Read();
}

}  // namespace

ScoConnectionManager::ScoConnectionManager(
    PeerId peer_id,
    hci_spec::ConnectionHandle acl_handle,
    DeviceAddress peer_address,
    DeviceAddress local_address,
    hci::Transport::WeakPtr transport,
    pw::async::Dispatcher& dispatcher)
    : next_req_id_(0u),
      peer_id_(peer_id),
      local_address_(local_address),
      peer_address_(peer_address),
      acl_handle_(acl_handle),
      transport_(std::move(transport)),
      dispatcher_(dispatcher),
      weak_ptr_factory_(this) {
  PW_CHECK(transport_.is_alive());

  AddEventHandler(
      hci_spec::kSynchronousConnectionCompleteEventCode,
      fit::bind_member<&ScoConnectionManager::OnSynchronousConnectionComplete>(
          this));
  AddEventHandler(
      hci_spec::kConnectionRequestEventCode,
      fit::bind_member<&ScoConnectionManager::OnConnectionRequest>(this));
}

ScoConnectionManager::~ScoConnectionManager() {
  // Remove all event handlers
  for (auto handler_id : event_handler_ids_) {
    transport_->command_channel()->RemoveEventHandler(handler_id);
  }

  // Close all connections.  Close may remove the connection from the map, so we
  // can't use an iterator, which would be invalidated by the removal.
  while (connections_.size() > 0) {
    auto pair = connections_.begin();
    hci_spec::ConnectionHandle handle = pair->first;
    ScoConnection* conn = pair->second.get();

    conn->Close();
    // Make sure we erase the connection if Close doesn't so the loop
    // terminates.
    connections_.erase(handle);
  }

  if (queued_request_) {
    CancelRequestWithId(queued_request_->id);
  }

  if (in_progress_request_) {
    bt_log(DEBUG,
           "gap-sco",
           "ScoConnectionManager destroyed while request in progress");
    // Clear in_progress_request_ before calling callback to prevent calls to
    // CompleteRequest() during execution of the callback (e.g. due to
    // destroying the RequestHandle).
    ConnectionRequest request = std::move(in_progress_request_.value());
    in_progress_request_.reset();
    request.callback(fit::error(HostError::kCanceled));
  }
}

ScoConnectionManager::RequestHandle ScoConnectionManager::OpenConnection(
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
        parameters,
    OpenConnectionCallback callback) {
  return QueueRequest(
      /*initiator=*/true,
      {std::move(parameters)},
      [cb = std::move(callback)](ConnectionResult result) mutable {
        // Convert result type.
        if (result.is_error()) {
          cb(fit::error(result.take_error()));
          return;
        }
        cb(fit::ok(result.value().first));
      });
}

ScoConnectionManager::RequestHandle ScoConnectionManager::AcceptConnection(
    std::vector<bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
        parameters,
    AcceptConnectionCallback callback) {
  return QueueRequest(
      /*initiator=*/false, std::move(parameters), std::move(callback));
}

hci::CommandChannel::EventHandlerId ScoConnectionManager::AddEventHandler(
    const hci_spec::EventCode& code,
    hci::CommandChannel::EventCallback event_cb) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  hci::CommandChannel::EventHandlerId event_id = 0;
  event_id = transport_->command_channel()->AddEventHandler(
      code, [self, cb = std::move(event_cb)](const hci::EventPacket& event) {
        if (!self.is_alive()) {
          return hci::CommandChannel::EventCallbackResult::kRemove;
        }
        return cb(event);
      });
  PW_CHECK(event_id);
  event_handler_ids_.push_back(event_id);
  return event_id;
}

hci::CommandChannel::EventCallbackResult
ScoConnectionManager::OnSynchronousConnectionComplete(
    const hci::EventPacket& event) {
  const auto params = event.view<
      pw::bluetooth::emboss::SynchronousConnectionCompleteEventView>();
  DeviceAddress addr(DeviceAddress::Type::kBREDR,
                     DeviceAddressBytes(params.bd_addr()));

  // Ignore events from other peers.
  if (addr != peer_address_) {
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  auto status = event.ToResult();
  if (status.is_error()) {
    // The controller emits this event with an error status after the host
    // rejects an unsolicited (e)SCO request, in which case there is no
    // in-progress request to complete.
    if (!in_progress_request_) {
      bt_log(DEBUG,
             "gap-sco",
             "Ignoring SCO connection complete error with no request in "
             "progress (peer: %s)",
             bt_str(peer_id_));
      return hci::CommandChannel::EventCallbackResult::kContinue;
    }
    bt_log(INFO,
           "gap-sco",
           "SCO connection failed to be established; trying next "
           "parameters if available (peer: %s, status: %s)",
           bt_str(peer_id_),
           bt_str(status));
    CompleteRequestOrTryNextParameters(fit::error(HostError::kFailed));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // The controller should only report SCO and eSCO link types (other values are
  // reserved).
  pw::bluetooth::emboss::LinkType link_type = params.link_type().Read();
  if (link_type != pw::bluetooth::emboss::LinkType::SCO &&
      link_type != pw::bluetooth::emboss::LinkType::ESCO) {
    bt_log(
        ERROR,
        "gap-sco",
        "Received SynchronousConnectionComplete event with invalid link type");
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  hci_spec::ConnectionHandle connection_handle =
      params.connection_handle().Read();
  auto link = std::make_unique<hci::ScoConnection>(connection_handle,
                                                   local_address_,
                                                   peer_address_,
                                                   transport_,
                                                   dispatcher_);

  if (!in_progress_request_) {
    bt_log(ERROR,
           "gap-sco",
           "Unexpected SCO connection complete, disconnecting (peer: %s)",
           bt_str(peer_id_));
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  fit::closure deactivated_cb = [this, connection_handle] {
    PW_CHECK(connections_.erase(connection_handle));
  };
  bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
      conn_params = in_progress_request_
                        ->parameters[in_progress_request_->current_param_index];
  auto conn = std::make_unique<ScoConnection>(std::move(link),
                                              std::move(deactivated_cb),
                                              conn_params,
                                              transport_->sco_data_channel());
  ScoConnection::WeakPtr conn_weak = conn->GetWeakPtr();

  auto [_, success] =
      connections_.try_emplace(connection_handle, std::move(conn));
  PW_CHECK(success,
           "SCO connection already exists with handle %#.4x (peer: %s)",
           connection_handle,
           bt_str(peer_id_));

  CompleteRequest(fit::ok(std::make_pair(
      std::move(conn_weak), in_progress_request_->current_param_index)));

  return hci::CommandChannel::EventCallbackResult::kContinue;
}

hci::CommandChannel::EventCallbackResult
ScoConnectionManager::OnConnectionRequest(const hci::EventPacket& event) {
  PW_CHECK(event.event_code() == hci_spec::kConnectionRequestEventCode);
  auto params = event.view<pw::bluetooth::emboss::ConnectionRequestEventView>();

  // Ignore requests for other link types.
  if (params.link_type().Read() != pw::bluetooth::emboss::LinkType::SCO &&
      params.link_type().Read() != pw::bluetooth::emboss::LinkType::ESCO) {
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // Ignore requests from other peers.
  DeviceAddress addr(DeviceAddress::Type::kBREDR,
                     DeviceAddressBytes(params.bd_addr()));
  if (addr != peer_address_) {
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  if (!in_progress_request_ || in_progress_request_->initiator) {
    bt_log(INFO,
           "sco",
           "reject unexpected %s connection request (peer: %s)",
           hci_spec::LinkTypeToString(params.link_type().Read()),
           bt_str(peer_id_));
    SendRejectConnectionCommand(
        DeviceAddressBytes(params.bd_addr()),
        pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  // Skip to the next parameters that support the requested link type. The
  // controller rejects parameters that don't include packet types for the
  // requested link type.
  if ((params.link_type().Read() == pw::bluetooth::emboss::LinkType::SCO &&
       !FindNextParametersThatSupportSco()) ||
      (params.link_type().Read() == pw::bluetooth::emboss::LinkType::ESCO &&
       !FindNextParametersThatSupportEsco())) {
    bt_log(DEBUG,
           "sco",
           "in progress request parameters don't support the requested "
           "transport (%s); rejecting",
           hci_spec::LinkTypeToString(params.link_type().Read()));
    // The controller will send an HCI Synchronous Connection Complete event, so
    // the request will be completed then.
    SendRejectConnectionCommand(DeviceAddressBytes(params.bd_addr()),
                                pw::bluetooth::emboss::StatusCode::
                                    CONNECTION_REJECTED_LIMITED_RESOURCES);
    return hci::CommandChannel::EventCallbackResult::kContinue;
  }

  bt_log(INFO,
         "sco",
         "accepting incoming %s connection from %s (peer: %s)",
         hci_spec::LinkTypeToString(params.link_type().Read()),
         bt_str(DeviceAddressBytes(params.bd_addr())),
         bt_str(peer_id_));

  auto accept = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          EnhancedAcceptSynchronousConnectionRequestCommandWriter>(
      hci_spec::kEnhancedAcceptSynchronousConnectionRequest);
  auto view = accept.view_t();
  view.bd_addr().CopyFrom(params.bd_addr());
  view.connection_parameters().CopyFrom(
      in_progress_request_
          ->parameters[in_progress_request_->current_param_index]
          .view());

  SendCommandWithStatusCallback(
      std::move(accept),
      [self = weak_ptr_factory_.GetWeakPtr(),
       peer_id = peer_id_](hci::Result<> status) {
        if (!self.is_alive() || status.is_ok()) {
          return;
        }
        bt_is_error(status,
                    WARN,
                    "sco",
                    "enhanced accept SCO connection command failed, waiting "
                    "for connection complete (peer: %s",
                    bt_str(peer_id));
        // Do not complete the request here. Wait for
        // HCI_Synchronous_Connection_Complete event, which should be received
        // after Connection_Accept_Timeout with status
        // kConnectionAcceptTimeoutExceeded.
      });

  in_progress_request_->received_request = true;

  return hci::CommandChannel::EventCallbackResult::kContinue;
}

bool ScoConnectionManager::FindNextParametersThatSupportSco() {
  PW_CHECK(in_progress_request_);
  while (in_progress_request_->current_param_index <
         in_progress_request_->parameters.size()) {
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>& params =
        in_progress_request_
            ->parameters[in_progress_request_->current_param_index];
    if (ConnectionParametersSupportScoTransport(params)) {
      return true;
    }
    in_progress_request_->current_param_index++;
  }
  return false;
}

bool ScoConnectionManager::FindNextParametersThatSupportEsco() {
  PW_CHECK(in_progress_request_);
  while (in_progress_request_->current_param_index <
         in_progress_request_->parameters.size()) {
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>& params =
        in_progress_request_
            ->parameters[in_progress_request_->current_param_index];
    if (ConnectionParametersSupportEscoTransport(params)) {
      return true;
    }
    in_progress_request_->current_param_index++;
  }
  return false;
}

ScoConnectionManager::RequestHandle ScoConnectionManager::QueueRequest(
    bool initiator,
    std::vector<bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>> params,
    ConnectionCallback cb) {
  PW_CHECK(cb);

  if (params.empty()) {
    cb(fit::error(HostError::kInvalidParameters));
    return RequestHandle([]() {});
  }

  if (queued_request_) {
    CancelRequestWithId(queued_request_->id);
  }

  auto req_id = next_req_id_++;
  queued_request_ = {req_id,
                     initiator,
                     /*received_request_arg=*/false,
                     std::move(params),
                     std::move(cb)};

  TryCreateNextConnection();

  return RequestHandle([req_id, self = weak_ptr_factory_.GetWeakPtr()]() {
    if (self.is_alive()) {
      self->CancelRequestWithId(req_id);
    }
  });
}

void ScoConnectionManager::TryCreateNextConnection() {
  // Cancel an in-progress responder request that hasn't received a connection
  // request event yet.
  if (in_progress_request_) {
    CancelRequestWithId(in_progress_request_->id);
  }

  if (in_progress_request_ || !queued_request_) {
    return;
  }

  in_progress_request_ = std::move(queued_request_);
  queued_request_.reset();

  if (in_progress_request_->initiator) {
    bt_log(DEBUG,
           "gap-sco",
           "Initiating SCO connection (peer: %s)",
           bt_str(peer_id_));

    auto packet = hci::CommandPacket::New<
        pw::bluetooth::emboss::EnhancedSetupSynchronousConnectionCommandWriter>(
        hci_spec::kEnhancedSetupSynchronousConnection);
    auto view = packet.view_t();
    view.connection_handle().Write(acl_handle_);
    view.connection_parameters().CopyFrom(
        in_progress_request_
            ->parameters[in_progress_request_->current_param_index]
            .view());

    auto status_cb = [self = weak_ptr_factory_.GetWeakPtr()](
                         hci::Result<> status) {
      if (!self.is_alive() || status.is_ok()) {
        return;
      }
      bt_is_error(status, WARN, "sco", "SCO setup connection command failed");
      self->CompleteRequest(fit::error(HostError::kFailed));
    };

    SendCommandWithStatusCallback(std::move(packet), std::move(status_cb));
  }
}

void ScoConnectionManager::CompleteRequestOrTryNextParameters(
    ConnectionResult result) {
  PW_CHECK(in_progress_request_);

  // Multiple parameter attempts are not supported for initiator requests.
  if (result.is_ok() || in_progress_request_->initiator) {
    CompleteRequest(std::move(result));
    return;
  }

  // Check if all accept request parameters have been exhausted.
  if (in_progress_request_->current_param_index + 1 >=
      in_progress_request_->parameters.size()) {
    bt_log(DEBUG, "sco", "all accept SCO parameters exhausted");
    CompleteRequest(fit::error(HostError::kParametersRejected));
    return;
  }

  // If a request was queued after the connection request event (blocking
  // cancelation at that time), cancel the current request.
  if (queued_request_) {
    CompleteRequest(fit::error(HostError::kCanceled));
    return;
  }

  // Wait for the next inbound connection request and accept it with the next
  // parameters.
  in_progress_request_->received_request = false;
  in_progress_request_->current_param_index++;
}

void ScoConnectionManager::CompleteRequest(ConnectionResult result) {
  PW_CHECK(in_progress_request_);
  bt_log(INFO,
         "gap-sco",
         "Completing SCO connection request (initiator: %d, success: %d, peer: "
         "%s)",
         in_progress_request_->initiator,
         result.is_ok(),
         bt_str(peer_id_));
  // Clear in_progress_request_ before calling callback to prevent additional
  // calls to CompleteRequest() during execution of the callback (e.g. due to
  // destroying the RequestHandle).
  ConnectionRequest request = std::move(in_progress_request_.value());
  in_progress_request_.reset();
  request.callback(std::move(result));
  TryCreateNextConnection();
}

void ScoConnectionManager::SendCommandWithStatusCallback(
    hci::CommandPacket command_packet, hci::ResultFunction<> result_cb) {
  hci::CommandChannel::CommandCallback command_cb;
  if (result_cb) {
    command_cb = [cb = std::move(result_cb)](auto,
                                             const hci::EventPacket& event) {
      cb(event.ToResult());
    };
  }
  transport_->command_channel()
      ->SendCommand(std::move(command_packet), std::move(command_cb))
      .IgnoreError();
}

void ScoConnectionManager::SendRejectConnectionCommand(
    DeviceAddressBytes addr, pw::bluetooth::emboss::StatusCode reason) {
  // The reject command has a small range of allowed reasons (the controller
  // sends "Invalid HCI Command Parameters" for other reasons).
  PW_CHECK(
      reason == pw::bluetooth::emboss::StatusCode::
                    CONNECTION_REJECTED_LIMITED_RESOURCES ||
          reason ==
              pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_SECURITY ||
          reason == pw::bluetooth::emboss::StatusCode::
                        CONNECTION_REJECTED_BAD_BD_ADDR,
      "Tried to send invalid reject reason: %s",
      hci_spec::StatusCodeToString(reason).c_str());

  auto reject = hci::CommandPacket::New<
      pw::bluetooth::emboss::RejectSynchronousConnectionRequestCommandWriter>(
      hci_spec::kRejectSynchronousConnectionRequest);
  auto reject_params = reject.view_t();
  reject_params.bd_addr().CopyFrom(addr.view());
  reject_params.reason().Write(reason);

  transport_->command_channel()
      ->SendCommand(std::move(reject),
                    hci::CommandChannel::CommandCallback(nullptr),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

void ScoConnectionManager::CancelRequestWithId(ScoRequestId id) {
  // Cancel queued request if id matches.
  if (queued_request_ && queued_request_->id == id) {
    bt_log(
        INFO, "gap-sco", "Cancelling queued SCO request (id: %" PRIu64 ")", id);
    // Clear queued_request_ before calling callback to prevent calls to
    // CancelRequestWithId() during execution of the callback (e.g. due to
    // destroying the RequestHandle).
    ConnectionRequest request = std::move(queued_request_.value());
    queued_request_.reset();
    request.callback(fit::error(HostError::kCanceled));
    return;
  }

  // Cancel in progress request if it is a responder request that hasn't
  // received a connection request yet.
  if (in_progress_request_ && in_progress_request_->id == id &&
      !in_progress_request_->initiator &&
      !in_progress_request_->received_request) {
    bt_log(INFO,
           "gap-sco",
           "Cancelling in progress SCO request (id: %" PRIu64 ")",
           id);
    CompleteRequest(fit::error(HostError::kCanceled));
  }
}

}  // namespace bt::sco
