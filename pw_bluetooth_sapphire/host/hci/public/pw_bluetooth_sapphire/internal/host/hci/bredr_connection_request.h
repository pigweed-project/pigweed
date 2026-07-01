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
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt::hci {

// The request can be in three possible states:
enum class RequestState : uint8_t {
  // The connection request is still pending
  kPending,
  // The connection request was intentionally cancelled
  kCanceled,
  // The connection request timed out whilst waiting for a response
  kTimedOut,
  // The connection request has succeeded
  kSuccess,
};

// This class represents a pending request by the BrEdr connector to initiate an
// outgoing connection. It tracks the state of that request and is responsible
// for running a call back when the connection status updates
//
// There should be only one of these at any given time, an it is managed by the
// BrEdrConnectionManager
class BrEdrConnectionRequest final {
 public:
  using OnCompleteDelegate = fit::function<void(Result<>, PeerId)>;

  BrEdrConnectionRequest(PeerId id,
                         DeviceAddress addr,
                         fit::closure timeout_callback,
                         pw::async::Dispatcher& dispatcher)
      : state_(RequestState::kPending),
        peer_id_(id),
        peer_address_(addr),
        timeout_task_(dispatcher),
        weak_self_(this) {
    timeout_task_.set_function(
        [timeout_cb = std::move(timeout_callback)](pw::async::Context /*ctx*/,
                                                   pw::Status status) {
          if (status.ok()) {
            timeout_cb();
          }
        });
  }

  ~BrEdrConnectionRequest();

  // Send the CreateConnection command over |command_channel| and begin the
  // create connection procedure. If the command status returns an error, then
  // |on_command_fail| called. The |clock_offset| and |page_scan_repetition|
  // parameters are standard parameters found in Core Spec 5.0, Vol 2, Part E,
  // section 7.1.5
  // |timeout| is the command timeout; this is how long we give from the point
  // we receive the CommandStatus response from the controller until we cancel
  // the procedure if we have not received ConnectionComplete
  void CreateConnection(
      CommandChannel* command_channel,
      std::optional<uint16_t> clock_offset,
      std::optional<pw::bluetooth::emboss::PageScanRepetitionMode>
          page_scan_repetition_mode,
      pw::chrono::SystemClock::duration timeout,
      OnCompleteDelegate on_command_fail);

  PeerId peer_id() const { return peer_id_; }
  DeviceAddress peer_address() const { return peer_address_; }

  // Complete the request, either successfully or not, and return the status
  // of the Request - In the case of Timeout or Cancellation, this will be
  // different from the status sent by the controller.
  Result<> CompleteRequest(Result<> status);

  // Mark the request as Timed out; triggered when the command timeout runs out
  // and called by BrEdrConnectionManager;
  void Timeout();

  // Attempt to mark the request as Canceled, and returns true if successful.
  // This is called during cleanup to ensure connection procedures are not
  // orphaned
  bool Cancel();

 private:
  RequestState state_;
  PeerId peer_id_;
  DeviceAddress peer_address_;

  SmartTask timeout_task_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<BrEdrConnectionRequest> weak_self_;
};

}  // namespace bt::hci
