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

#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_common.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"
#include "pw_result/expected.h"
#include "pw_span/span.h"

namespace bt::iso {

class IsoGroupManager final {
 public:
  using CreateCigDependency =
      pw::Function<std::unique_ptr<IsoGroup>(hci_spec::CigIdentifier,
                                             hci::Transport::WeakPtr,
                                             CigStreamCreator::WeakPtr,
                                             IsoGroup::OnClosedCallback)>;

  // Constructs an IsoGroupManager.
  //  `hci`: The HCI transport to use for sending commands.
  //  `cis_creator`: An interface for creating CIS streams.
  //  `create_cig`: A dependency injection point for creating IsoGroup
  //  instances. This can be replaced in tests to inject mock or fake IsoGroup
  //  objects.
  IsoGroupManager(hci::Transport::WeakPtr hci,
                  CigStreamCreator::WeakPtr cis_creator,
                  CreateCigDependency create_cig = IsoGroup::CreateCig);

  using CreateCigCompleteCallback =
      pw::Callback<void(pw::expected<IsoGroup::WeakPtr, HostError>)>;
  void CreateCig(CigParams cig_params,
                 std::vector<CigCisParams> cis_params,
                 CreateCigCompleteCallback callback,
                 IsoGroup::OnClosedCallback on_closed_callback);

  using WeakPtr = WeakSelf<IsoGroupManager>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  std::optional<hci_spec::CigIdentifier> AllocateCigId();

  hci::Transport::WeakPtr hci_;
  CigStreamCreator::WeakPtr cis_creator_;

  hci_spec::CigIdentifier next_cig_id_ = 0;
  std::unordered_map<hci_spec::CigIdentifier, std::unique_ptr<IsoGroup>>
      groups_;

  CreateCigDependency create_cig_;

  WeakSelf<IsoGroupManager> weak_self_;  // Keep last.
};

}  // namespace bt::iso
