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

#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection_request.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"

namespace bt::hci {

CommandPacket CreateConnectionPacket(
    DeviceAddress address,
    std::optional<pw::bluetooth::emboss::PageScanRepetitionMode>
        page_scan_repetition_mode,
    std::optional<uint16_t> clock_offset) {
  auto request =
      CommandPacket::New<pw::bluetooth::emboss::CreateConnectionCommandWriter>(
          hci_spec::kCreateConnection);
  auto params = request.view_t();
  params.bd_addr().CopyFrom(address.value().view());
  params.packet_type().BackingStorage().WriteUInt(
      hci_spec::kEnableAllPacketTypes);

  // The Page Scan Repetition Mode of the remote device as retrieved by Inquiry.
  // If we do not have one for the device, opt for R2 so we will send for at
  // least 2.56s
  if (page_scan_repetition_mode) {
    params.page_scan_repetition_mode().Write(*page_scan_repetition_mode);
  } else {
    params.page_scan_repetition_mode().Write(
        pw::bluetooth::emboss::PageScanRepetitionMode::R2_);
  }

  params.reserved().Write(0);  // Reserved, must be set to 0.

  // Clock Offset.  The lower 15 bits are set to the clock offset as retrieved
  // by an Inquiry. The highest bit is set to 1 if the rest of this parameter
  // is valid. If we don't have one, use the default.
  if (clock_offset) {
    params.clock_offset().valid().Write(true);
    params.clock_offset().clock_offset().Write(*clock_offset);
  } else {
    params.clock_offset().valid().Write(false);
  }

  params.allow_role_switch().Write(
      pw::bluetooth::emboss::GenericEnableParam::DISABLE);

  return request;
}

void BrEdrConnectionRequest::CreateConnection(
    CommandChannel* command_channel,
    std::optional<uint16_t> clock_offset,
    std::optional<pw::bluetooth::emboss::PageScanRepetitionMode>
        page_scan_repetition_mode,
    pw::chrono::SystemClock::duration timeout,
    OnCompleteDelegate on_command_fail) {
  PW_DCHECK(timeout.count() > 0);

  // HCI Command Status Event will be sent as our completion callback.
  auto self = weak_self_.GetWeakPtr();
  auto complete_cb = [self,
                      timeout,
                      peer_id = peer_id_,
                      on_command_fail_cb = std::move(on_command_fail)](
                         auto, const EventPacket& event) {
    PW_DCHECK(event.event_code() == hci_spec::kCommandStatusEventCode);

    if (!self.is_alive())
      return;

    Result<> status = event.ToResult();
    if (status.is_error()) {
      on_command_fail_cb(status, peer_id);
    } else {
      // Both CommandChannel and the controller perform some scheduling, so log
      // when the controller finally acknowledges Create Connection to observe
      // outgoing connection sequencing.
      // TODO(fxbug.dev/42173957): Added to investigate timing and can be
      // removed if it adds no value
      bt_log(INFO,
             "hci-bredr",
             "Create Connection for peer %s successfully dispatched",
             bt_str(peer_id));

      // The request was started but has not completed; initiate the command
      // timeout period. NOTE: The request will complete when the controller
      // asynchronously notifies us of with a BrEdr Connection Complete event.
      self->timeout_task_.PostAfter(timeout);
    }
  };

  auto packet = CreateConnectionPacket(
      peer_address_, page_scan_repetition_mode, clock_offset);

  bt_log(INFO,
         "hci-bredr",
         "initiating connection request (peer: %s)",
         bt_str(peer_id_));
  command_channel
      ->SendCommand(std::move(packet),
                    std::move(complete_cb),
                    hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

// Status is either a Success or an Error value
Result<> BrEdrConnectionRequest::CompleteRequest(Result<> status) {
  bt_log(INFO,
         "hci-bredr",
         "connection complete (status: %s, peer: %s)",
         bt_str(status),
         bt_str(peer_id_));
  timeout_task_.Cancel();

  if (status.is_error()) {
    if (state_ == RequestState::kTimedOut) {
      return ToResult(HostError::kTimedOut);
    }
    if (status ==
        ToResult(pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID)) {
      // The "Unknown Connection Identifier" error code is returned if this
      // event was sent due to a successful cancellation via the
      // HCI_Create_Connection_Cancel command
      // See Core Spec v5.0 Vol 2, Part E, Section 7.1.7
      state_ = RequestState::kCanceled;
      return ToResult(HostError::kCanceled);
    }
  }
  state_ = RequestState::kSuccess;
  return status;
}

void BrEdrConnectionRequest::Timeout() {
  // If the request was cancelled, this handler will have been removed
  PW_CHECK(state_ == RequestState::kPending);
  bt_log(INFO,
         "hci-bredr",
         "create connection timed out: canceling request (peer: %s)",
         bt_str(peer_id_));
  state_ = RequestState::kTimedOut;
  timeout_task_.Cancel();
}

bool BrEdrConnectionRequest::Cancel() {
  if (state_ == RequestState::kSuccess) {
    bt_log(DEBUG,
           "hci-bredr",
           "connection has already succeeded (peer: %s)",
           bt_str(peer_id_));
    return false;
  }
  if (state_ != RequestState::kPending) {
    bt_log(WARN,
           "hci-bredr",
           "connection attempt already canceled! (peer: %s)",
           bt_str(peer_id_));
    return false;
  }
  // TODO(fxbug.dev/42143836) - We should correctly handle cancels due to a
  // disconnect call during a pending connection creation attempt
  bt_log(INFO,
         "hci-bredr",
         "canceling connection request (peer: %s)",
         bt_str(peer_id_));
  state_ = RequestState::kCanceled;
  timeout_task_.Cancel();
  return true;
}

BrEdrConnectionRequest::~BrEdrConnectionRequest() { Cancel(); }

}  // namespace bt::hci
