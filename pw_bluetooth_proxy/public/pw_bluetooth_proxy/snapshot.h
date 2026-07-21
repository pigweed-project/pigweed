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

#include <variant>

#include "pw_bluetooth_proxy/config.h"
#include "pw_function/function.h"

namespace pw::bluetooth::proxy {

// --- Capacity Boundaries & Constants ---

inline constexpr bool kEnableCreditSnapshotUpdates =
    PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES != 0;

// --- Monolithic Top-Level Container ---

struct ProxySnapshot {};

// --- Real-Time Incremental State Update Event Variant ---

using SubsystemStateUpdate = std::variant<std::monostate>;

// Container notification callback signature
using SubsystemStateUpdateCallback =
    pw::Function<void(const SubsystemStateUpdate& update)>;

}  // namespace pw::bluetooth::proxy
