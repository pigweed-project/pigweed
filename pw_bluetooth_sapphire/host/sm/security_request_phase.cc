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

#include "pw_bluetooth_sapphire/internal/host/sm/security_request_phase.h"

#include <pw_assert/check.h>

#include <type_traits>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {

SecurityRequestPhase::SecurityRequestPhase(
    PairingChannel::WeakPtr chan,
    Listener::WeakPtr listener,
    SecurityLevel desired_level,
    BondableMode bondable_mode,
    PairingRequestCallback on_pairing_req)
    : PairingPhase(std::move(chan), std::move(listener), Role::kResponder),
      bondable_mode_(bondable_mode),
      pending_security_request_(desired_level),
      on_pairing_req_(std::move(on_pairing_req)),
      weak_self_(this) {
  SetPairingChannelHandler(*this);
}

void SecurityRequestPhase::Start() {
  MakeSecurityRequest(pending_security_request_, bondable_mode_);
}

void SecurityRequestPhase::MakeSecurityRequest(SecurityLevel desired_level,
                                               BondableMode bondable_mode) {
  PW_CHECK(desired_level >= SecurityLevel::kEncrypted);
  AuthReqField security_req_payload = 0u;
  if (desired_level >= SecurityLevel::kAuthenticated) {
    // inclusive-language: ignore
    security_req_payload |= AuthReq::kMITM;
  }
  if (bondable_mode == BondableMode::Bondable) {
    security_req_payload |= AuthReq::kBondingFlag;
  }
  if (desired_level == SecurityLevel::kSecureAuthenticated) {
    security_req_payload |= AuthReq::kSC;
  }
  pending_security_request_ = desired_level;
  sm_chan().SendMessage(kSecurityRequest, security_req_payload);
}

void SecurityRequestPhase::OnPairingRequest(PairingRequestParams req_params) {
  WeakSelf<SecurityRequestPhase>::WeakPtr self = weak_self_.GetWeakPtr();
  PairingRequestCallback callback = std::move(on_pairing_req_);
  PW_CHECK(callback);
  callback(req_params);
  if (self.is_alive()) {
    on_pairing_req_ = std::move(callback);
  }
}

void SecurityRequestPhase::OnRxBFrame(ByteBufferPtr sdu) {
  fit::result<ErrorCode, ValidPacketReader> maybe_reader =
      ValidPacketReader::ParseSdu(sdu);
  if (maybe_reader.is_error()) {
    Abort(maybe_reader.error_value());
    return;
  }
  ValidPacketReader reader = maybe_reader.value();
  Code smp_code = reader.code();

  if (smp_code == kPairingRequest) {
    OnPairingRequest(reader.payload<PairingRequestParams>());
  } else {
    bt_log(DEBUG,
           "sm",
           "received unexpected code %#.2X with pending Security Request",
           smp_code);
    Abort(ErrorCode::kUnspecifiedReason);
  }
}

}  // namespace bt::sm
