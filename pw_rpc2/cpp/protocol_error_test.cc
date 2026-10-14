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

#include "pw_rpc2/internal/protocol_error.h"

#include <cstdint>
#include <iterator>

#include "pw_enum/to_string.h"
#include "pw_enum/traits.h"
#include "pw_rpc2/internal/packet_type.h"
#include "pw_rpc2/status.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::rpc2::RpcCode;
using ::pw::rpc2::RpcStatus;
using ::pw::rpc2::internal::CodeOrigin;
using ::pw::rpc2::internal::EndpointRole;
using ::pw::rpc2::internal::IsClientError;
using ::pw::rpc2::internal::IsServerError;
using ::pw::rpc2::internal::OriginOf;
using ::pw::rpc2::internal::PeerErrorToRpcStatus;
using ::pw::rpc2::internal::ProtocolError;

constexpr ProtocolError Code(unsigned value) {
  return static_cast<ProtocolError>(value);
}

// The value after `code`. Unassigned codes are derived from the last code
// defined in each band rather than hard-coded, so renumbering the codes cannot
// silently change what these tests check.
constexpr ProtocolError After(ProtocolError code) {
  return Code(static_cast<unsigned>(code) + 1);
}

constexpr ProtocolError kLastEitherCode =
    ProtocolError::kReceivedPacketForUnknownCall;
constexpr ProtocolError kLastServerCode =
    ProtocolError::kMethodReturnedWithoutFinishing;

constexpr RpcStatus FromServer(ProtocolError code) {
  return PeerErrorToRpcStatus(EndpointRole::kServer, code);
}

constexpr RpcStatus FromClient(ProtocolError code) {
  return PeerErrorToRpcStatus(EndpointRole::kClient, code);
}

// --- Origins ---

// Every defined code's top two bits match the band it was declared in.
static_assert(OriginOf(ProtocolError::kNone) == CodeOrigin::kAny);
static_assert(OriginOf(ProtocolError::kInternal) == CodeOrigin::kAny);
static_assert(OriginOf(ProtocolError::kCancelled) == CodeOrigin::kAny);
static_assert(OriginOf(ProtocolError::kMethodTypeMismatch) == CodeOrigin::kAny);
static_assert(OriginOf(ProtocolError::kReceivedPacketForWrongEndpoint) ==
              CodeOrigin::kAny);
static_assert(OriginOf(ProtocolError::kReceivedPacketForUnknownCall) ==
              CodeOrigin::kAny);
static_assert(OriginOf(ProtocolError::kUnknownService) == CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kUnknownMethod) == CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kReceivedDuplicateStart) ==
              CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kInvalidRequestPayload) ==
              CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kFailedToAllocateCall) ==
              CodeOrigin::kServer);
static_assert(
    OriginOf(ProtocolError::kFailedToAllocateCallResourcesWhileRunning) ==
    CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kServiceUnregistered) ==
              CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kDroppedWithoutResponse) ==
              CodeOrigin::kServer);
static_assert(OriginOf(ProtocolError::kMethodReturnedWithoutFinishing) ==
              CodeOrigin::kServer);

static_assert(OriginOf(Code(0x3F)) == CodeOrigin::kAny);
static_assert(OriginOf(Code(0x40)) == CodeOrigin::kServer);
static_assert(OriginOf(Code(0x7F)) == CodeOrigin::kServer);
static_assert(OriginOf(Code(0x80)) == CodeOrigin::kClient);
static_assert(OriginOf(Code(0xBF)) == CodeOrigin::kClient);
static_assert(OriginOf(Code(0xC0)) == CodeOrigin::kReserved);
static_assert(OriginOf(Code(0xFF)) == CodeOrigin::kReserved);

// --- Defined codes ---

// Each band's codes are contiguous from the start of the band, so the value
// after the last code in a band is its first unassigned value.
constexpr bool DefinedExactlyInBands() {
  for (unsigned value = 0; value <= 0xFF; ++value) {
    const bool in_either_band = value <= static_cast<unsigned>(kLastEitherCode);
    const bool in_server_band =
        value >= 0x40 && value <= static_cast<unsigned>(kLastServerCode);
    if (pw::IsValidEnum(Code(value)) != (in_either_band || in_server_band)) {
      return false;
    }
  }
  return true;
}

static_assert(DefinedExactlyInBands());
static_assert(OriginOf(kLastEitherCode) == CodeOrigin::kAny);
static_assert(OriginOf(After(kLastEitherCode)) == CodeOrigin::kAny);
static_assert(OriginOf(kLastServerCode) == CodeOrigin::kServer);
static_assert(OriginOf(After(kLastServerCode)) == CodeOrigin::kServer);

// --- Which endpoint may send each code ---

static_assert(!IsServerError(ProtocolError::kNone));
static_assert(!IsClientError(ProtocolError::kNone));

// A client may send exactly the either-endpoint codes, other than `kNone`. A
// server may send those and the server-only codes.
constexpr bool SendersMatchBands() {
  for (unsigned value = 0; value <= 0xFF; ++value) {
    const ProtocolError code = Code(value);
    const bool sendable = code != ProtocolError::kNone && pw::IsValidEnum(code);
    const CodeOrigin origin = OriginOf(code);
    if (IsClientError(code) != (sendable && origin == CodeOrigin::kAny)) {
      return false;
    }
    if (IsServerError(code) != (sendable && (origin == CodeOrigin::kAny ||
                                             origin == CodeOrigin::kServer))) {
      return false;
    }
  }
  return true;
}

static_assert(SendersMatchBands());

static_assert(IsServerError(ProtocolError::kCancelled));
static_assert(IsClientError(ProtocolError::kCancelled));
static_assert(IsServerError(ProtocolError::kDroppedWithoutResponse));
static_assert(!IsClientError(ProtocolError::kDroppedWithoutResponse));

// Undefined and reserved codes are never sent.
static_assert(!IsServerError(After(kLastEitherCode)));
static_assert(!IsClientError(After(kLastEitherCode)));
static_assert(!IsServerError(After(kLastServerCode)));
static_assert(!IsClientError(Code(0x80)));
static_assert(!IsServerError(Code(0xC0)));
static_assert(!IsClientError(Code(0xC0)));
static_assert(!IsServerError(Code(0xFF)));
static_assert(!IsClientError(Code(0xFF)));

// --- PeerErrorToRpcStatus: every row of the wire table ---

// Codes either endpoint may send map the same way from both.
static_assert(FromServer(ProtocolError::kInternal).IsProtocolError());
static_assert(FromClient(ProtocolError::kInternal).IsProtocolError());
static_assert(FromServer(ProtocolError::kCancelled).IsCancelled());
static_assert(FromClient(ProtocolError::kCancelled).IsCancelled());
static_assert(FromServer(ProtocolError::kMethodTypeMismatch).IsUnimplemented());
static_assert(FromClient(ProtocolError::kMethodTypeMismatch).IsUnimplemented());
static_assert(FromServer(ProtocolError::kReceivedPacketForWrongEndpoint)
                  .IsProtocolError());
static_assert(FromClient(ProtocolError::kReceivedPacketForWrongEndpoint)
                  .IsProtocolError());
static_assert(
    FromServer(ProtocolError::kReceivedPacketForUnknownCall).IsProtocolError());
static_assert(
    FromClient(ProtocolError::kReceivedPacketForUnknownCall).IsProtocolError());

// Server-only codes sent by a server.
static_assert(FromServer(ProtocolError::kUnknownService).IsUnimplemented());
static_assert(FromServer(ProtocolError::kUnknownMethod).IsUnimplemented());
static_assert(
    FromServer(ProtocolError::kReceivedDuplicateStart).IsProtocolError());
static_assert(FromServer(ProtocolError::kInvalidRequestPayload).IsDataError());
static_assert(FromServer(ProtocolError::kFailedToAllocateCall).IsUnavailable());
static_assert(
    FromServer(ProtocolError::kFailedToAllocateCallResourcesWhileRunning)
        .IsAborted());
static_assert(FromServer(ProtocolError::kServiceUnregistered).IsAborted());
static_assert(FromServer(ProtocolError::kDroppedWithoutResponse).IsCancelled());
static_assert(
    FromServer(ProtocolError::kMethodReturnedWithoutFinishing).IsCancelled());

// A client that sends a server-only code broke the protocol.
static_assert(FromClient(ProtocolError::kUnknownService).IsProtocolError());
static_assert(FromClient(ProtocolError::kUnknownMethod).IsProtocolError());
static_assert(
    FromClient(ProtocolError::kReceivedDuplicateStart).IsProtocolError());
static_assert(
    FromClient(ProtocolError::kInvalidRequestPayload).IsProtocolError());
static_assert(
    FromClient(ProtocolError::kFailedToAllocateCall).IsProtocolError());
static_assert(
    FromClient(ProtocolError::kFailedToAllocateCallResourcesWhileRunning)
        .IsProtocolError());
static_assert(
    FromClient(ProtocolError::kServiceUnregistered).IsProtocolError());
static_assert(
    FromClient(ProtocolError::kDroppedWithoutResponse).IsProtocolError());
static_assert(FromClient(ProtocolError::kMethodReturnedWithoutFinishing)
                  .IsProtocolError());

// Zero is never sent.
static_assert(FromServer(ProtocolError::kNone).IsProtocolError());
static_assert(FromClient(ProtocolError::kNone).IsProtocolError());

// The first unassigned code in each band.
static_assert(FromServer(After(kLastEitherCode)).IsProtocolError());
static_assert(FromClient(After(kLastEitherCode)).IsProtocolError());
static_assert(FromServer(After(kLastServerCode)).IsProtocolError());
static_assert(FromClient(After(kLastServerCode)).IsProtocolError());

// Band boundaries. No client-only codes are defined, so 0x80 is the first
// unassigned client code. 0xC0 and up are reserved.
static_assert(FromServer(Code(0x7F)).IsProtocolError());
static_assert(FromClient(Code(0x7F)).IsProtocolError());
static_assert(FromServer(Code(0x80)).IsProtocolError());
static_assert(FromClient(Code(0x80)).IsProtocolError());
static_assert(FromServer(Code(0xC0)).IsProtocolError());
static_assert(FromClient(Code(0xC0)).IsProtocolError());
static_assert(FromServer(Code(0xFF)).IsProtocolError());
static_assert(FromClient(Code(0xFF)).IsProtocolError());

// Every code a sender may not send is a protocol error, and a received code
// never maps to a code that only arises locally.
constexpr bool UnsendableCodesAreProtocolErrors() {
  for (unsigned value = 0; value <= 0xFF; ++value) {
    const ProtocolError code = Code(value);
    for (EndpointRole sender : {EndpointRole::kServer, EndpointRole::kClient}) {
      const bool may_send = sender == EndpointRole::kServer
                                ? IsServerError(code)
                                : IsClientError(code);
      const RpcStatus result = PeerErrorToRpcStatus(sender, code);
      if (!may_send && !result.IsProtocolError()) {
        return false;
      }
      switch (result.code()) {
        case RpcCode::kCancelled:
        case RpcCode::kUnavailable:
        case RpcCode::kAborted:
        case RpcCode::kUnimplemented:
        case RpcCode::kDataError:
        case RpcCode::kProtocolError:
          break;
        case RpcCode::kOk:
        case RpcCode::kEndOfStream:
        case RpcCode::kClosed:
        case RpcCode::kWriteClosed:
        case RpcCode::kDisconnected:
        case RpcCode::kResourceExhausted:
        case RpcCode::kMessageTooLarge:
          return false;
      }
    }
  }
  return true;
}

static_assert(UnsendableCodesAreProtocolErrors());

// --- Values and names ---

// The codes that protocol version 1 defines, with the values and names that
// protocol.rst documents. The values are written out rather than derived from
// the enum because they are a wire contract: every endpoint that speaks
// version 1 must agree on them.
struct DocumentedCode {
  ProtocolError code;
  uint8_t value;
  const char* name;
};

constexpr DocumentedCode kVersion1Codes[] = {
    {ProtocolError::kNone, 0x00, "NONE"},
    {ProtocolError::kInternal, 0x01, "INTERNAL"},
    {ProtocolError::kCancelled, 0x02, "CANCELLED"},
    {ProtocolError::kMethodTypeMismatch, 0x03, "METHOD_TYPE_MISMATCH"},
    {ProtocolError::kReceivedPacketForWrongEndpoint,
     0x04,
     "RECEIVED_PACKET_FOR_WRONG_ENDPOINT"},
    {ProtocolError::kReceivedPacketForUnknownCall,
     0x05,
     "RECEIVED_PACKET_FOR_UNKNOWN_CALL"},
    {ProtocolError::kUnknownService, 0x40, "UNKNOWN_SERVICE"},
    {ProtocolError::kUnknownMethod, 0x41, "UNKNOWN_METHOD"},
    {ProtocolError::kReceivedDuplicateStart, 0x42, "RECEIVED_DUPLICATE_START"},
    {ProtocolError::kInvalidRequestPayload, 0x43, "INVALID_REQUEST_PAYLOAD"},
    {ProtocolError::kFailedToAllocateCall, 0x44, "FAILED_TO_ALLOCATE_CALL"},
    {ProtocolError::kFailedToAllocateCallResourcesWhileRunning,
     0x45,
     "FAILED_TO_ALLOCATE_CALL_RESOURCES_WHILE_RUNNING"},
    {ProtocolError::kServiceUnregistered, 0x46, "SERVICE_UNREGISTERED"},
    {ProtocolError::kDroppedWithoutResponse, 0x47, "DROPPED_WITHOUT_RESPONSE"},
    {ProtocolError::kMethodReturnedWithoutFinishing,
     0x48,
     "METHOD_RETURNED_WITHOUT_FINISHING"},
};

// Every code is covered.
static_assert(std::size(kVersion1Codes) ==
              pw::EnumTraits<ProtocolError>::kDistinctValueCount);

// Each code has its documented value. The table is in wire order, so no code is
// listed twice.
constexpr bool ValuesMatchProtocolDocs() {
  int previous_value = -1;
  for (const DocumentedCode& documented : kVersion1Codes) {
    if (static_cast<uint8_t>(documented.code) != documented.value ||
        documented.value <= previous_value) {
      return false;
    }
    previous_value = documented.value;
  }
  return true;
}

static_assert(ValuesMatchProtocolDocs());

TEST(ProtocolError, EnumToStringMatchesProtocolDocs) {
  for (const DocumentedCode& documented : kVersion1Codes) {
    EXPECT_STREQ(pw::EnumToString(documented.code), documented.name);
  }
}

}  // namespace
