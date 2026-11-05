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

// inclusive-language: disable

#include "pw_bluetooth_sapphire/internal/host/sm/phase_1.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/scoped_channel.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {

std::unique_ptr<Phase1> Phase1::CreatePhase1Initiator(
    PairingChannel::WeakPtr chan,
    Listener::WeakPtr listener,
    IOCapability io_capability,
    BondableMode bondable_mode,
    SecurityLevel requested_level,
    CompleteCallback on_complete) {
  // Use `new` & unique_ptr constructor here instead of `std::make_unique`
  // because the private Phase1 constructor prevents std::make_unique from
  // working (https://abseil.io/tips/134).
  return std::unique_ptr<Phase1>(new Phase1(std::move(chan),
                                            std::move(listener),
                                            Role::kInitiator,
                                            std::nullopt,
                                            io_capability,
                                            bondable_mode,
                                            requested_level,
                                            std::move(on_complete)));
}

std::unique_ptr<Phase1> Phase1::CreatePhase1Responder(
    PairingChannel::WeakPtr chan,
    Listener::WeakPtr listener,
    PairingRequestParams preq,
    IOCapability io_capability,
    BondableMode bondable_mode,
    SecurityLevel minimum_allowed_level,
    CompleteCallback on_complete) {
  // Use `new` & unique_ptr constructor here instead of `std::make_unique`
  // because the private Phase1 constructor prevents std::make_unique from
  // working (https://abseil.io/tips/134).
  return std::unique_ptr<Phase1>(new Phase1(std::move(chan),
                                            std::move(listener),
                                            Role::kResponder,
                                            preq,
                                            io_capability,
                                            bondable_mode,
                                            minimum_allowed_level,
                                            std::move(on_complete)));
}

Phase1::Phase1(PairingChannel::WeakPtr chan,
               Listener::WeakPtr listener,
               Role role,
               std::optional<PairingRequestParams> preq,
               IOCapability io_capability,
               BondableMode bondable_mode,
               SecurityLevel requested_level,
               CompleteCallback on_complete)
    : PairingPhase(std::move(chan), std::move(listener), role),
      preq_(preq),
      requested_level_(requested_level),
      oob_available_(false),
      io_capability_(io_capability),
      bondable_mode_(bondable_mode),
      on_complete_(std::move(on_complete)) {
  PW_CHECK(!(role == Role::kInitiator && preq_.has_value()));
  PW_CHECK(!(role == Role::kResponder && !preq_.has_value()));
  PW_CHECK(requested_level_ >= SecurityLevel::kEncrypted);
  if (requested_level_ > SecurityLevel::kEncrypted) {
    PW_CHECK(io_capability != IOCapability::kNoInputNoOutput);
  }
  SetPairingChannelHandler(*this);
}

void Phase1::Start() {
  PW_CHECK(!has_failed());
  if (role() == Role::kResponder) {
    PW_CHECK(preq_.has_value());
    RespondToPairingRequest(*preq_);
    return;
  }
  PW_CHECK(!preq_.has_value());
  InitiateFeatureExchange();
}

void Phase1::InitiateFeatureExchange() {
  // Only the initiator can initiate the feature exchange.
  PW_CHECK(role() == Role::kInitiator);
  std::optional<LocalPairingParams> preq_values = BuildPairingParameters();
  if (!preq_values.has_value()) {
    return;
  }
  preq_ = PairingRequestParams{
      .io_capability = preq_values->io_capability,
      .oob_data_flag = preq_values->oob_data_flag,
      .auth_req = preq_values->auth_req,
      .max_encryption_key_size = preq_values->max_encryption_key_size,
      .initiator_key_dist_gen = preq_values->local_keys,
      .responder_key_dist_gen = preq_values->remote_keys};
  sm_chan().SendMessage(kPairingRequest, *preq_);
}

void Phase1::RespondToPairingRequest(const PairingRequestParams& req_params) {
  // We should only be in this state when pairing is initiated by the remote
  // i.e. we are the responder.
  PW_CHECK(role() == Role::kResponder);

  const PairingRequestParams preq = req_params;
  std::optional<LocalPairingParams> pres_values = BuildPairingParameters();
  if (!pres_values.has_value()) {
    return;
  }
  pres_ = PairingResponseParams{
      .io_capability = pres_values->io_capability,
      .oob_data_flag = pres_values->oob_data_flag,
      .auth_req = pres_values->auth_req,
      .max_encryption_key_size = pres_values->max_encryption_key_size,
      .initiator_key_dist_gen = 0,
      .responder_key_dist_gen = 0};
  // The keys that will be exchanged correspond to the intersection of what the
  // initiator requests and we support.
  pres_->initiator_key_dist_gen =
      pres_values->remote_keys & preq.initiator_key_dist_gen;
  pres_->responder_key_dist_gen =
      pres_values->local_keys & preq.responder_key_dist_gen;

  fit::result<ErrorCode, PairingFeatures> maybe_features =
      ResolveFeatures(/*local_initiator=*/false, preq, *pres_);
  if (maybe_features.is_error()) {
    bt_log(DEBUG, "sm", "rejecting pairing features");
    Abort(maybe_features.error_value());
    return;
  }
  PairingFeatures features = maybe_features.value();
  // If we've accepted a non-bondable pairing request in bondable mode as
  // indicated by setting features.will_bond false, we should reflect that in
  // the rsp_params we send to the peer.
  if (!features.will_bond && bondable_mode_ == BondableMode::Bondable) {
    pres_->auth_req &= ~AuthReq::kBondingFlag;
  }

  sm_chan().SendMessage(kPairingResponse, *pres_);

  const PairingResponseParams pres = *pres_;
  CompleteCallback callback = std::move(on_complete_);
  PW_CHECK(callback);
  callback(features, preq, pres);
}

std::optional<LocalPairingParams> Phase1::BuildPairingParameters() {
  // We build `local_params` to reflect the capabilities of this device over the
  // LE transport.
  LocalPairingParams local_params;

  // On LE, the SC flag is set if LE Secure Connections pairing is supported by
  // the device.
  // On BR/EDR, the SC bit is RFU.
  if (sm_chan().link_type() == LinkType::kLE &&
      sm_chan().SupportsSecureConnections()) {
    local_params.auth_req |= AuthReq::kSC;
  }

  // On BR/EDR, MITM bit is RFU.
  if (sm_chan().link_type() == LinkType::kLE &&
      requested_level_ >= SecurityLevel::kAuthenticated) {
    local_params.auth_req |= AuthReq::kMITM;
  }

  // If we are in non-bondable mode there will be no key distribution per V5.1
  // Vol 3 Part C Section 9.4.2.2, so we use the default "no keys" value for
  // LocalPairingParams.
  // BR/EDR CTKD is always "bondable".
  if (bondable_mode_ == BondableMode::Bondable) {
    // On BR/EDR, BondingFlag bit is RFU.
    if (sm_chan().link_type() == LinkType::kLE) {
      local_params.auth_req |= AuthReq::kBondingFlag;
    }

    // We always request identity information from the remote.
    // This applies to both LE and BR/EDR.
    local_params.remote_keys = KeyDistGen::kIdKey;

    PW_CHECK(listener().is_alive());
    WeakSelf<Phase1>::WeakPtr self = weak_self_.GetWeakPtr();
    const bool has_identity = listener()->OnIdentityRequest().has_value();
    if (!self.is_alive()) {
      return std::nullopt;
    }
    if (has_identity) {
      // IdKey applies to both LE and BR/EDR.
      local_params.local_keys |= KeyDistGen::kIdKey;
    }

    // LE: For the current connection, the responder-generated encryption keys
    // (LTK) is always used. As device roles may change in future connections,
    // Fuchsia supports distribution and generation of LTKs by both the local
    // and remote device (V5.0 Vol. 3 Part H 2.4.2.3).
    // BR/EDR: EncKey indicates the intent to derive the LE LTK, which is always
    // the case for us.
    local_params.remote_keys |= KeyDistGen::kEncKey;
    local_params.local_keys |= KeyDistGen::kEncKey;

    // If we support SC over LE, we always try to generate the cross-transport
    // BR/EDR key by setting the link key bit (V5.0 Vol. 3 Part H 3.6.1).
    // On BR/EDR, LinkKey is RFU.
    if (local_params.auth_req & AuthReq::kSC) {
      local_params.local_keys |= KeyDistGen::kLinkKey;
      local_params.remote_keys |= KeyDistGen::kLinkKey;
    }
  }

  // The CT2 bit indicates support for the 2nd Cross-Transport Key Derivation
  // hashing function, a.k.a. H7 (v5.2 Vol. 3 Part H 3.5.1 and 2.4.2.4).
  // This is used for both LE and BR/EDR CTKD.
  local_params.auth_req |= AuthReq::kCT2;

  // On BR/EDR, IO Capability is RFU.
  local_params.io_capability = io_capability_;

  // On BR/EDR, OOB data flag is RFU.
  local_params.oob_data_flag =
      oob_available_ ? OOBDataFlag::kPresent : OOBDataFlag::kNotPresent;
  return local_params;
}

fit::result<ErrorCode, PairingFeatures> Phase1::ResolveFeatures(
    bool local_initiator,
    const PairingRequestParams& preq,
    const PairingResponseParams& pres) {
  // Select the smaller of the initiator and responder max. encryption key size
  // values (Vol 3, Part H, 2.3.4).
  uint8_t enc_key_size =
      std::min(preq.max_encryption_key_size, pres.max_encryption_key_size);
  uint8_t min_allowed_enc_key_size =
      (requested_level_ == SecurityLevel::kSecureAuthenticated)
          ? kMaxEncryptionKeySize
          : kMinEncryptionKeySize;
  // In BR/EDR CTKD, the LE LTK needs to be as strong as the BR/EDR link key,
  // which has the max size.
  if (sm_chan().link_type() == LinkType::kACL) {
    min_allowed_enc_key_size = kMaxEncryptionKeySize;
  }
  if (enc_key_size < min_allowed_enc_key_size) {
    bt_log(DEBUG, "sm", "encryption key size too small! (%u)", enc_key_size);
    return fit::error(ErrorCode::kEncryptionKeySize);
  }

  bool will_bond =
      (preq.auth_req & kBondingFlag) && (pres.auth_req & kBondingFlag);
  // BR/EDR doesn't set the bonding flag, but it always will bond.
  if (sm_chan().link_type() == LinkType::kACL) {
    will_bond = true;
  }
  if (!will_bond) {
    bt_log(
        INFO,
        "sm",
        "negotiated non-bondable pairing (local mode: %s)",
        bondable_mode_ == BondableMode::Bondable ? "bondable" : "non-bondable");
  }
  bool sc = (preq.auth_req & AuthReq::kSC) && (pres.auth_req & AuthReq::kSC);
  bool mitm =
      (preq.auth_req & AuthReq::kMITM) || (pres.auth_req & AuthReq::kMITM);
  // On BR/EDR, the MITM bit is RFU, so we ignore it.
  if (sm_chan().link_type() == LinkType::kACL) {
    mitm = false;
  }
  bool init_oob = preq.oob_data_flag == OOBDataFlag::kPresent;
  bool rsp_oob = pres.oob_data_flag == OOBDataFlag::kPresent;

  IOCapability local_ioc, peer_ioc;
  if (local_initiator) {
    local_ioc = preq.io_capability;
    peer_ioc = pres.io_capability;
  } else {
    local_ioc = pres.io_capability;
    peer_ioc = preq.io_capability;
  }

  PairingMethod method = util::SelectPairingMethod(
      sc, init_oob, rsp_oob, mitm, local_ioc, peer_ioc, local_initiator);

  // If MITM protection is required but the pairing method cannot provide MITM,
  // then reject the pairing.
  if (mitm && method == PairingMethod::kJustWorks) {
    return fit::error(ErrorCode::kAuthenticationRequirements);
  }

  // The "Pairing Response" command (i.e. |pres|) determines the keys that shall
  // be distributed. The keys that will be distributed by us and the peer
  // depends on whichever one initiated the feature exchange by sending a
  // "Pairing Request" command.
  KeyDistGenField local_keys, remote_keys;
  if (local_initiator) {
    local_keys = pres.initiator_key_dist_gen;
    remote_keys = pres.responder_key_dist_gen;

    // v5.1, Vol 3, Part H Section 3.6.1 requires that the responder shall not
    // set to one any flag in the key dist gen fields that the initiator has set
    // to zero. Hence we reject the pairing if the responder requests keys that
    // we don't support.
    if ((preq.initiator_key_dist_gen & local_keys) != local_keys ||
        (preq.responder_key_dist_gen & remote_keys) != remote_keys) {
      return fit::error(ErrorCode::kInvalidParameters);
    }
  } else {
    local_keys = pres.responder_key_dist_gen;
    remote_keys = pres.initiator_key_dist_gen;

    // When we are the responder we always respect the initiator's wishes.
    PW_CHECK((preq.initiator_key_dist_gen & remote_keys) == remote_keys);
    PW_CHECK((preq.responder_key_dist_gen & local_keys) == local_keys);
  }
  // v5.1 Vol 3 Part C Section 9.4.2.2 says that bonding information shall not
  // be exchanged or stored in non-bondable mode. This check ensures that we
  // avoid a situation where, if we were in bondable mode and a peer requested
  // non-bondable mode with a non-zero keydistgen field, we pair in non-bondable
  // mode but also attempt to distribute keys.
  if (!will_bond && (local_keys || remote_keys)) {
    return fit::error(ErrorCode::kInvalidParameters);
  }

  // "If both [...] devices support [LE] Secure Connections [...] the devices
  // may optionally generate the BR/EDR key [..] as part of the LE pairing
  // procedure" (v5.2 Vol. 3 Part C 14.1).
  std::optional<CrossTransportKeyAlgo> generate_ct_key = std::nullopt;
  CrossTransportKeyAlgo ct_algo =
      (preq.auth_req & AuthReq::kCT2) && (pres.auth_req & AuthReq::kCT2)
          ? CrossTransportKeyAlgo::kUseH7
          : CrossTransportKeyAlgo::kUseH6;
  if (sm_chan().link_type() == bt::LinkType::kACL) {
    // "When SMP is running on the BR/EDR transport, the EncKey field is set
    // to one to indicate that the device would like to derive the LTK from
    // the BR/EDR Link Key. When EncKey is set to 1 by both devices in the
    // initiator and responder Key Distribution / Generation fields, the
    // procedures for calculating the LTK from the BR/EDR Link Key shall be
    // used." (v6.0 Vol. 3, Part H, 3.6.1).
    if (local_keys & remote_keys & KeyDistGen::kEncKey) {
      generate_ct_key = ct_algo;
    }
  } else if (sc) {
    // "In LE Secure Connections pairing, when SMP is running on the LE
    // transport, then the EncKey field is ignored" (V5.0 Vol. 3 Part
    // H 3.6.1). We ignore the Encryption Key bit here to allow for uniform
    // handling of it later.
    local_keys &= ~KeyDistGen::kEncKey;
    remote_keys &= ~KeyDistGen::kEncKey;

    // "When hci_spec::LinkKey is set to 1 by both devices in the initiator and
    // responder [KeyDistGen] fields, the procedures for calculating the BR/EDR
    // link key from the LTK shall be used". The chosen procedure depends on the
    // CT2 bit of the AuthReq (v5.2 Vol. 3 Part H 3.5.1 and 3.6.1).
    if (local_keys & remote_keys & KeyDistGen::kLinkKey) {
      generate_ct_key = ct_algo;
    }
  } else if (requested_level_ == SecurityLevel::kSecureAuthenticated) {
    // SecureAuthenticated means Secure Connections is required, so if this
    // pairing would not use Secure Connections it does not meet the
    // requirements of `requested_level_`
    return fit::error(ErrorCode::kAuthenticationRequirements);
  }

  return fit::ok(PairingFeatures{.initiator = local_initiator,
                                 .secure_connections = sc,
                                 .will_bond = will_bond,
                                 .generate_ct_key = generate_ct_key,
                                 .method = method,
                                 .encryption_key_size = enc_key_size,
                                 .local_key_distribution = local_keys,
                                 .remote_key_distribution = remote_keys});
}

void Phase1::OnPairingResponse(const PairingResponseParams& response_params) {
  // Support receiving a pairing response only as the initiator.
  if (role() == Role::kResponder) {
    bt_log(DEBUG, "sm", "received pairing response when acting as responder");
    Abort(ErrorCode::kCommandNotSupported);
    return;
  }

  if (!preq_.has_value() || pres_.has_value()) {
    bt_log(DEBUG, "sm", "ignoring unexpected \"Pairing Response\" packet");
    Abort(ErrorCode::kUnspecifiedReason);
    return;
  }

  fit::result<ErrorCode, PairingFeatures> maybe_features =
      ResolveFeatures(/*local_initiator=*/true, *preq_, response_params);

  if (maybe_features.is_error()) {
    bt_log(DEBUG, "sm", "rejecting pairing features");
    Abort(maybe_features.error_value());
    return;
  }
  PairingFeatures features = maybe_features.value();
  pres_ = response_params;
  const PairingRequestParams preq = *preq_;
  const PairingResponseParams pres = *pres_;
  CompleteCallback callback = std::move(on_complete_);
  PW_CHECK(callback);
  callback(features, preq, pres);
}

void Phase1::OnRxBFrame(ByteBufferPtr sdu) {
  fit::result<ErrorCode, ValidPacketReader> maybe_reader =
      ValidPacketReader::ParseSdu(sdu);
  if (maybe_reader.is_error()) {
    Abort(maybe_reader.error_value());
    return;
  }
  ValidPacketReader reader = maybe_reader.value();
  Code smp_code = reader.code();

  if (smp_code == kPairingFailed) {
    OnFailure(Error(reader.payload<ErrorCode>()));
  } else if (smp_code == kPairingResponse) {
    OnPairingResponse(reader.payload<PairingResponseParams>());
  } else {
    bt_log(INFO,
           "sm",
           "received unexpected code %#.2X when in Pairing Phase 1",
           smp_code);
    Abort(ErrorCode::kUnspecifiedReason);
  }
}

}  // namespace bt::sm
