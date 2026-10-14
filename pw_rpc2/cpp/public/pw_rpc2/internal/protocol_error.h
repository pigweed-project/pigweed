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
#include "pw_rpc2/internal/packet_type.h"
#include "pw_rpc2/internal/protocol_error_enum.h"
#include "pw_rpc2/status.h"

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

// LINT.IfChange(cpp_rpc2_peer_error_mapping)

/// Converts an error code received in a peer's error packet to the status that
/// the call completes with. `sender` is the role of the peer that sent the
/// packet.
///
/// An endpoint only sends codes that the negotiated protocol version defines
/// and that its role may send. Any other code --- zero, an undefined or
/// reserved code, or a code only the receiver's role may send --- is a
/// protocol violation, so it maps to `RpcCode::kProtocolError`.
constexpr RpcStatus PeerErrorToRpcStatus(EndpointRole sender,
                                         ProtocolError code) {
  if (sender == EndpointRole::kServer ? !IsServerError(code)
                                      : !IsClientError(code)) {
    return RpcStatus::ProtocolError();
  }
  switch (code) {
    case ProtocolError::kCancelled:
    case ProtocolError::kDroppedWithoutResponse:
    case ProtocolError::kMethodReturnedWithoutFinishing:
      return RpcStatus::Cancelled();
    case ProtocolError::kFailedToAllocateCall:
      return RpcStatus::Unavailable();
    case ProtocolError::kFailedToAllocateCallResourcesWhileRunning:
    case ProtocolError::kServiceUnregistered:
      return RpcStatus::Aborted();
    case ProtocolError::kMethodTypeMismatch:
    case ProtocolError::kUnknownService:
    case ProtocolError::kUnknownMethod:
      return RpcStatus::Unimplemented();
    case ProtocolError::kInvalidRequestPayload:
      return RpcStatus::DataError();
    case ProtocolError::kNone:
    case ProtocolError::kInternal:
    case ProtocolError::kReceivedPacketForWrongEndpoint:
    case ProtocolError::kReceivedPacketForUnknownCall:
    case ProtocolError::kReceivedDuplicateStart:
      return RpcStatus::ProtocolError();
  }
  return RpcStatus::ProtocolError();
}

// LINT.ThenChange(//pw_rpc2/docs.rst:rpc2_peer_error_mapping)

}  // namespace pw::rpc2::internal
