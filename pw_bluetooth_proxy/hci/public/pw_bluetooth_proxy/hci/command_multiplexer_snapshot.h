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

#include <cstdint>

#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy::hci {

struct CommandMultiplexerSnapshot;

/// Variant/Type representing an incremental update to the CommandMultiplexer
/// state.
using CommandMultiplexerStateUpdate = CommandMultiplexerSnapshot;

/// Callback type invoked when the CommandMultiplexer state mutates.
///
/// @note When receiving a @c CommandMultiplexerStateUpdate, the platform
/// container is responsible for updating its persistent @c
/// CommandMultiplexerSnapshot.
///
/// @warning **Re-entrancy Safety:** Do not invoke proxy methods from within
/// this callback; it is called synchronously while holding internal mutexes.
using CommandMultiplexerStateUpdateCallback =
    Function<void(const CommandMultiplexerStateUpdate& update)>;

struct CommandMultiplexerSnapshot {
  uint16_t command_credits = 1;

  /// Applies state updates in-place to the top-level CommandMultiplexer
  /// snapshot.
  Status ApplyStateUpdate(const CommandMultiplexerStateUpdate& update);
};

}  // namespace pw::bluetooth::proxy::hci
