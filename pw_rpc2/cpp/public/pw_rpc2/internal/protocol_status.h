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

/// Wire-level protocol status codes for `pw_rpc2`.
///
/// Each non-zero value identifies the specific condition that terminated a
/// call in an `ERROR_TERMINAL` packet. Codes are partitioned into decimal
/// bands so that common and role-specific codes remain distinct and can grow
/// independently:
///
/// - `0`--`99`: Common codes (valid from either client or server, except
///   `kOk`, which is never sent in an error packet).
/// - `100`--`199`: Server-only codes (only sent in error packets with
///   `flags::kServer` set).
/// - `200`--`255`: Reserved (e.g., for future client-only error codes).
enum class ProtocolStatus : uint8_t {
  // --- Common codes (0-99) ---

  /// No protocol error occurred. Used as the success return value for internal
  /// invocation functions; never sent in an error packet.
  kOk = 0,

  /// Unrecognized error code.
  kUnknown = 1,

  /// An error that indicates a programming bug within pw_rpc2.
  kInternal = 2,

  /// The call was deliberately cancelled by application code.
  kCancelled = 3,

  /// The endpoint received a packet type that may only be sent by its own role
  /// (a server received a server-to-client packet, or a client received a
  /// client-to-server packet).
  kReceivedPacketForWrongEndpoint = 4,

  /// A packet does not match the method's type. On the server, a unary or
  /// server-streaming call must be started by a packet that carries the
  /// request message and closes the client's stream. On the client, a unary or
  /// client-streaming call must be answered by a single packet that carries
  /// the response message and terminates the RPC.
  kMethodTypeMismatch = 5,

  // --- Server-only codes (100-199) ---

  /// The server released a unary call without sending a response or
  /// cancelling it.
  kDroppedWithoutResponse = 100,

  /// The target service was unregistered from the server while the call was
  /// running.
  kServiceUnregistered = 101,

  /// The requested service is not registered on the server.
  kUnknownService = 102,

  /// The requested method is not registered on the target service.
  kUnknownMethod = 103,

  /// The request payload was invalid.
  kInvalidRequestPayload = 104,

  /// Failed to allocate call state for an incoming request.
  kFailedToAllocateCall = 105,

  /// Failed to allocate necessary resources while running the call.
  kFailedToAllocateCallResourcesWhileRunning = 106,

  // --- Reserved (200-255) ---
};

// LINT.ThenChange(//pw_rpc2/protocol.rst:rpc2_error_codes)

/// True if `code` may be sent in a server error packet (common or server-only,
/// excluding `kOk`).
template <typename StatusEnum = ProtocolStatus>
constexpr bool IsServerError(StatusEnum code) {
  static_assert(std::is_same_v<StatusEnum, ProtocolStatus>);
  return code != ProtocolStatus::kOk && IsValidEnum(code);
}

/// True if `code` may be sent in a client error packet (`1`--`99`, excluding
/// `kOk`).
template <typename StatusEnum = ProtocolStatus>
constexpr bool IsClientError(StatusEnum code) {
  static_assert(std::is_same_v<StatusEnum, ProtocolStatus>);
  const auto value = static_cast<uint8_t>(code);
  return value > 0 && value < 100 && IsValidEnum(code);
}

/// Translates a wire `ProtocolStatus` into its equivalent `pw::Status` for
/// public call completion APIs.
///
/// Guaranteed never to return `OkStatus()` or `Status::OutOfRange()` (stream
/// EOF), even if an unrecognized value is received in an error packet from the
/// wire.
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
      return Status::Cancelled();
    case ProtocolStatus::kReceivedPacketForWrongEndpoint:
      return Status::Unimplemented();
    case ProtocolStatus::kUnknownService:
    case ProtocolStatus::kUnknownMethod:
      return Status::NotFound();
    case ProtocolStatus::kInvalidRequestPayload:
      return Status::DataLoss();
    case ProtocolStatus::kFailedToAllocateCall:
    case ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning:
      return Status::ResourceExhausted();
    case ProtocolStatus::kMethodTypeMismatch:
      return Status::FailedPrecondition();
  }
  return Status::Unknown();
}

}  // namespace pw::rpc2::internal

PW_ENUM(pw::rpc2::internal::ProtocolStatus,
        kOk,
        kUnknown,
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
        kFailedToAllocateCallResourcesWhileRunning);
