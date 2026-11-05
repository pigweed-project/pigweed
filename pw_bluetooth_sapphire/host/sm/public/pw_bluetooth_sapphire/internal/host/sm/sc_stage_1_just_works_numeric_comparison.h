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
#include <lib/fit/function.h>

#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/sc_stage_1.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"

namespace bt::sm {

// ScStage1JustWorksNumericComparison encapsulates Stage 1 of LE Secure
// Connections Pairing Phase 2, which handles authentication using the Just
// Works or Numeric Comparison methods described in Spec V5.0 Vol. 3 Part
// H 2.3.5.6.2.
//
// This class is not thread safe and is meant to be accessed on the thread it
// was created on. All callbacks will be run by the default dispatcher of a
// Phase2SecureConnections's creation thread.
class ScStage1JustWorksNumericComparison final : public ScStage1 {
 public:
  ScStage1JustWorksNumericComparison(PairingPhase::Listener::WeakPtr listener,
                                     Role role,
                                     UInt256 local_pub_key_x,
                                     UInt256 peer_pub_key_x,
                                     PairingMethod method,
                                     PairingChannel::WeakPtr sm_chan,
                                     Stage1CompleteCallback on_complete);
  void Run() override;
  void OnPairingConfirm(PairingConfirmValue confirm) override;
  void OnPairingRandom(PairingRandomValue rand) override;

 private:
  void SendPairingRandom();

  // Called after all crypto messages have been exchanged
  void CompleteStage1();

  // Invokes |on_complete_| with |result|.
  void NotifyComplete(fit::result<ErrorCode, Output> result);

  PairingPhase::Listener::WeakPtr listener_;
  Role role_;
  UInt256 local_public_key_x_;
  UInt256 peer_public_key_x_;
  PairingMethod method_;

  // In Just Works/Numeric Comparison SC pairing, only the responder sends the
  // Pairing Confirm (see V5.0 Vol. 3 Part H 2.3.5.6.2, Figure 2.3). This member
  // stores the locally generated Pairing Confirm when acting as responder, or
  // the received Pairing Confirm when acting as initiator.
  std::optional<PairingConfirmValue> responder_confirm_;
  bool sent_pairing_confirm_;

  PairingRandomValue local_rand_;
  bool sent_local_rand_;
  // The presence of |peer_rand_| signals if we've received the peer's Pairing
  // Random message.
  std::optional<PairingRandomValue> peer_rand_;

  PairingChannel::WeakPtr sm_chan_;
  Stage1CompleteCallback on_complete_;
  WeakSelf<ScStage1JustWorksNumericComparison> weak_self_;
};
}  // namespace bt::sm
