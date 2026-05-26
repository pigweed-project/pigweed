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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_group_manager.h"

#include "pw_assert/check.h"
#include "pw_bluetooth/hci_commands.emb.h"

namespace bt::iso {
namespace {

constexpr hci_spec::CigIdentifier kCigIdModulus = hci_spec::kMaxCigId + 1;

}  // namespace

IsoGroupManager::IsoGroupManager(hci::Transport::WeakPtr hci,
                                 CigStreamCreator::WeakPtr cis_creator,
                                 CreateCigDependency create_cig)
    : hci_(std::move(hci)),
      cis_creator_(std::move(cis_creator)),
      create_cig_(std::move(create_cig)),
      weak_self_(this) {
  PW_CHECK(hci_.is_alive());
}

void IsoGroupManager::CreateCig(
    CigParams cig_params,
    std::vector<CigCisParams> cis_params,
    IsoGroupManager::CreateCigCompleteCallback callback,
    IsoGroup::OnClosedCallback on_closed_callback) {
  auto cig_id = AllocateCigId();
  if (!cig_id.has_value()) {
    callback(pw::unexpected(HostError::kOutOfMemory));
    return;
  }

  auto wrapped_on_closed_callback =
      [self = GetWeakPtr(),
       on_closed_cb = std::move(on_closed_callback)](IsoGroup& cig) mutable {
        on_closed_cb(cig);
        if (self.is_alive()) {
          self->groups_.erase(cig.id());
        }
      };
  auto cig = create_cig_(
      *cig_id, hci_, cis_creator_, std::move(wrapped_on_closed_callback));

  auto on_set_params_completed_callback =
      [cb = std::move(callback),
       weak_cig = cig->GetWeakPtr()](IsoGroup::SetParamsResult result) mutable {
        if (!weak_cig.is_alive()) {
          bt_log(
              WARN, "iso", "CIG destroyed during set CIG parameters command");
          cb(pw::unexpected(HostError::kFailed));
          return;
        }

        cb(result.transform([&] { return weak_cig; }));
      };

  cig->SetParams(std::move(cig_params),
                 std::move(cis_params),
                 std::move(on_set_params_completed_callback));
  groups_.emplace(*cig_id, std::move(cig));
}

std::optional<hci_spec::CigIdentifier> IsoGroupManager::AllocateCigId() {
  if (groups_.size() >= kCigIdModulus) {
    return std::nullopt;
  }

  hci_spec::CigIdentifier allocated = next_cig_id_;
  while (groups_.count(allocated) != 0) {
    allocated = (allocated + 1) % kCigIdModulus;
  }

  next_cig_id_ = (allocated + 1) % kCigIdModulus;
  return allocated;
}

}  // namespace bt::iso
