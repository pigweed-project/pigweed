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

#include "pw_enum/generate.h"
#include "pw_status/status.h"

namespace pw::rpc2::internal {

/// Wire-level protocol status codes sent by a `pw_rpc2` server
/// (`PacketType::kServerError`).
///
/// Each value identifies the specific server condition that terminated a call.
enum class ServerError : uint8_t {
  /// No protocol error occurred. Used as the success return value for internal
  /// invocation functions; never sent in a `PacketType::kServerError` packet.
  kOk = 0,

  /// Unrecognized error code.
  kUnknown = 1,

  /// An error that indicates a programming bug within pw_rpc2.
  kInternal = 2,

  /// The call was deliberately cancelled by the server-side application code.
  kCancelled = 3,

  /// The server received a packet type that may only be sent from a server to
  /// a client.
  kReceivedPacketForClient = 4,

  /// The server released a unary call without sending a response or
  /// cancelling it.
  kDroppedWithoutResponse = 5,

  /// The target service was unregistered from the server while the call was
  /// running.
  kServiceUnregistered = 6,

  /// The requested service is not registered on the server.
  kUnknownService = 7,

  /// The requested method is not registered on the target service.
  kUnknownMethod = 8,

  /// The request payload was invalid.
  kInvalidRequestPayload = 9,

  /// Failed to allocate call state for an incoming request.
  kFailedToAllocateCall = 10,

  /// Failed to allocate necessary resources while running the call.
  kFailedToAllocateCallResourcesWhileRunning = 11,
};

/// Wire-level protocol status codes sent by a `pw_rpc2` client
/// (`PacketType::kClientError`).
///
/// Each value identifies the specific client condition that terminated a call.
/// Codes with equivalent meanings in `ServerError` share the same value.
enum class ClientError : uint8_t {
  /// No protocol error occurred. Never sent in a `PacketType::kClientError`
  /// packet.
  kOk = 0,

  /// Unrecognized error code.
  kUnknown = 1,

  /// An error that indicates a programming bug within pw_rpc2.
  kInternal = 2,

  /// The call was deliberately cancelled by the client-side application code.
  kCancelled = 3,

  /// The client received a packet type that may only be sent from a client to
  /// a server.
  kReceivedPacketForServer = 4,
};

/// Translates a wire `ServerError` into its equivalent `pw::Status` for public
/// call completion APIs on the client.
///
/// Guaranteed never to return `OkStatus()` or `Status::OutOfRange()` (stream
/// EOF), even if an unrecognized value is received in an error packet from the
/// wire.
constexpr Status ToStatus(ServerError code) {
  switch (code) {
    case ServerError::kOk:
      return Status::Internal();
    case ServerError::kUnknown:
      return Status::Unknown();
    case ServerError::kInternal:
      return Status::Internal();
    case ServerError::kCancelled:
    case ServerError::kDroppedWithoutResponse:
    case ServerError::kServiceUnregistered:
      return Status::Cancelled();
    case ServerError::kReceivedPacketForClient:
      return Status::Unimplemented();
    case ServerError::kUnknownService:
    case ServerError::kUnknownMethod:
      return Status::NotFound();
    case ServerError::kInvalidRequestPayload:
      return Status::DataLoss();
    case ServerError::kFailedToAllocateCall:
    case ServerError::kFailedToAllocateCallResourcesWhileRunning:
      return Status::ResourceExhausted();
  }
  return Status::Unknown();
}

/// Translates a wire `ClientError` into its equivalent `pw::Status` for public
/// call completion APIs on the server.
///
/// Guaranteed never to return `OkStatus()` or `Status::OutOfRange()` (stream
/// EOF), even if an unrecognized value is received in an error packet from the
/// wire.
constexpr Status ToStatus(ClientError code) {
  switch (code) {
    case ClientError::kOk:
      return Status::Internal();
    case ClientError::kUnknown:
      return Status::Unknown();
    case ClientError::kInternal:
      return Status::Internal();
    case ClientError::kCancelled:
      return Status::Cancelled();
    case ClientError::kReceivedPacketForServer:
      return Status::Unimplemented();
  }
  return Status::Unknown();
}

}  // namespace pw::rpc2::internal

PW_ENUM(pw::rpc2::internal::ServerError,
        kOk,
        kUnknown,
        kInternal,
        kCancelled,
        kReceivedPacketForClient,
        kDroppedWithoutResponse,
        kServiceUnregistered,
        kUnknownService,
        kUnknownMethod,
        kInvalidRequestPayload,
        kFailedToAllocateCall,
        kFailedToAllocateCallResourcesWhileRunning);

PW_ENUM(pw::rpc2::internal::ClientError,
        kOk,
        kUnknown,
        kInternal,
        kCancelled,
        kReceivedPacketForServer);
