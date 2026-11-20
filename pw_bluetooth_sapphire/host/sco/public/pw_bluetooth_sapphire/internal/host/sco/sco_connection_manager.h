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

#pragma once

#include <cinttypes>

#include "pw_async/dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/sco/sco_connection.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::sco {

// ScoConnectionManager handles SCO connections for a single BR/EDR connection.
// This includes queuing outbound and inbound connection requests and handling
// events related to SCO connections.
class ScoConnectionManager final {
 public:
  // Request handle returned to clients. Cancels request when destroyed.
  class RequestHandle final {
   public:
    explicit RequestHandle(fit::callback<void()> on_cancel)
        : on_cancel_(std::move(on_cancel)) {}
    RequestHandle(RequestHandle&&) = default;
    RequestHandle& operator=(RequestHandle&&) = default;
    ~RequestHandle() { Cancel(); }

    void Cancel() {
      if (on_cancel_) {
        on_cancel_();
      }
    }

   private:
    fit::callback<void()> on_cancel_;
    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(RequestHandle);
  };

  // |peer_id| corresponds to the peer associated with this BR/EDR connection.
  // |acl_handle| corresponds to the ACL connection associated with these SCO
  // connections. |transport| must outlive this object.
  ScoConnectionManager(PeerId peer_id,
                       hci_spec::ConnectionHandle acl_handle,
                       DeviceAddress peer_address,
                       DeviceAddress local_address,
                       hci::Transport::WeakPtr transport,
                       pw::async::Dispatcher& dispatcher);
  // Closes connections and cancels connection requests.
  ~ScoConnectionManager();

  // Initiate an outbound connection. A request will be queued if a connection
  // is already in progress. On error, |callback| will be called with an error
  // result. The error will be |kCanceled| if the request was canceled, or
  // |kFailed| if establishing a connection failed. Returns a handle that will
  // cancel the request when dropped. If connection establishment is already in
  // progress when canceled, |callback| will be called immediately with
  // |kCanceled| and the connection will be disconnected once it completes.
  using OpenConnectionResult = fit::result<HostError, ScoConnection::WeakPtr>;
  using OpenConnectionCallback = fit::callback<void(OpenConnectionResult)>;
  RequestHandle OpenConnection(
      bt::StaticPacket<
          pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
          parameters,
      OpenConnectionCallback callback);

  // Accept inbound connection requests using the parameters given in order. The
  // parameters will be tried in order until either a connection is successful,
  // all parameters have been rejected, or the procedure is canceled. On
  // success, |callback| will be called with the connection object and the index
  // of the parameters used to establish the connection. On error, |callback|
  // will be called with an error result. If another Open/Accept request is made
  // before a connection request is received, this request will be canceled
  // (with error |kCanceled|). Returns a handle that will cancel the request
  // when destroyed. If connection establishment is already in progress when
  // canceled, |callback| will be called immediately with |kCanceled| and the
  // connection will be disconnected once it completes.
  using AcceptConnectionResult = fit::result<
      HostError,
      std::pair<ScoConnection::WeakPtr, size_t /*index of parameters used*/>>;
  using AcceptConnectionCallback = fit::callback<void(AcceptConnectionResult)>;
  RequestHandle AcceptConnection(
      std::vector<bt::StaticPacket<
          pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
          parameters,
      AcceptConnectionCallback callback);

 private:
  using ScoRequestId = uint64_t;
  using ConnectionResult = fit::result<
      HostError,
      std::pair<ScoConnection::WeakPtr, size_t /*index of parameters used*/>>;
  using ConnectionCallback = fit::callback<void(ConnectionResult)>;

  class ConnectionRequest final {
   public:
    ConnectionRequest(
        ScoRequestId id_arg,
        bool initiator_arg,
        bool received_request_arg,
        std::vector<bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
            parameters_arg,
        ConnectionCallback callback_arg)
        : id(id_arg),
          initiator(initiator_arg),
          received_request(received_request_arg),
          parameters(std::move(parameters_arg)),
          callback(std::move(callback_arg)) {}
    ConnectionRequest(ConnectionRequest&&) = default;
    ConnectionRequest& operator=(ConnectionRequest&&) = default;
    ~ConnectionRequest() {
      if (callback) {
        bt_log(DEBUG,
               "sco",
               "Cancelling SCO connection request (id: %" PRIu64 ")",
               id);
        callback(fit::error(HostError::kCanceled));
      }
    }

    bool canceled() const { return !callback; }

    ScoRequestId id;
    bool initiator;
    bool received_request;
    size_t current_param_index = 0;
    std::vector<bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
        parameters;
    // Set to nullptr once invoked, which also serves as the sentinel for a
    // request canceled while awaiting a controller response.
    ConnectionCallback callback;
  };

  hci::CommandChannel::EventHandlerId AddEventHandler(
      const hci_spec::EventCode& code,
      hci::CommandChannel::EventCallback event_callback);

  // Event handlers:
  hci::CommandChannel::EventCallbackResult OnSynchronousConnectionComplete(
      const hci::EventPacket& event);
  hci::CommandChannel::EventCallbackResult OnConnectionRequest(
      const hci::EventPacket& event);

  // Returns true if parameters matching the corresponding transport were found
  // in the current request, or false otherwise. Mutates the current request's
  // parameter index to that of the matching parameters (or past the end on
  // failure).
  bool FindNextParametersThatSupportSco();
  bool FindNextParametersThatSupportEsco();

  ScoConnectionManager::RequestHandle QueueRequest(
      bool initiator,
      std::vector<bt::StaticPacket<
          pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>,
      ConnectionCallback);

  void TryCreateNextConnection();

  void CompleteRequestOrTryNextParameters(ConnectionResult);

  void CompleteRequest(ConnectionResult);

  void SendCommandWithStatusCallback(hci::CommandPacket command_packet,
                                     hci::ResultFunction<> cb);

  void SendRejectConnectionCommand(DeviceAddressBytes addr,
                                   pw::bluetooth::emboss::StatusCode reason);

  // If either the queued or in progress request has the given id, cancel it.
  // Called when a RequestHandle is dropped.
  void CancelRequestWithId(ScoRequestId);

  // The id that should be associated with the next request. Incremented when
  // the current value is used.
  ScoRequestId next_req_id_;

  // If a request is made while in_progress_request_ is waiting for a complete
  // event, it gets queued in queued_request_.
  std::optional<ConnectionRequest> queued_request_;

  std::optional<ConnectionRequest> in_progress_request_;

  // Holds active connections.
  std::unordered_map<hci_spec::ConnectionHandle, std::unique_ptr<ScoConnection>>
      connections_;

  // Handler IDs for registered events
  std::vector<hci::CommandChannel::EventHandlerId> event_handler_ids_;

  PeerId peer_id_;

  const DeviceAddress local_address_;
  const DeviceAddress peer_address_;

  hci_spec::ConnectionHandle acl_handle_;

  hci::Transport::WeakPtr transport_;
  pw::async::Dispatcher& dispatcher_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<ScoConnectionManager> weak_ptr_factory_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ScoConnectionManager);
};
}  // namespace bt::sco
