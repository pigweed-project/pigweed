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

// IWYU pragma: private, include "pw_rpc2/internal/protocol_error.h"

// Defines only the `ProtocolError` enum, so that `protocol_error.h` can use the
// `pw::EnumTraits` that `PW_ENUM` generates at the end of this header. Include
// `pw_rpc2/internal/protocol_error.h` instead of this header.

#include <cstdint>

#include "pw_enum/generate.h"

namespace pw::rpc2::internal {

/// @submodule{pw_rpc2,status}

// LINT.IfChange(cpp_rpc2_error_codes)

/// The 8-bit error code carried by an `ERROR_TERMINAL` packet.
///
/// Every code names exactly one condition, so the peer's logs say precisely
/// why a call ended. `ProtocolError` is internal: no public API takes or
/// returns one. A received code becomes a status in one place, through
/// `PeerErrorToRpcStatus()`.
///
/// The top two bits of each code are its origin, which determines which
/// endpoint may send it (see `CodeOrigin`):
///
/// - `0x00`--`0x3F`: Either endpoint (except `kNone`, which is never sent).
/// - `0x40`--`0x7F`: Server only.
/// - `0x80`--`0xBF`: Client only (none are defined).
/// - `0xC0`--`0xFF`: Reserved.
///
/// These are the codes protocol version 1 defines. An endpoint never sends a
/// code that the negotiated version does not define, so a receiver treats any
/// code it does not recognize as a protocol violation. A later version that
/// adds codes sends an older peer the closest code that peer's version defines.
enum class ProtocolError : uint8_t {
  // --- Either endpoint (0x00-0x3F) ---

  /// No error. Never sent; `Method::Invoke()` returns it when there is no
  /// error to send.
  kNone = 0x00,

  /// A bug in the RPC implementation.
  kInternal = 0x01,

  /// Application code cancelled the call: explicitly with `Cancel()`, or by
  /// dropping a client call handle or reader before the call finished.
  kCancelled = 0x02,

  /// A packet does not match the method's type. On the server, a unary or
  /// server-streaming call must be started by a packet that carries the
  /// request message and closes the client's stream. On the client, a unary or
  /// client-streaming call must be answered by a single packet that carries
  /// the response message and terminates the RPC.
  kMethodTypeMismatch = 0x03,

  /// The endpoint received a packet type that only its own role may send (a
  /// server received a server-to-client packet, or a client received a
  /// client-to-server packet).
  kReceivedPacketForWrongEndpoint = 0x04,

  /// The endpoint received a non-terminal packet for a call ID it does not
  /// know about.
  kReceivedPacketForUnknownCall = 0x05,

  // --- Server only (0x40-0x7F) ---

  /// The requested service is not registered on the server.
  kUnknownService = 0x40,

  /// The requested method is not registered on the target service.
  kUnknownMethod = 0x41,

  /// The server received a start packet for a call ID that is still active.
  /// Both calls end.
  kReceivedDuplicateStart = 0x42,

  /// The request payload could not be decoded.
  kInvalidRequestPayload = 0x43,

  /// The server could not allocate state for the call. The method never ran.
  kFailedToAllocateCall = 0x44,

  /// An allocation failed after the method started.
  kFailedToAllocateCallResourcesWhileRunning = 0x45,

  /// The service was unregistered while the call was running. The C++ server
  /// does not support unregistering services, so it never sends this.
  kServiceUnregistered = 0x46,

  /// A `UnaryWriter` was destroyed without responding or cancelling.
  kDroppedWithoutResponse = 0x47,

  /// The method's future completed while a `Writer` or `UnaryWriter` it handed
  /// out still held the call open.
  kMethodReturnedWithoutFinishing = 0x48,

  // --- Client only (0x80-0xBF) ---

  // None are defined.

  // --- Reserved (0xC0-0xFF) ---
};

// LINT.ThenChange(//pw_rpc2/protocol.rst:rpc2_error_codes)

/// @endsubmodule

}  // namespace pw::rpc2::internal

PW_ENUM(pw::rpc2::internal::ProtocolError,
        kNone,
        kInternal,
        kCancelled,
        kMethodTypeMismatch,
        kReceivedPacketForWrongEndpoint,
        kReceivedPacketForUnknownCall,
        kUnknownService,
        kUnknownMethod,
        kReceivedDuplicateStart,
        kInvalidRequestPayload,
        kFailedToAllocateCall,
        kFailedToAllocateCallResourcesWhileRunning,
        kServiceUnregistered,
        kDroppedWithoutResponse,
        kMethodReturnedWithoutFinishing);
