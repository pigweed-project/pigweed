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

#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/sc_stage_1.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"

namespace bt::sm {
// ScStage1Passkey encapsulates Stage 1 of LE Secure Connections Pairing Phase
// 2, which takes care of authentication using the Passkey Entry method as in
// V5.0 Vol. 3 Part H 2.3.5.6.3.
//
// This class is not thread safe and is meant to be accessed on the thread it
// was created on. All callbacks will be run by the default dispatcher of a
// ScStage1Passkey's creation thread.
class ScStage1Passkey final : public ScStage1 {
 public:
  ScStage1Passkey(PairingPhase::Listener::WeakPtr listener,
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
  // For SC passkey entry, each bit of the passkey is exchanged and confirmed
  // one-by-one. Calling this function clears variables from the previous
  // exchange (if necessary) and starts the next.
  void StartBitExchange();

  // Functions that handle sending/receiving SMP messages in the exchange of a
  // bit of the passkey.
  void SendPairingConfirm();
  void SendPairingRandom();

  // Called after SMP messages have been exchanged & verified for a given bit of
  // the passkey. Ends stage 1 after all bits are exchanged, or calls
  // StartBitExchange if there are still more bits.
  void FinishBitExchange();

  // Invokes |on_complete_| with |result|.
  void NotifyComplete(fit::result<ErrorCode, Output> result);

  PairingPhase::Listener::WeakPtr listener_;
  Role role_;
  UInt256 local_public_key_x_;
  UInt256 peer_public_key_x_;
  PairingMethod method_;

  // This member is set when the PairingPhase::Listener request for the Passkey
  // completes.
  std::optional<uint32_t> passkey_;
  size_t passkey_bit_location_;

  PairingConfirmValue local_confirm_;
  bool sent_local_confirm_;
  // The presence of |peer_confirm_| signals if we've received the peer's
  // Pairing Random message.
  std::optional<PairingConfirmValue> peer_confirm_;

  PairingRandomValue local_rand_;
  bool sent_local_rand_;
  // The presence of |peer_rand_| signals if we've received the peer's Pairing
  // Random message.
  std::optional<PairingRandomValue> peer_rand_;

  PairingChannel::WeakPtr sm_chan_;
  Stage1CompleteCallback on_complete_;
  WeakSelf<ScStage1Passkey> weak_self_;
};
}  // namespace bt::sm
