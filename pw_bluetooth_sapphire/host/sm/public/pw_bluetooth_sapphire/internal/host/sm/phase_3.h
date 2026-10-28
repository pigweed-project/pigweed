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
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::sm {

// Represents Phase 3 of SM pairing. In this phase, the keys the devices agreed
// to distribute during Phase 1 are exchanged. Phase 3 must take place on an
// already-encrypted link.
//
// THREAD SAFETY
//
// This class is not thread safe and is meant to be accessed on the thread it
// was created on. All callbacks will be run by the default dispatcher of a
// Phase3's creation thread.

using Phase3CompleteCallback = fit::function<void(PairingData)>;

class Phase3 final : public PairingPhase, public PairingChannelHandler {
 public:
  // Initializes Phase3 with the following parameters:
  //   - |chan|: The L2CAP SMP fixed channel.
  //   - |role|: The local device's HCI role.
  //   - |listener|: The current Phase's listener.
  //   - |io_capability|: The local I/O capability.
  //   - |features|: The features that determine pairing, negotiated during
  //   Phase 1. There must be
  //     some keys to distribute if Phase3 exists - construction will panic if
  //     both the local & remote key_distribution fields of features are 0.
  //   - |le_sec|: The current security properties of key encrypting the link.
  //   - |peer_address|: The connection address of the remote peer.
  Phase3(PairingChannel::WeakPtr chan,
         Listener::WeakPtr listener,
         Role role,
         PairingFeatures features,
         SecurityProperties le_sec,
         const DeviceAddress& peer_address,
         Phase3CompleteCallback on_complete);
  ~Phase3() override { InvalidatePairingChannelHandler(); }

  // Performs the key distribution phase of pairing.
  void Start() final;

 private:
  // Called when the Encryption Info (i.e. the LTK) is received from the peer.
  void OnEncryptionInformation(const EncryptionInformationParams& ltk);

  // Called when EDiv and Rand values are received from the peer.
  void OnCentralIdentification(const CentralIdentificationParams& params);

  // Called when the "Identity Resolving Key" is received from the peer.
  void OnIdentityInformation(const IRK& irk);

  // Called when the "Identity Address" is received from the peer.
  void OnIdentityAddressInformation(
      const IdentityAddressInformationParams& params);

  // Called whenever a complete key is received from the peer.
  void OnExpectedKeyReceived();

  // Called to send all agreed upon keys to the peer during Phase 3. Returns
  // false if an error occurs and pairing should be aborted.
  bool SendLocalKeys();

  // Only used during legacy pairing. Returns false if the command cannot be
  // sent.
  bool SendEncryptionKey();

  // Sends the identity information. Returns false if the command cannot be
  // sent.
  bool SendIdentityInfo();

  // Called to collect all pairing keys and notify the callback that we are
  // complete
  void SignalComplete();

  // l2cap::Channel callbacks:
  void OnChannelClosed() final { PairingPhase::HandleChannelClosed(); }
  void OnRxBFrame(ByteBufferPtr sdu) final;

  // True if all keys that are expected from the remote have been received.
  bool RequestedKeysObtained() const;

  // True if all local keys that were agreed to be distributed have been sent to
  // the peer.
  bool LocalKeysSent() const;

  bool KeyExchangeComplete() const {
    return RequestedKeysObtained() && LocalKeysSent();
  }
  bool ShouldReceiveLtk() const;       // True if peer should send the LTK
  bool ShouldReceiveIdentity() const;  // True if peer should send identity
  bool ShouldSendLtk() const;          // True if we should send the LTK
  bool ShouldSendIdentity() const;     // True if we should send identity info

  // PairingPhase override
  std::string ToStringInternal() override {
    return bt_lib_cpp_string::StringPrintf(
        "Pairing Phase 3 (security key distribution) - paired with %s security "
        "properties, sending "
        "0x%02X local key distribution value and expecting 0x%02X as peer key "
        "distribution value",
        le_sec_.ToString().c_str(),
        features_.local_key_distribution,
        features_.remote_key_distribution);
  }

  const PairingFeatures features_;

  // Current security properties of the LE-U link.
  const SecurityProperties le_sec_;

  // Remote peer address used on the active connection.
  const DeviceAddress peer_address_;

  // The remote keys that have been obtained so far.
  KeyDistGenField obtained_remote_keys_;

  // True if all the local keys in features_ have been sent to the peer.
  bool sent_local_keys_;

  // Generated and distributed if the EncKey bit of the local device's
  // KeyDistGenField is set.
  std::optional<LTK> local_ltk_;

  // Data from the peer tracked during Phase 3. Parts of the LTK are received in
  // separate events. The LTK is only received in Legacy pairing.
  std::optional<UInt128> peer_ltk_bytes_;  // LTK without ediv/rand.
  std::optional<LTK> peer_ltk_;            // Full LTK with ediv/rand
  std::optional<IRK> irk_;
  std::optional<DeviceAddress> identity_address_;

  const Phase3CompleteCallback on_complete_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Phase3);
};

}  // namespace bt::sm
