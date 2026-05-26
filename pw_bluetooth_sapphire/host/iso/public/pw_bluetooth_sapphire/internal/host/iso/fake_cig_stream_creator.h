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

namespace bt::iso::testing {

class FakeCigStreamCreator : public CigStreamCreator {
 public:
  FakeCigStreamCreator() : weak_self_(this) {}
  ~FakeCigStreamCreator() override = default;

  WeakSelf<IsoStream>::WeakPtr CreateCisConfiguration(
      CigCisIdentifier id,
      hci_spec::ConnectionHandle cis_handle,
      CisEstablishedCallback on_established_cb,
      pw::Callback<void()> on_closed_cb) override {
    auto stream = std::make_unique<FakeIsoStream>(
        cis_handle, std::move(on_established_cb), std::move(on_closed_cb));
    auto weak_stream = stream->GetWeakPtr();
    streams_.emplace(id, std::move(stream));
    return weak_stream;
  }

  const std::unordered_map<CigCisIdentifier, std::unique_ptr<FakeIsoStream>>&
  streams() const {
    return streams_;
  }

  CigStreamCreator::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  std::unordered_map<CigCisIdentifier, std::unique_ptr<FakeIsoStream>> streams_;
  WeakSelf<FakeCigStreamCreator> weak_self_;  // Keep last.
};

}  // namespace bt::iso::testing
