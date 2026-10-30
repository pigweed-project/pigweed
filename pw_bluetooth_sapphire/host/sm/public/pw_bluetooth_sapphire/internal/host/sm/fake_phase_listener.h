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
#include "pw_bluetooth_sapphire/internal/host/sm/pairing_phase.h"
#include "pw_unit_test/framework.h"

namespace bt::sm {

class FakeListener : public PairingPhase::Listener {
 public:
  FakeListener() : weak_self_(this) {}
  ~FakeListener() override = default;

  // PairingPhase::Listener override:
  std::optional<IdentityInfo> OnIdentityRequest() override {
    identity_info_count_++;
    return identity_info_;
  }

  // PairingPhase::Listener override. Confirms pairing even without a delegate
  // present so that the simplest pairing flows (JustWorks) work with minimal
  // configuration.
  void ConfirmPairing(ConfirmCallback confirm) override {
    if (confirm_delegate_) {
      confirm_delegate_(std::move(confirm));
    } else {
      confirm(true);
    }
  }

  // PairingPhase::Listener override:
  void DisplayPasskey(uint32_t passkey,
                      Delegate::DisplayMethod method,
                      ConfirmCallback confirm) override {
    if (display_delegate_) {
      display_delegate_(passkey, method, std::move(confirm));
    } else {
      ADD_FAILURE() << "No passkey display delegate set for pairing";
    }
  }

  // PairingPhase::Listener override:
  void RequestPasskey(PasskeyResponseCallback respond) override {
    if (request_passkey_delegate_) {
      request_passkey_delegate_(std::move(respond));
    } else {
      ADD_FAILURE()
          << "No passkey entry delegate set for passkey entry pairing";
    }
  }

  // PairingPhase::Listener override:
  void OnPairingFailed(Error error) override {
    pairing_error_count_++;
    last_error_ = error;
  }

  // PairingPhase::Listener override:
  bool IsBrEdrCrossTransportKeyDerivationAllowed() const override {
    return bredr_ctkd_allowed_;
  }

  PairingPhase::Listener::WeakPtr as_weak_ptr() {
    return weak_self_.GetWeakPtr();
  }

  void set_bredr_ctkd_allowed(bool allowed) { bredr_ctkd_allowed_ = allowed; }

  void set_identity_info(std::optional<IdentityInfo> value) {
    identity_info_ = value;
  }
  int identity_info_count() const { return identity_info_count_; }

  using ConfirmDelegate = fit::function<void(ConfirmCallback)>;
  void set_confirm_delegate(ConfirmDelegate delegate) {
    confirm_delegate_ = std::move(delegate);
  }

  using DisplayDelegate =
      fit::function<void(uint32_t, Delegate::DisplayMethod, ConfirmCallback)>;
  void set_display_delegate(DisplayDelegate delegate) {
    display_delegate_ = std::move(delegate);
  }

  // sm::Delegate override:
  using RequestPasskeyDelegate = fit::function<void(PasskeyResponseCallback)>;
  void set_request_passkey_delegate(RequestPasskeyDelegate delegate) {
    request_passkey_delegate_ = std::move(delegate);
  }

  int pairing_error_count() const { return pairing_error_count_; }
  const std::optional<Error>& last_error() const { return last_error_; }

 private:
  std::optional<IdentityInfo> identity_info_ = std::nullopt;
  int identity_info_count_ = 0;

  // Delegate functions used to respond to user input requests from the pairing
  // phases.
  ConfirmDelegate confirm_delegate_;
  DisplayDelegate display_delegate_;
  RequestPasskeyDelegate request_passkey_delegate_;

  int pairing_error_count_ = 0;
  std::optional<Error> last_error_;
  bool bredr_ctkd_allowed_ = true;

  WeakSelf<PairingPhase::Listener> weak_self_;
};

}  // namespace bt::sm
