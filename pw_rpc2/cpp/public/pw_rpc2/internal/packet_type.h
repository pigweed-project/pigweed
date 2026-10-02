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

#include "pw_assert/assert.h"

namespace pw::rpc2::internal {
namespace flags {

/// The bits of a packet's `type` byte, which is a bit field of orthogonal
/// properties rather than an enumeration:
///
/// @code{.unparsed}
///   Bit:    7   6   5     4   3        2          1           0
///         +---+---+---+-----------+--------+-------------+---------+
///         | 0 | 0 | 0 | CloseMode | kStart | kHasPayload | kServer |
///         +---+---+---+-----------+--------+-------------+---------+
/// @endcode
///
/// Bits 4:3 encode the `CloseMode`, so a packet has at most one of
/// `kStreamEnd`, `kOkTerminal`, or `kErrorTerminal`. Bits 7:5 are reserved and
/// must be zero.
///
/// Only some combinations are valid (see `PacketType::IsValid()`). Code builds
/// packet types from these flags with `PacketType::Make()` and inspects them
/// with `PacketType`'s accessors.
enum Flag : uint8_t {
  /// Bit 0: set if the server sent the packet.
  kServer = 0b001,

  /// Bit 1: set if the packet carries a message (of zero or more bytes) that is
  /// delivered to the call. A packet without it carries no bytes after its
  /// header.
  kHasPayload = 0b010,

  /// Bit 2: set if the packet starts a new call. The header is followed by the
  /// `service_id` and `method_id` to invoke.
  kStart = 0b100,

  /// Bits 4:3 (`CloseMode::kStreamEnd`): the sender half-closes its stream.
  kStreamEnd = 0b01'000,

  /// Bits 4:3 (`CloseMode::kOkTerminal`): the RPC completes successfully.
  kOkTerminal = 0b10'000,

  /// Bits 4:3 (`CloseMode::kErrorTerminal`): the RPC aborts with an error code.
  kErrorTerminal = 0b11'000,
};

inline constexpr uint8_t kCloseModeMask = 0b11'000;
inline constexpr uint8_t kReservedMask = 0b111'00'000;

}  // namespace flags

/// Identifies the role of an RPC endpoint on a connection.
///
/// The value is the `flags::kServer` bit of the packets the endpoint
/// sends.
enum class EndpointRole : uint8_t {
  kClient = 0,  // Sends packets with bit 0 == 0, expects bit 0 == 1.
  kServer = flags::kServer,  // Sends packets with bit 0 == 1.
};

/// What a packet does to the sender's stream and the RPC (bits 4:3).
///
/// The values are the `flags::kCloseModeMask` bits of the packet's type byte.
enum class CloseMode : uint8_t {
  /// The sender's stream remains open.
  kOpen = 0,
  /// The sender half-closes (stops writing to) its stream. The RPC continues.
  ///
  /// The protocol allows either the client or the server to half-close. The
  /// C++ server currently always ends the RPC when it closes its stream, for
  /// consistency with gRPC, but clients must still accept a server half-close
  /// and wait for the terminal packet that follows it.
  kStreamEnd = flags::kStreamEnd,
  /// The RPC completes successfully, closing both streams.
  kOkTerminal = flags::kOkTerminal,
  /// The RPC aborts with an error code, closing both streams.
  kErrorTerminal = flags::kErrorTerminal,
};

/// The validated `type` byte of a regular protocol packet in pw_rpc2, which is
/// exchanged after the initial handshake is established.
///
/// A packet type is a combination of `flags`. Code inspects it through
/// the accessors below, which each report one property.
class PacketType {
 public:
  /// True if `bits` is a valid combination of `flags`.
  static constexpr bool IsValid(uint8_t bits) {
    return bits < 32u && ((kValidMask >> bits) & 1u) != 0u;
  }

  /// Returns the packet type with the combined `kFlags`, which are checked at
  /// compile time. For example:
  ///
  /// @code{.cpp}
  ///   PacketType::Make<flags::kServer, flags::kOkTerminal>()
  /// @endcode
  template <flags::Flag... kFlags>
  static constexpr PacketType Make() {
    constexpr unsigned kBits = (0u | ... | kFlags);
    static_assert((0u + ... + kFlags) == kBits,
                  "Packet flags must not overlap; pass each close mode alone");
    static_assert(IsValid(kBits), "Invalid combination of packet flags");
    return PacketType(static_cast<uint8_t>(kBits));
  }

  /// Returns the packet type with `bits`, which the caller has already checked
  /// with `IsValid()`, such as a type byte received on the wire.
  static constexpr PacketType FromValidatedBits(uint8_t bits) {
    PW_DASSERT(IsValid(bits));
    return PacketType(bits);
  }

  /// The type byte as sent on the wire.
  constexpr uint8_t bits() const { return bits_; }

  /// True if the server sent this packet.
  constexpr bool is_server() const { return (bits_ & flags::kServer) != 0u; }

  /// True if this packet carries a message, which may be empty.
  constexpr bool has_payload() const {
    return (bits_ & flags::kHasPayload) != 0u;
  }

  /// True if this packet starts a new call.
  constexpr bool is_start() const { return (bits_ & flags::kStart) != 0u; }

  /// What this packet does to the sender's stream and the RPC.
  constexpr CloseMode close_mode() const {
    return static_cast<CloseMode>(bits_ & flags::kCloseModeMask);
  }

  /// True if this packet ends the RPC, successfully or with an error.
  constexpr bool is_terminal() const {
    return close_mode() == CloseMode::kOkTerminal || is_error();
  }

  /// True if this packet aborts the RPC with an error code.
  constexpr bool is_error() const {
    return close_mode() == CloseMode::kErrorTerminal;
  }

  /// True if this packet was sent by the peer of `receiver`.
  constexpr bool is_for(EndpointRole receiver) const {
    return is_server() != (receiver == EndpointRole::kServer);
  }

  friend constexpr bool operator==(PacketType lhs, PacketType rhs) {
    return lhs.bits_ == rhs.bits_;
  }
  friend constexpr bool operator!=(PacketType lhs, PacketType rhs) {
    return lhs.bits_ != rhs.bits_;
  }

 private:
  constexpr explicit PacketType(uint8_t bits) : bits_(bits) {}

  // The rules that determine which combinations of `flags` are valid.
  static constexpr bool IsValidBits(uint8_t bits) {
    if ((bits & flags::kReservedMask) != 0u) {
      return false;
    }

    const bool server = (bits & flags::kServer) != 0u;
    const bool has_payload = (bits & flags::kHasPayload) != 0u;
    const uint8_t close_mode =
        static_cast<uint8_t>(bits & flags::kCloseModeMask);

    if ((bits & flags::kStart) != 0u) {
      // Only a client starts a call, and a start packet cannot end the RPC.
      return !server && (close_mode == 0u || close_mode == flags::kStreamEnd);
    }

    switch (close_mode) {
      case flags::kStreamEnd:
        // Either side may half-close its stream, with or without a message.
        return true;
      case flags::kOkTerminal:
        // Only the server completes an RPC successfully.
        return server;
      case flags::kErrorTerminal:
        // Error packets carry an error code, never a message.
        return !has_payload;
      default:
        // A packet that neither starts a call, carries a message, nor closes
        // anything would be a no-op.
        return has_payload;
    }
  }

  // Bit `n` is set if the type byte `n` is valid.
  static constexpr uint32_t ValidMask() {
    uint32_t mask = 0;
    for (uint8_t bits = 0; bits < 32u; ++bits) {
      if (IsValidBits(bits)) {
        mask |= uint32_t{1} << bits;
      }
    }
    return mask;
  }

  static const uint32_t kValidMask;

  uint8_t bits_;
};

// Defined out of line because `ValidMask()` cannot be called in a constant
// expression until `PacketType` is complete.
inline constexpr uint32_t PacketType::kValidMask = PacketType::ValidMask();

}  // namespace pw::rpc2::internal
