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
#include <pw_bluetooth/hci_common.emb.h>

#include "pw_bluetooth_sapphire/internal/host/common/error.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

namespace bt {
namespace hci {

using Error = Error<pw::bluetooth::emboss::StatusCode>;

template <typename... V>
using Result = fit::result<bt::hci::Error, V...>;

template <typename... V>
using ResultFunction = fit::function<void(bt::hci::Result<V...> result)>;

template <typename... V>
using ResultCallback = fit::callback<void(bt::hci::Result<V...> result)>;

// Returns true if |error| is an LMP or Different Transaction Collision status
// (Core Spec v5.4, Vol 1, Part F, Sections 2.35 and 2.42), which the
// Controller reports when a host-initiated LMP procedure races with a
// peer-initiated procedure.
inline bool IsTransactionCollision(const Error& error) {
  return error.is_any_of(
      pw::bluetooth::emboss::StatusCode::LMP_ERROR_TRANSACTION_COLLISION,
      pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION);
}

}  // namespace hci

// Specializations for pw::bluetooth::emboss::StatusCode.
template <>
struct ProtocolErrorTraits<pw::bluetooth::emboss::StatusCode> {
  static std::string ToString(pw::bluetooth::emboss::StatusCode ecode);

  static constexpr bool is_success(pw::bluetooth::emboss::StatusCode ecode) {
    return ecode == pw::bluetooth::emboss::StatusCode::SUCCESS;
  }
};

}  // namespace bt
