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

#include "pw_bluetooth_sapphire/internal/host/iso/fake_cig_stream_creator.h"
#include "pw_bluetooth_sapphire/internal/host/iso/fake_iso_group.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_group_manager.h"

namespace bt::iso::testing {

class FakeIsoGroupManagerProvider final {
 public:
  FakeIsoGroupManagerProvider(hci::Transport::WeakPtr hci)
      : iso_group_manager_(
            std::move(hci),
            fake_cig_stream_creator_.GetWeakPtr(),
            fit::bind_member<&FakeIsoGroupManagerProvider::CreateCig>(this)) {}

  IsoGroupManager& iso_group_manager() { return iso_group_manager_; }

 private:
  std::unique_ptr<IsoGroup> CreateCig(
      hci_spec::CigIdentifier id,
      hci::Transport::WeakPtr hci,
      CigStreamCreator::WeakPtr cig_stream_creator,
      IsoGroup::OnClosedCallback on_closed_callback) {
    auto fake_cig =
        std::make_unique<FakeIsoGroup>(id,
                                       std::move(hci),
                                       std::move(cig_stream_creator),
                                       std::move(on_closed_callback));
    last_created_cig_ = fake_cig->GetWeakPtrForFake();
    return fake_cig;
  }

  IsoGroup::WeakPtr last_created_cig_;
  FakeCigStreamCreator fake_cig_stream_creator_;
  IsoGroupManager iso_group_manager_;
};

}  // namespace bt::iso::testing
