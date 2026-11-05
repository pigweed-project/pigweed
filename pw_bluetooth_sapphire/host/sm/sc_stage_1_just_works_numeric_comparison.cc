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

#include "pw_bluetooth_sapphire/internal/host/sm/sc_stage_1_just_works_numeric_comparison.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/sm/delegate.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

namespace bt::sm {

ScStage1JustWorksNumericComparison::ScStage1JustWorksNumericComparison(
    PairingPhase::Listener::WeakPtr listener,
    Role role,
    UInt256 local_pub_key_x,
    UInt256 peer_pub_key_x,
    PairingMethod method,
    PairingChannel::WeakPtr sm_chan,
    Stage1CompleteCallback on_complete)
    : listener_(std::move(listener)),
      role_(role),
      local_public_key_x_(local_pub_key_x),
      peer_public_key_x_(peer_pub_key_x),
      method_(method),
      sent_pairing_confirm_(false),
      local_rand_(Random<UInt128>()),
      sent_local_rand_(false),
      peer_rand_(),
      sm_chan_(std::move(sm_chan)),
      on_complete_(std::move(on_complete)),
      weak_self_(this) {
  PW_CHECK(method == PairingMethod::kJustWorks ||
           method == PairingMethod::kNumericComparison);
}

void ScStage1JustWorksNumericComparison::Run() {
  // The responder sends the Pairing Confirm message to start Stage 1, so as
  // initiator there is nothing to do besides wait for the peer's Confirm value
  // (Vol 3, Part H, 2.3.5.6.2).
  if (role_ == Role::kResponder) {
    std::optional<UInt128> maybe_confirm =
        util::F4(local_public_key_x_, peer_public_key_x_, local_rand_, 0);
    if (!maybe_confirm.has_value()) {
      bt_log(WARN, "sm", "unable to calculate confirm value in SC Phase 1");
      NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
      return;
    }
    responder_confirm_ = *maybe_confirm;
    sm_chan_->SendMessage(kPairingConfirm, *responder_confirm_);
    sent_pairing_confirm_ = true;
  }
}

void ScStage1JustWorksNumericComparison::OnPairingConfirm(
    PairingConfirmValue confirm) {
  // Only the responder can send the confirm value to the initiator (Vol 3, Part
  // H, 2.3.5.6.2).
  if (role_ == Role::kResponder) {
    bt_log(WARN,
           "sm",
           "cannot accept pairing confirm in SC Numeric Comparison/Just Works "
           "responder mode");
    NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  if (responder_confirm_.has_value()) {
    bt_log(WARN,
           "sm",
           "received multiple Pairing Confirm values in SC Numeric "
           "Comparison/Just Works");
    NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  responder_confirm_ = confirm;
  // We already know that we're the initiator at this point, which sends the
  // Random immediately after receiving the Confirm.
  SendPairingRandom();
}

void ScStage1JustWorksNumericComparison::SendPairingRandom() {
  // The random value is always sent after the confirm exchange (Vol 3, Part
  // H, 2.3.5.6.2).
  PW_CHECK(responder_confirm_.has_value());
  PW_CHECK(!sent_local_rand_);
  if (role_ == Role::kResponder) {
    PW_CHECK(peer_rand_.has_value());
  }
  sm_chan_->SendMessage(kPairingRandom, local_rand_);
  sent_local_rand_ = true;
  if (role_ == Role::kResponder) {
    CompleteStage1();
  }
}

void ScStage1JustWorksNumericComparison::OnPairingRandom(
    PairingRandomValue rand) {
  if (!responder_confirm_.has_value()) {
    bt_log(WARN,
           "sm",
           "received Pairing Random before the confirm value was exchanged");
    NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  if (peer_rand_.has_value()) {
    bt_log(WARN, "sm", "received multiple Pairing Random values from peer");
    NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  peer_rand_ = rand;
  if (role_ == Role::kResponder) {
    SendPairingRandom();
    return;
  }
  // Otherwise, we're the initiator & we must validate the |responder_confirm_|
  // with |rand|.
  if (!sent_local_rand_) {
    bt_log(
        WARN, "sm", "received peer random before sending our own as initiator");
    NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
    return;
  }
  std::optional<UInt128> maybe_confirm_check =
      util::F4(peer_public_key_x_, local_public_key_x_, rand, 0);
  if (!maybe_confirm_check.has_value()) {
    bt_log(WARN, "sm", "unable to calculate SC confirm check value");
    NotifyComplete(fit::error(ErrorCode::kConfirmValueFailed));
    return;
  }
  if (*maybe_confirm_check != *responder_confirm_) {
    bt_log(WARN, "sm", "peer SC confirm value did not match check, aborting");
    NotifyComplete(fit::error(ErrorCode::kConfirmValueFailed));
    return;
  }
  CompleteStage1();
}

void ScStage1JustWorksNumericComparison::CompleteStage1() {
  PW_CHECK(responder_confirm_.has_value());
  PW_CHECK(peer_rand_.has_value());
  PW_CHECK(sent_local_rand_);
  const auto& [initiator_rand, responder_rand] =
      util::MapToRoles(local_rand_, *peer_rand_, role_);
  const auto& [initiator_pub_key_x, responder_pub_key_x] =
      util::MapToRoles(local_public_key_x_, peer_public_key_x_, role_);
  auto results = Output{.initiator_r = {0},
                        .responder_r = {0},
                        .initiator_rand = initiator_rand,
                        .responder_rand = responder_rand};
  WeakSelf<ScStage1JustWorksNumericComparison>::WeakPtr self =
      weak_self_.GetWeakPtr();
  if (method_ == PairingMethod::kNumericComparison) {
    std::optional<uint32_t> g2_result = util::G2(initiator_pub_key_x,
                                                 responder_pub_key_x,
                                                 initiator_rand,
                                                 responder_rand);
    if (!g2_result.has_value()) {
      bt_log(WARN, "sm", "unable to calculate numeric comparison user check");
      NotifyComplete(fit::error(ErrorCode::kNumericComparisonFailed));
      return;
    }

    // The code displayed to the user is the least significant 6 digits of the
    // G2 function.
    uint32_t comparison_code = *g2_result % 1000000;
    listener_->DisplayPasskey(
        comparison_code,
        Delegate::DisplayMethod::kComparison,
        [self, results](bool passkey_confirmed) {
          bt_log(INFO,
                 "sm",
                 "PairingDelegate %s SC numeric display pairing",
                 passkey_confirmed ? "accepted" : "rejected");
          if (self.is_alive()) {
            passkey_confirmed ? self->NotifyComplete(fit::ok(results))
                              : self->NotifyComplete(fit::error(
                                    ErrorCode::kNumericComparisonFailed));
          }
        });
  } else {  // method == kJustWorks
    listener_->ConfirmPairing([self, results](bool user_confirmed) {
      bt_log(INFO,
             "sm",
             "PairingDelegate %s SC just works pairing",
             user_confirmed ? "accepted" : "rejected");
      if (self.is_alive()) {
        user_confirmed
            ? self->NotifyComplete(fit::ok(results))
            : self->NotifyComplete(fit::error(ErrorCode::kUnspecifiedReason));
      }
    });
  }
}

void ScStage1JustWorksNumericComparison::NotifyComplete(
    fit::result<ErrorCode, Output> result) {
  WeakSelf<ScStage1JustWorksNumericComparison>::WeakPtr self =
      weak_self_.GetWeakPtr();
  Stage1CompleteCallback callback = std::move(on_complete_);
  PW_CHECK(callback);
  callback(result);
  if (self.is_alive()) {
    on_complete_ = std::move(callback);
  }
}

}  // namespace bt::sm
