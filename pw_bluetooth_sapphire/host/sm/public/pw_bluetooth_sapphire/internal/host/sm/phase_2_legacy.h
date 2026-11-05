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

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {

// Phase2Legacy encapsulates Phase 2 of LE Legacy Pairing, which takes care of
// authentication and shared encryption key generation using the Legacy Protocol
// (see V5.1 Vol. 3 Part H Section 2.3.5.2-2.3.5.5)
//
// This class is not thread safe and is meant to be accessed on the thread it
// was created on. All callbacks will be run by the default dispatcher of a
// Phase2Legacy's creation thread.
class Phase2Legacy final : public PairingPhase, public PairingChannelHandler {
 public:
  // |chan|, |listener|, and |role|: used to construct the base PairingPhase
  // |id|: unique id of this pairing instance for differentiating between
  // Pairing Delegate responses |features|: features negotiated in Phase 1 of
  // pairing |preq|, |pres|: Byte representation of Pairing Request/Response
  // exchanged in Phase 1, used for
  //                 for cryptographic hashing
  // |initiator_addr|, |responder_addr|: 48-bit bd-address of the initiator and
  // responder, used
  //                                     used for cryptographic hashing
  // |cb|: Callback that is notified when the Phase2 has negotiated a new
  // encryption key.
  Phase2Legacy(PairingChannel::WeakPtr chan,
               Listener::WeakPtr listener,
               Role role,
               PairingFeatures features,
               const ByteBuffer& preq,
               const ByteBuffer& pres,
               const DeviceAddress& initiator_addr,
               const DeviceAddress& responder_addr,
               OnPhase2KeyGeneratedCallback cb);
  ~Phase2Legacy() override { InvalidatePairingChannelHandler(); }

  // Begin Phase 2 of LE legacy pairing. This is called after LE pairing
  // features are exchanged and results (asynchronously) in the generation and
  // encryption of a link using the STK. Follows (roughly) the following steps:
  //    1. Asynchronously obtain the TK.
  //    2. Generate the local confirm/rand values.
  //    3. If initiator, start the exchange, otherwise wait for the peer to send
  //    its confirm value.
  void Start() final;

 private:
  // Ask the listener for user input to verify the TK used in Legacy pairing.
  // The type of user input requested depends on the PairingMethod in
  // |features_|.
  void MakeTemporaryKeyRequest();
  // Callback passed to request the temporary key which handles the Listener's
  // response.
  void HandleTemporaryKey(std::optional<uint32_t> maybe_tk);

  void SendConfirmValue();
  // Called for SMP commands sent by the peer.
  void OnPairingConfirm(PairingConfirmValue confirm);

  void SendRandomValue();
  // Called for SMP commands sent by the peer.
  void OnPairingRandom(PairingRandomValue rand);

  // Check the preconditions for being able to receive a pairing confirm/random
  // value according to the current state.
  fit::result<ErrorCode> CanReceivePairingConfirm() const;
  fit::result<ErrorCode> CanReceivePairingRandom() const;

  // l2cap::Channel callbacks
  void OnRxBFrame(ByteBufferPtr sdu) final;
  void OnChannelClosed() final { PairingPhase::HandleChannelClosed(); }

  // PairingPhase overrides
  std::string ToStringInternal() override {
    return bt_lib_cpp_string::StringPrintf(
        "Legacy Pairing Phase 2 (encryption key agreement) - pairing with %s "
        "method",
        util::PairingMethodToString(features_.method).c_str());
  }

  bool sent_local_confirm_;
  bool sent_local_rand_;
  std::optional<UInt128> tk_;
  std::optional<UInt128> local_confirm_;
  std::optional<UInt128> peer_confirm_;
  std::optional<UInt128> local_rand_;
  std::optional<UInt128> peer_rand_;
  StaticByteBuffer<util::PacketSize<PairingRequestParams>()> preq_;
  StaticByteBuffer<util::PacketSize<PairingRequestParams>()> pres_;

  const PairingFeatures features_;
  const DeviceAddress initiator_addr_;
  const DeviceAddress responder_addr_;

  OnPhase2KeyGeneratedCallback on_stk_ready_;

  WeakSelf<Phase2Legacy> weak_self_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Phase2Legacy);
};

}  // namespace bt::sm
