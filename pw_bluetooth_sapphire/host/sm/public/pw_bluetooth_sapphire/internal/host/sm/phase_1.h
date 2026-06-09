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

#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {
// Responsible for Phase 1 of SMP pairing, the feature exchange. Takes in the
// current SM settings and notifies a constructor-provided callback with the
// negotiated features upon completion.
//
// This class is not thread safe and is meant to be accessed on the thread it
// was created on. All callbacks will be run by the default dispatcher of a
// Phase1's creation thread.
class Phase1 final : public PairingPhase, public PairingChannelHandler {
 public:
  // Called when Phase 1 completes successfully. |features| are the negotiated
  // features. |preq| and |pres| are the SMP "Pairing Request" and "Pairing
  // Response" payloads exchanged by the devices, which are returned for use in
  // Phase 2 crypto calculations.
  using CompleteCallback = fit::function<void(PairingFeatures features,
                                              PairingRequestParams preq,
                                              PairingResponseParams pres)>;

  // Returns a Phase 1 that starts pairing to |requested_level|. Note the lack
  // of a `preq` parameter: Phase 1 builds & sends the Pairing Request as
  // initiator.
  static std::unique_ptr<Phase1> CreatePhase1Initiator(
      PairingChannel::WeakPtr chan,
      Listener::WeakPtr listener,
      IOCapability io_capability,
      BondableMode bondable_mode,
      SecurityLevel requested_level,
      CompleteCallback on_complete);

  // Returns a Phase 1 in the responder role. Note the `preq` parameter: Phase 1
  // is supplied the Pairing Request from the remote as responder. See private
  // ctor for parameter descriptions.
  static std::unique_ptr<Phase1> CreatePhase1Responder(
      PairingChannel::WeakPtr chan,
      Listener::WeakPtr listener,
      PairingRequestParams preq,
      IOCapability io_capability,
      BondableMode bondable_mode,
      SecurityLevel requested_level,
      CompleteCallback on_complete);

  ~Phase1() override { InvalidatePairingChannelHandler(); }

  // Runs the Phase 1 state machine, performing the feature exchange.
  void Start() final;

 private:
  //   |chan|, |listener|, and |role|: used to construct the base PairingPhase
  //   |preq|: If empty, the device is in the initiator role and starts the
  //   pairing.
  //           If present, the device is in the responder role, and will respond
  //           to |preq|, the peer initiator's pairing request.
  //   |bondable_mode|: the bondable mode of the local device (see V5.1 Vol. 3
  //   Part C Section 9.4). |requested_level|: The minimum security level
  //   required by the local device for this pairing.
  //                      Must be at least SecurityLevel::kEncrypted. If
  //                      authenticated, |io_capability| must support
  //                      authenticated pairing. Otherwise, pairing fails.
  //   |on_complete|: called at the end of Phase 1 with the resulting features.
  Phase1(PairingChannel::WeakPtr chan,
         Listener::WeakPtr listener,
         Role role,
         std::optional<PairingRequestParams> preq,
         IOCapability io_capability,
         BondableMode bondable_mode,
         SecurityLevel requested_level,
         CompleteCallback on_complete);

  // Start the feature exchange by sending the Pairing Request. The local device
  // must be in the SMP initiator role (V5.1 Vol 3, Part H, 2.3).
  void InitiateFeatureExchange();

  // Handle the peer-initiated feature exchange. The local device must be in the
  // SMP responder role (V5.1 Vol 3, Part H, 2.3).
  void RespondToPairingRequest(const PairingRequestParams& req_params);

  // The returned `LocalPairingParams` contains the locally-preferred pairing
  // parameters for the LE transport. The caller is responsible for making any
  // adjustments necessary (e.g. due to peer preferences) before converting the
  // `LocalPairingParams` to SMP Pairing(Response|Request) PDUs.
  LocalPairingParams BuildPairingParameters();

  // Called to complete a feature exchange. Returns the resulting
  // PairingFeatures if the parameters should be accepted, or an error code if
  // the parameters are rejected and pairing should abort.
  fit::result<ErrorCode, PairingFeatures> ResolveFeatures(
      bool local_initiator,
      const PairingRequestParams& preq,
      const PairingResponseParams& pres);

  // Called for SMP commands sent by the peer.
  void OnPairingResponse(const PairingResponseParams& response_params);

  // PairingChannelHandler callbacks:
  void OnRxBFrame(ByteBufferPtr sdu) final;
  void OnChannelClosed() final { PairingPhase::HandleChannelClosed(); }

  // PairingPhase overrides
  std::string ToStringInternal() override {
    return bt_lib_cpp_string::StringPrintf(
        "Pairing Phase 1 (feature exchange) - pairing to %s security with "
        "\"%s\" IOCapabilities",
        LevelToString(requested_level_),
        util::IOCapabilityToString(io_capability_).c_str());
  }

  // If acting as the Responder to a peer-initiatied pairing, then |preq_| is
  // the |preq| ctor parameter. Otherwise, these are generated and exchanged
  // during Phase 1.
  std::optional<PairingRequestParams> preq_;
  std::optional<PairingResponseParams> pres_;

  // Phase 1 ensures that the feature exchange results in a pairing that
  // supports this level of security. If this is impossible, pairing will abort.
  SecurityLevel requested_level_;

  // Local feature flags that determine the feature exchange negotiation with
  // the peer.
  bool oob_available_;
  IOCapability io_capability_;
  BondableMode bondable_mode_;

  CompleteCallback on_complete_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Phase1);
};

}  // namespace bt::sm
