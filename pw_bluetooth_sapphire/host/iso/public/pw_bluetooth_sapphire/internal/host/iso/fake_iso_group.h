// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/iso/fake_iso_stream.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_common.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"

namespace bt::iso::testing {

class FakeIsoGroup : public IsoGroup {
 public:
  FakeIsoGroup(hci_spec::CigIdentifier id,
               hci::Transport::WeakPtr hci,
               CigStreamCreator::WeakPtr cig_stream_creator,
               OnClosedCallback on_closed_callback)
      : IsoGroup(id,
                 std::move(hci),
                 std::move(cig_stream_creator),
                 std::move(on_closed_callback)),
        weak_self_(this) {}

  void set_create_cises_result(pw::Status result) {
    create_cises_result_ = result;
  }

  void TriggerOnClosedCallback() { on_closed_callback_(*this); }

  void CompleteSetParams(SetParamsResult result) {
    PW_CHECK(set_params_callback_);
    auto cb = std::move(set_params_callback_);
    cb(result);
  }

  SetParamsCallback steal_set_params_callback() {
    return std::move(set_params_callback_);
  }

  // IsoGroup overrides:
  void SetParams(CigParams cig_params,
                 std::vector<CigCisParams> cis_params,
                 SetParamsCallback callback) override {
    last_cig_params_ = std::move(cig_params);
    last_cis_params_ = std::move(cis_params);
    set_params_callback_ = std::move(callback);
  }

  pw::Status CreateCises(pw::span<CreateCisData> establish_data) override {
    last_create_cises_data_.assign(establish_data.begin(),
                                   establish_data.end());
    return create_cises_result_;
  }

  IsoGroup::WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }

  using WeakPtr = WeakSelf<FakeIsoGroup>::WeakPtr;
  WeakPtr GetWeakPtrForFake() { return weak_self_.GetWeakPtr(); }

  const CigParams& last_cig_params() const { return last_cig_params_; }
  const std::vector<CigCisParams>& last_cis_params() const {
    return last_cis_params_;
  }
  const std::vector<CreateCisData>& last_create_cises_data() const {
    return last_create_cises_data_;
  }

 private:
  pw::Status create_cises_result_ = pw::OkStatus();
  SetParamsCallback set_params_callback_;

  CigParams last_cig_params_;
  std::vector<CigCisParams> last_cis_params_;
  std::vector<CreateCisData> last_create_cises_data_;

  WeakSelf<FakeIsoGroup> weak_self_;  // Keep last.
};

}  // namespace bt::iso::testing
