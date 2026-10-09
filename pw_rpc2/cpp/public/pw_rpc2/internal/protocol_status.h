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
#include <type_traits>

#include "pw_enum/generate.h"
#include "pw_enum/traits.h"
#include "pw_status/status.h"

namespace pw::rpc2::internal {

// LINT.IfChange(cpp_rpc2_error_codes)

/// Which endpoint may send a `ProtocolStatus` code in an error packet.
///
/// Each value is the top two bits of the codes with that origin. See
/// `OriginOf()`.
enum class ProtocolStatusOrigin : uint8_t {
  /// The code may appear anywhere: either endpoint may send it, and it may also
  /// arise locally.
  kAny = 0x00,
  /// Only a server may send the code. A client only receives it from the wire.
  kServer = 0x40,
  /// Only a client may send the code. A server only receives it from the wire.
  kClient = 0x80,
  /// The code is never sent. It only arises locally, and is rejected (mapped to
  /// `ProtocolStatus::kUnknown`) if received from the wire.
  kLocal = 0xC0,
};

/// Status codes for `pw_rpc2`.
///
/// Wire codes identify the condition that terminated a call in an
/// `ERROR_TERMINAL` packet. Local codes describe conditions that an endpoint
/// detects itself and that are never sent.
///
/// The top two bits of each code are its `ProtocolStatusOrigin`, which
/// determines which endpoint may send it:
///
/// - `0x00`--`0x3F`: Either endpoint (except `kOk`, which is never sent).
/// - `0x40`--`0x7F`: Server only.
/// - `0x80`--`0xBF`: Client only (none are currently defined).
/// - `0xC0`--`0xFF`: Local only; never sent.
enum class ProtocolStatus : uint8_t {
  // --- Either endpoint (0x00-0x3F) ---

  /// No error occurred. Used as the success return value for internal
  /// invocation functions; never sent in an error packet.
  kOk = 0x00,

  /// An error that indicates a programming bug within pw_rpc2.
  kInternal = 0x01,

  /// The call was deliberately cancelled by application code.
  kCancelled = 0x02,

  /// The endpoint received a packet type that may only be sent by its own role
  /// (a server received a server-to-client packet, or a client received a
  /// client-to-server packet).
  kReceivedPacketForWrongEndpoint = 0x03,

  /// A packet does not match the method's type. On the server, a unary or
  /// server-streaming call must be started by a packet that carries the
  /// request message and closes the client's stream. On the client, a unary or
  /// client-streaming call must be answered by a single packet that carries
  /// the response message and terminates the RPC.
  kMethodTypeMismatch = 0x04,

  // --- Server only (0x40-0x7F) ---

  /// The server released a unary call without sending a response or
  /// cancelling it.
  kDroppedWithoutResponse = 0x40,

  /// The target service was unregistered from the server while the call was
  /// running.
  kServiceUnregistered = 0x41,

  /// The requested service is not registered on the server.
  kUnknownService = 0x42,

  /// The requested method is not registered on the target service.
  kUnknownMethod = 0x43,

  /// The request payload was invalid.
  kInvalidRequestPayload = 0x44,

  /// Failed to allocate call state for an incoming request.
  kFailedToAllocateCall = 0x45,

  /// Failed to allocate necessary resources while running the call.
  kFailedToAllocateCallResourcesWhileRunning = 0x46,

  // --- Client only (0x80-0xBF) ---

  // None are currently defined.

  // LINT.ThenChange(//pw_rpc2/protocol.rst:rpc2_error_codes)

  // --- Local only (0xC0-0xFF) ---

  /// The peer finished its stream, and every message it sent has been read.
  kEndOfStream = 0xC0,

  /// The user closed the `Client` or `Server`, or the handle used is closed.
  kClosed = 0xC1,

  /// The underlying transport socket closed.
  kSocketClosed = 0xC2,

  /// The transport failed to connect to the peer.
  kConnectFailed = 0xC3,

  /// The connection handshake failed: a handshake packet was malformed or
  /// unexpected, or the endpoints' protocol versions are incompatible.
  kHandshakeFailed = 0xC4,

  /// An inbound RPC packet could not be parsed.
  kMalformedPacket = 0xC5,

  /// The client used every available call ID.
  kCallIdsExhausted = 0xC6,

  /// The call's write side is closed, so nothing more can be written to it.
  kWriteClosed = 0xC7,

  /// An outbound message is larger than the transport or reservation allows.
  kMessageTooLarge = 0xC8,

  /// An outbound message could not be serialized.
  kSerializationFailed = 0xC9,

  /// An inbound message payload could not be deserialized.
  kDeserializationFailed = 0xCA,

  /// A local allocation failed.
  kOutOfMemory = 0xCB,

  /// A received code was unrecognized, or was sent by an endpoint that may not
  /// send it.
  kUnknown = 0xFF,
};

/// Returns which endpoint may send `code`, from its top two bits.
constexpr ProtocolStatusOrigin OriginOf(ProtocolStatus code) {
  return static_cast<ProtocolStatusOrigin>(static_cast<uint8_t>(code) & 0xC0);
}

/// True if `code` may be sent in a server error packet: a defined code that
/// either endpoint or only a server may send, excluding `kOk`.
template <typename StatusEnum = ProtocolStatus>
constexpr bool IsServerError(StatusEnum code) {
  static_assert(std::is_same_v<StatusEnum, ProtocolStatus>);
  const ProtocolStatusOrigin origin = OriginOf(code);
  return code != ProtocolStatus::kOk && IsValidEnum(code) &&
         (origin == ProtocolStatusOrigin::kAny ||
          origin == ProtocolStatusOrigin::kServer);
}

/// True if `code` may be sent in a client error packet: a defined code that
/// either endpoint or only a client may send, excluding `kOk`.
template <typename StatusEnum = ProtocolStatus>
constexpr bool IsClientError(StatusEnum code) {
  static_assert(std::is_same_v<StatusEnum, ProtocolStatus>);
  const ProtocolStatusOrigin origin = OriginOf(code);
  return code != ProtocolStatus::kOk && IsValidEnum(code) &&
         (origin == ProtocolStatusOrigin::kAny ||
          origin == ProtocolStatusOrigin::kClient);
}

/// Translates a `ProtocolStatus` into its equivalent `pw::Status` for public
/// call completion APIs.
///
/// Never returns `OkStatus()`. Returns `Status::OutOfRange()` (stream EOF) only
/// for the local `kEndOfStream`, so no code received from the wire maps to it.
constexpr Status ToStatus(ProtocolStatus code) {
  switch (code) {
    case ProtocolStatus::kOk:
      return Status::Internal();
    case ProtocolStatus::kUnknown:
      return Status::Unknown();
    case ProtocolStatus::kInternal:
      return Status::Internal();
    case ProtocolStatus::kCancelled:
    case ProtocolStatus::kDroppedWithoutResponse:
    case ProtocolStatus::kServiceUnregistered:
    case ProtocolStatus::kClosed:
      return Status::Cancelled();
    case ProtocolStatus::kReceivedPacketForWrongEndpoint:
      return Status::Unimplemented();
    case ProtocolStatus::kUnknownService:
    case ProtocolStatus::kUnknownMethod:
      return Status::NotFound();
    case ProtocolStatus::kInvalidRequestPayload:
    case ProtocolStatus::kHandshakeFailed:
    case ProtocolStatus::kMalformedPacket:
    case ProtocolStatus::kDeserializationFailed:
      return Status::DataLoss();
    case ProtocolStatus::kFailedToAllocateCall:
    case ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning:
    case ProtocolStatus::kCallIdsExhausted:
    case ProtocolStatus::kMessageTooLarge:
    case ProtocolStatus::kOutOfMemory:
      return Status::ResourceExhausted();
    case ProtocolStatus::kMethodTypeMismatch:
    case ProtocolStatus::kWriteClosed:
      return Status::FailedPrecondition();
    case ProtocolStatus::kEndOfStream:
      return Status::OutOfRange();
    case ProtocolStatus::kSocketClosed:
    case ProtocolStatus::kConnectFailed:
      return Status::Unavailable();
    case ProtocolStatus::kSerializationFailed:
      return Status::InvalidArgument();
  }
  return Status::Unknown();
}

}  // namespace pw::rpc2::internal

PW_ENUM(pw::rpc2::internal::ProtocolStatus,
        kOk,
        kInternal,
        kCancelled,
        kReceivedPacketForWrongEndpoint,
        kMethodTypeMismatch,
        kDroppedWithoutResponse,
        kServiceUnregistered,
        kUnknownService,
        kUnknownMethod,
        kInvalidRequestPayload,
        kFailedToAllocateCall,
        kFailedToAllocateCallResourcesWhileRunning,
        kEndOfStream,
        kClosed,
        kSocketClosed,
        kConnectFailed,
        kHandshakeFailed,
        kMalformedPacket,
        kCallIdsExhausted,
        kWriteClosed,
        kMessageTooLarge,
        kSerializationFailed,
        kDeserializationFailed,
        kOutOfMemory,
        kUnknown);
