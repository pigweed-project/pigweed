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

#include "pw_enum/traits.h"
#include "pw_rpc2/internal/protocol_error_enum.h"

namespace pw::rpc2::internal {

/// Which endpoint may send a `ProtocolError`.
///
/// Each value is the top two bits of the codes with that origin. See
/// `OriginOf()`.
enum class CodeOrigin : uint8_t {
  /// Either endpoint may send the code (except `kNone`, which is never sent).
  kAny = 0x00,
  /// Only a server may send the code.
  kServer = 0x40,
  /// Only a client may send the code.
  kClient = 0x80,
  /// No code in this range may be sent.
  kReserved = 0xC0,
};

/// Returns which endpoint may send `code`, from its top two bits.
constexpr CodeOrigin OriginOf(ProtocolError code) {
  return static_cast<CodeOrigin>(static_cast<uint8_t>(code) & 0xC0);
}

/// True if a server may send `code` in an error packet: a defined code other
/// than `kNone` that either endpoint or only a server may send.
constexpr bool IsServerError(ProtocolError code) {
  const CodeOrigin origin = OriginOf(code);
  return code != ProtocolError::kNone && pw::IsValidEnum(code) &&
         (origin == CodeOrigin::kAny || origin == CodeOrigin::kServer);
}

/// True if a client may send `code` in an error packet: a defined code other
/// than `kNone` that either endpoint or only a client may send.
constexpr bool IsClientError(ProtocolError code) {
  const CodeOrigin origin = OriginOf(code);
  return code != ProtocolError::kNone && pw::IsValidEnum(code) &&
         (origin == CodeOrigin::kAny || origin == CodeOrigin::kClient);
}

}  // namespace pw::rpc2::internal
