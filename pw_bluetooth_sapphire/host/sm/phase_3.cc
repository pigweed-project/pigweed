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

#include "pw_bluetooth_sapphire/internal/host/sm/phase_3.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <optional>
#include <type_traits>

#include "lib/fit/function.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {

Phase3::Phase3(PairingChannel::WeakPtr chan,
               Listener::WeakPtr listener,
               Role role,
               PairingFeatures features,
               SecurityProperties le_sec,
               const DeviceAddress& peer_address,
               Phase3CompleteCallback on_complete)
    : PairingPhase(std::move(chan), std::move(listener), role),
      features_(features),
      le_sec_(le_sec),
      peer_address_(peer_address),
      obtained_remote_keys_(0),
      sent_local_keys_(false),
      on_complete_(std::move(on_complete)) {
  // LTKs may not be distributed during Secure Connections.
  PW_CHECK(!(features_.secure_connections &&
             (ShouldSendLtk() || ShouldReceiveLtk())),
           "Phase 3 may not distribute the LTK in Secure Connections pairing");

  PW_CHECK(
      HasKeysToDistribute(features_, sm_chan().link_type() == LinkType::kACL));
  // The link must be encrypted with at least an STK in order for Phase 3 to
  // take place.
  PW_CHECK(le_sec.level() != SecurityLevel::kNoSecurity);
  SetPairingChannelHandler(*this);
}

void Phase3::Start() {
  PW_CHECK(!has_failed());
  PW_CHECK(!KeyExchangeComplete());

  if (role() == Role::kInitiator && !RequestedKeysObtained()) {
    bt_log(DEBUG, "sm", "waiting to receive keys from the responder");
    return;
  }

  if (!LocalKeysSent() && !SendLocalKeys()) {
    bt_log(WARN, "sm", "unable to send local keys");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }
  // If there are no keys left to exchange then we're done with pairing. This
  // cannot be an else branch because the above call to SendLocalKeys could've
  // completed pairing.
  if (KeyExchangeComplete()) {
    SignalComplete();
  }
}

void Phase3::OnEncryptionInformation(const EncryptionInformationParams& ltk) {
  // Only allowed on the LE transport.
  if (sm_chan().link_type() != bt::LinkType::kLE) {
    bt_log(
        DEBUG, "sm", "\"Encryption Information\" over BR/EDR not supported!");
    Abort(ErrorCode::kCommandNotSupported);
    return;
  }

  if (!ShouldReceiveLtk()) {
    bt_log(WARN, "sm", "received unexpected LTK");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // abort pairing if we received a second LTK from the peer.
  if (peer_ltk_bytes_.has_value() || peer_ltk_.has_value()) {
    bt_log(WARN, "sm", "already received LTK! aborting");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Abort pairing if the LTK is the sample LTK from the core spec
  if (ltk == kSpecSampleLtk) {
    bt_log(WARN, "sm", "LTK is sample from spec, not secure! aborting");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Check that the received key has 0s at all locations more significant than
  // negotiated key_size
  uint8_t key_size = features_.encryption_key_size;
  PW_DCHECK(key_size <= ltk.size());
  for (auto i = key_size; i < ltk.size(); i++) {
    if (ltk[i] != 0) {
      bt_log(WARN, "sm", "received LTK is larger than max keysize! aborting");
      Abort(ErrorCode::kInvalidParameters);
      return;
    }
  }

  PW_DCHECK(!(obtained_remote_keys_ & KeyDistGen::kEncKey));
  peer_ltk_bytes_ = ltk;

  // Wait to receive EDiv and Rand
}

void Phase3::OnCentralIdentification(
    const CentralIdentificationParams& params) {
  // Only allowed on the LE transport.
  if (sm_chan().link_type() != bt::LinkType::kLE) {
    bt_log(
        DEBUG, "sm", "\"Central Identification\" over BR/EDR not supported!");
    Abort(ErrorCode::kCommandNotSupported);
    return;
  }
  uint16_t ediv =
      pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.ediv);
  uint64_t random =
      pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.rand);

  if (!ShouldReceiveLtk()) {
    bt_log(WARN, "sm", "received unexpected ediv/rand");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // EDIV and Rand must be sent AFTER the LTK (Vol 3, Part H, 3.6.1).
  if (!peer_ltk_bytes_.has_value()) {
    bt_log(WARN, "sm", "received EDIV and Rand before LTK!");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  if (obtained_remote_keys_ & KeyDistGen::kEncKey) {
    bt_log(WARN, "sm", "already received EDIV and Rand!");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Abort pairing if the Rand is the sample Rand from the core spec
  if (random == kSpecSampleRandom) {
    bt_log(WARN, "sm", "random is sample from core spec, not secure! aborting");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // The security properties of the LTK are determined by the current link
  // properties (i.e. the properties of the STK).
  peer_ltk_ = LTK(le_sec_, hci_spec::LinkKey(*peer_ltk_bytes_, random, ediv));
  obtained_remote_keys_ |= KeyDistGen::kEncKey;

  // "EncKey" received. Complete pairing if possible.
  OnExpectedKeyReceived();
}

void Phase3::OnIdentityInformation(const IRK& irk) {
  if (!ShouldReceiveIdentity()) {
    bt_log(WARN, "sm", "received unexpected IRK");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // Abort if we receive an IRK more than once.
  if (irk_.has_value()) {
    bt_log(WARN, "sm", "already received IRK! aborting");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  PW_DCHECK(!(obtained_remote_keys_ & KeyDistGen::kIdKey));
  irk_ = irk;

  // Wait to receive identity address
}

void Phase3::OnIdentityAddressInformation(
    const IdentityAddressInformationParams& params) {
  if (!ShouldReceiveIdentity()) {
    bt_log(WARN, "sm", "received unexpected identity address");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  // The identity address must be sent after the IRK (Vol 3, Part H, 3.6.1).
  if (!irk_.has_value()) {
    bt_log(WARN, "sm", "received identity address before the IRK!");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  if (obtained_remote_keys_ & KeyDistGen::kIdKey) {
    bt_log(WARN, "sm", "already received identity information!");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }
  DeviceAddress::Type type = DeviceAddress::Type::kLERandom;
  if (params.type != AddressType::kStaticRandom) {
    if (params.type != AddressType::kPublic) {
      bt_log(WARN,
             "sm",
             "received invalid bt address type: %d",
             static_cast<int>(params.type));
      Abort(ErrorCode::kInvalidParameters);
      return;
    }
    type = DeviceAddress::Type::kLEPublic;
  }

  const DeviceAddress identity_address(type, params.bd_addr);
  // Core Spec v6.0, Vol 6, Part B, Section 1.3.2.1: The two most significant
  // bits of a static address shall be equal to 1.
  if (params.type == AddressType::kStaticRandom &&
      !identity_address.IsStaticRandom()) {
    bt_log(WARN,
           "sm",
           "received non-static random identity address: %s",
           bt_str(identity_address));
    Abort(ErrorCode::kInvalidParameters);
    return;
  }

  // Core Spec v6.0, Vol 3, Part H, Section 3.6.5 and Vol 6, Part B,
  // Section 1.3.2.3: Over LE, the distributed identity address must match the
  // peer's public/static connection address or correspond to an RPA resolved by
  // the distributed IRK.
  if (sm_chan().link_type() == bt::LinkType::kLE) {
    const bool matches_direct_identity =
        (peer_address_.IsPublic() || peer_address_.IsStaticRandom()) &&
        (identity_address == peer_address_);
    const bool resolves_rpa = peer_address_.IsResolvablePrivate() &&
                              util::IrkCanResolveRpa(*irk_, peer_address_);
    if (!matches_direct_identity && !resolves_rpa) {
      bt_log(WARN,
             "sm",
             "received unverified identity address %s for LE peer %s",
             bt_str(identity_address),
             bt_str(peer_address_));
      Abort(ErrorCode::kInvalidParameters);
      return;
    }
  }

  // Store the identity address and mark all identity info as received.
  identity_address_ = identity_address;
  obtained_remote_keys_ |= KeyDistGen::kIdKey;

  // "IdKey" received. Complete pairing if possible.
  OnExpectedKeyReceived();
}

void Phase3::OnExpectedKeyReceived() {
  if (!RequestedKeysObtained()) {
    bt_log(DEBUG, "sm", "received one expected key, more keys pending");
    return;
  }

  if (role() == Role::kInitiator && !LocalKeysSent() && !SendLocalKeys()) {
    bt_log(WARN, "sm", "unable to send local keys to peer");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }
  // If we've received all the expected keys and sent the local keys, Phase 3 is
  // complete
  SignalComplete();
}

bool Phase3::SendLocalKeys() {
  PW_DCHECK(!LocalKeysSent());

  if (ShouldSendLtk() && !SendEncryptionKey()) {
    return false;
  }

  if (ShouldSendIdentity() && !SendIdentityInfo()) {
    return false;
  }

  sent_local_keys_ = true;
  return true;
}

bool Phase3::SendEncryptionKey() {
  PW_CHECK(!features_.secure_connections);

  // Only allowed on the LE transport.
  if (sm_chan().link_type() != bt::LinkType::kLE) {
    return false;
  }

  // Generate a completely random value for LTK.
  UInt128 ltk_bytes = Random<UInt128>();

  // Mask the ltk down to the maximum encryption key size.
  uint8_t key_size = features_.encryption_key_size;
  if (key_size < 16) {
    MutableBufferView view(ltk_bytes.data() + key_size, 16 - key_size);
    view.SetToZeros();
  }

  // Generate completely random values for EDiv and Rand, use masked LTK.
  // The LTK inherits the security properties of the STK currently encrypting
  // the link.
  local_ltk_ =
      LTK(le_sec_,
          hci_spec::LinkKey(ltk_bytes, Random<uint64_t>(), Random<uint16_t>()));

  // Send LTK
  sm_chan().SendMessage(kEncryptionInformation, local_ltk_->key().value());
  // Send EDiv & Rand
  sm_chan().SendMessage(
      kCentralIdentification,
      CentralIdentificationParams{
          .ediv = pw::bytes::ConvertOrderTo(cpp20::endian::little,
                                            local_ltk_->key().ediv()),
          .rand = pw::bytes::ConvertOrderTo(cpp20::endian::little,
                                            local_ltk_->key().rand())});

  return true;
}

bool Phase3::SendIdentityInfo() {
  std::optional<IdentityInfo> maybe_id_info = listener()->OnIdentityRequest();
  if (!maybe_id_info.has_value()) {
    bt_log(DEBUG,
           "sm",
           "local identity information required but no longer "
           "available; abort pairing");
    return false;
  }
  auto id_info = *maybe_id_info;

  if (!id_info.address.IsStaticRandom() && !id_info.address.IsPublic()) {
    bt_log(DEBUG, "sm", "identity address must be public or static random!");
    return false;
  }

  // Send IRK
  sm_chan().SendMessage(kIdentityInformation, id_info.irk);
  // Send identity address
  sm_chan().SendMessage(
      kIdentityAddressInformation,
      IdentityAddressInformationParams{
          .type = (id_info.address.IsStaticRandom() ? AddressType::kStaticRandom
                                                    : AddressType::kPublic),
          .bd_addr = id_info.address.value()});

  return true;
}

void Phase3::SignalComplete() {
  PW_CHECK(KeyExchangeComplete());

  // The security properties of all keys are determined by the security
  // properties of the link used to distribute them. This is already reflected
  // by |le_sec_|.
  PairingData pairing_data;
  pairing_data.peer_ltk = peer_ltk_;
  pairing_data.local_ltk = local_ltk_;

  if (irk_.has_value()) {
    // If there is an IRK there must also be an identity address.
    PW_CHECK(identity_address_.has_value());
    pairing_data.irk = Key(le_sec_, *irk_);
    pairing_data.identity_address = identity_address_;
  }
  on_complete_(pairing_data);
}

void Phase3::OnRxBFrame(ByteBufferPtr sdu) {
  auto maybe_reader = ValidPacketReader::ParseSdu(sdu);
  if (maybe_reader.is_error()) {
    Abort(maybe_reader.error_value());
    return;
  }
  ValidPacketReader reader = maybe_reader.value();
  Code smp_code = reader.code();

  if (smp_code != kPairingFailed &&
      sm_chan().link_type() == bt::LinkType::kACL) {
    PW_CHECK(listener().is_alive());
    if (!listener()->IsBrEdrCrossTransportKeyDerivationAllowed()) {
      bt_log(WARN,
             "sm",
             "aborting BR/EDR CTKD in Phase 3: CTKD is no longer allowed");
      Abort(ErrorCode::kCrossTransportKeyDerivationNotAllowed);
      return;
    }
  }

  switch (smp_code) {
    case kPairingFailed:
      OnFailure(Error(reader.payload<ErrorCode>()));
      break;
    case kEncryptionInformation:
      OnEncryptionInformation(reader.payload<EncryptionInformationParams>());
      break;
    case kCentralIdentification:
      OnCentralIdentification(reader.payload<CentralIdentificationParams>());
      break;
    case kIdentityInformation:
      OnIdentityInformation(reader.payload<IRK>());
      break;
    case kIdentityAddressInformation:
      OnIdentityAddressInformation(
          reader.payload<IdentityAddressInformationParams>());
      break;
    default:
      bt_log(INFO,
             "sm",
             "received unexpected code %d when in Pairing Phase 3",
             smp_code);
      Abort(ErrorCode::kUnspecifiedReason);
  }
}

bool Phase3::RequestedKeysObtained() const {
  // DistributableKeys masks key fields that don't correspond to keys exchanged
  // in Phase 3.
  const KeyDistGenField kMaskedRemoteKeys =
      DistributableKeys(features_.remote_key_distribution,
                        sm_chan().link_type() == LinkType::kACL);
  // Return true if we expect no keys from the remote. We keep track of received
  // keys individually in `obtained_remote_keys` as they are received
  // asynchronously from the peer.
  return !kMaskedRemoteKeys || (kMaskedRemoteKeys == obtained_remote_keys_);
}

bool Phase3::LocalKeysSent() const {
  // DistributableKeys masks key fields that don't correspond to keys exchanged
  // in Phase 3.
  const KeyDistGenField kMaskedLocalKeys =
      DistributableKeys(features_.local_key_distribution,
                        sm_chan().link_type() == LinkType::kACL);
  // Return true if we didn't agree to send any keys. We need only store a
  // boolean to track whether we've sent the keys, as sending the keys to the
  // peer occurs sequentially.
  return !kMaskedLocalKeys || sent_local_keys_;
}

bool Phase3::ShouldReceiveLtk() const {
  return static_cast<bool>(features_.remote_key_distribution &
                           KeyDistGen::kEncKey) &&
         sm_chan().link_type() == bt::LinkType::kLE;
}

bool Phase3::ShouldReceiveIdentity() const {
  return (features_.remote_key_distribution & KeyDistGen::kIdKey);
}

bool Phase3::ShouldSendLtk() const {
  return static_cast<bool>(features_.local_key_distribution &
                           KeyDistGen::kEncKey) &&
         sm_chan().link_type() == bt::LinkType::kLE;
}

bool Phase3::ShouldSendIdentity() const {
  return (features_.local_key_distribution & KeyDistGen::kIdKey);
}

}  // namespace bt::sm
