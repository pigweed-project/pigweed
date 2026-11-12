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

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_request.h"

namespace bt::gap {

namespace {

const char* const kInspectHasIncomingPropertyName = "has_incoming";
const char* const kInspectCallbacksPropertyName = "callbacks";
const char* const kInspectFirstCreateConnectionReqMadeName =
    "first_create_connection_request_timestamp";
const char* const kInspectPeerIdPropertyName = "peer_id";
constexpr pw::chrono::SystemClock::duration kRetryWindowAfterFirstCreateConn =
    std::chrono::seconds(30);

}  // namespace

BrEdrConnectionRequest::BrEdrConnectionRequest(
    pw::async::Dispatcher& pw_dispatcher,
    const DeviceAddress& addr,
    PeerId peer_id,
    Peer::InitializingConnectionToken token)
    : peer_id_(peer_id),
      address_(addr),
      callbacks_(/*convert=*/[](auto& c) { return c.size(); }),
      peer_init_conn_token_(std::move(token)),
      dispatcher_(pw_dispatcher) {}

BrEdrConnectionRequest::BrEdrConnectionRequest(
    pw::async::Dispatcher& pw_dispatcher,
    const DeviceAddress& addr,
    PeerId peer_id,
    Peer::InitializingConnectionToken token,
    OnComplete&& callback)
    : BrEdrConnectionRequest(pw_dispatcher, addr, peer_id, std::move(token)) {
  callbacks_.Mutable()->push_back(std::move(callback));
}

void BrEdrConnectionRequest::NotifyCallbacks(hci::Result<> status,
                                             const RefFactory& generate_ref) {
  // Clear token before notifying callbacks so that connection state change is
  // reflected in callbacks.
  peer_init_conn_token_.reset();

  // If this request has been moved from, |callbacks_| may be empty.
  for (const auto& callback : *callbacks_) {
    BrEdrConnection* conn_ref = generate_ref();
    if (status.is_ok() && !conn_ref) {
      // An earlier callback destroyed the connection.
      callback(ToResult(HostError::kCanceled), nullptr);
      continue;
    }
    callback(status, conn_ref);
  }
}

void BrEdrConnectionRequest::AttachInspect(inspect::Node& parent,
                                           std::string name) {
  inspect_node_ = parent.CreateChild(name);
  has_incoming_.AttachInspect(inspect_node_, kInspectHasIncomingPropertyName);
  callbacks_.AttachInspect(inspect_node_, kInspectCallbacksPropertyName);
  first_create_connection_req_made_.AttachInspect(
      inspect_node_, kInspectFirstCreateConnectionReqMadeName);
  peer_id_property_ = inspect_node_.CreateString(kInspectPeerIdPropertyName,
                                                 peer_id_.ToString());
}

std::unique_ptr<hci::BrEdrConnectionRequest>
BrEdrConnectionRequest::CreateHciConnectionRequest(
    hci::CommandChannel* command_channel,
    std::optional<uint16_t> clock_offset,
    std::optional<pw::bluetooth::emboss::PageScanRepetitionMode>
        page_scan_repetition_mode,
    OnTimeout timeout_cb,
    OnFailure failure_cb,
    pw::async::Dispatcher& dispatcher) {
  std::unique_ptr<hci::BrEdrConnectionRequest> request =
      std::make_unique<hci::BrEdrConnectionRequest>(
          peer_id_, address_, std::move(timeout_cb), dispatcher);

  request->CreateConnection(command_channel,
                            clock_offset,
                            page_scan_repetition_mode,
                            request_timeout_,
                            std::move(failure_cb));

  // Record that the first create connection attempt was made (if not already
  // attempted) for the outstanding HCI connection request for peer with id
  // |peer_id_|
  if (!first_create_connection_req_made_.value()) {
    first_create_connection_req_made_.Set(dispatcher_.now());
  }

  return request;
}

bool BrEdrConnectionRequest::ShouldRetry(hci::Error failure_mode) {
  pw::chrono::SystemClock::time_point now = dispatcher_.now();
  std::optional<pw::chrono::SystemClock::time_point>
      first_create_conn_req_made = first_create_connection_req_made_.value();
  return failure_mode.is(pw::bluetooth::emboss::StatusCode::PAGE_TIMEOUT) &&
         first_create_conn_req_made.has_value() &&
         now - *first_create_conn_req_made < kRetryWindowAfterFirstCreateConn;
}
}  // namespace bt::gap
