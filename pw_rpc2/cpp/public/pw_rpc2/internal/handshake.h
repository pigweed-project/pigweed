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

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"

namespace pw::rpc2::internal {

/// Metadata resulting from a completed protocol handshake.
///
/// When passed as the `local_info` of a handshake future, `negotiated_version`
/// is the highest protocol version the local endpoint supports; it must be
/// non-zero. When returned in an `EstablishedConnection`, it is the version
/// both peers agreed on: the minimum of the two peers' maximum versions.
struct HandshakeInfo {
  uint8_t negotiated_version = HandshakePacket::kMaxVersion;
};

/// Represents an established RPC connection produced by a successful handshake.
struct EstablishedConnection {
  transport::ReliableDatagramSocket connection;
  HandshakeInfo info{};
};

/// Common base class for initiator and responder handshake futures.
class HandshakeFutureBase {
 protected:
  constexpr HandshakeFutureBase() = default;

  HandshakeFutureBase(transport::ReliableDatagramSocket connection,
                      HandshakeInfo local_info)
      : connection_(std::move(connection)), handshake_info_(local_info) {}

  async2::Poll<Status> PendWritePacket(async2::Context& cx,
                                       HandshakePacket::Type type);

  async2::Poll<Result<HandshakePacket>> PendReadPacket(
      async2::Context& cx, HandshakePacket::Type expected_type);

  void Cancel();

  transport::ReliableDatagramSocket connection_;
  HandshakeInfo handshake_info_{};
  std::variant<std::monostate,
               transport::ReserveWriteFuture,
               transport::ReadFuture>
      io_fut_;
};

/// Stages of the three-way handshake, named for the packet exchanged at each
/// step. Both sides run the same sequence and start at `kSyn`; each writes the
/// packet that the other reads.
enum class HandshakeStage : uint8_t {
  /// Default-constructed or moved-from state.
  kEmpty,
  kSyn,
  kSynAck,
  kAck,
  kCompleted,
};

/// Lifetime and state plumbing shared by the initiator and responder handshake
/// futures. Both sides own their connection, cancel it if destroyed before the
/// handshake completes, and are complete exactly when `stage_` is
/// `kCompleted`; only the `Pend()` state machine differs.
class HandshakeFutureImpl : public HandshakeFutureBase {
 public:
  using value_type = Result<EstablishedConnection>;
  using Stage = HandshakeStage;

  HandshakeFutureImpl(const HandshakeFutureImpl&) = delete;
  HandshakeFutureImpl& operator=(const HandshakeFutureImpl&) = delete;

  [[nodiscard]] constexpr bool is_pendable() const { return is_active(); }
  [[nodiscard]] constexpr bool is_complete() const {
    return stage_ == Stage::kCompleted;
  }

 protected:
  [[nodiscard]] constexpr bool is_active() const {
    return stage_ != Stage::kEmpty && stage_ != Stage::kCompleted;
  }

  // The special members are protected rather than public: this is an
  // implementation base, so it must not be deleted through a base pointer, and
  // an initiator must not be sliced into or move-assigned from a responder.
  ~HandshakeFutureImpl() {
    if (is_active()) {
      Cancel();
    }
  }

  HandshakeFutureImpl(HandshakeFutureImpl&& other) noexcept
      : HandshakeFutureBase(std::move(other)),
        stage_(std::exchange(other.stage_, Stage::kEmpty)) {}

  HandshakeFutureImpl& operator=(HandshakeFutureImpl&& other) noexcept {
    if (this != &other) {
      if (is_active()) {
        Cancel();
      }
      HandshakeFutureBase::operator=(std::move(other));
      stage_ = std::exchange(other.stage_, Stage::kEmpty);
    }
    return *this;
  }

  constexpr HandshakeFutureImpl() = default;

  explicit HandshakeFutureImpl(transport::ReliableDatagramSocket connection,
                               HandshakeInfo local_info = {})
      : HandshakeFutureBase(std::move(connection), local_info),
        stage_(Stage::kSyn) {}

  async2::Poll<Result<EstablishedConnection>> Fail(Status status);

  async2::Poll<Result<EstablishedConnection>> Complete() {
    stage_ = Stage::kCompleted;
    return async2::Ready(Result<EstablishedConnection>(
        EstablishedConnection{std::move(connection_), handshake_info_}));
  }

  Stage stage_ = Stage::kEmpty;
};

/// Asynchronous future that coordinates the initiator side of a three-way
/// protocol handshake.
///
/// Handshake flow (Initiator):
/// 1. `kSyn`: Sends its maximum supported version, `local_info`'s
///    `negotiated_version` (`HandshakePacket::Type::kSyn`).
/// 2. `kSynAck`: Receives responder's negotiated version
///    (`HandshakePacket::Type::kSynAck`), verifying it is non-zero and `<=`
///    the local endpoint's max supported version, and adopts it.
/// 3. `kAck`: Sends final confirmation (`HandshakePacket::Type::kAck`) echoing
///    the negotiated version.
///
/// A responder that supports a newer version than the initiator negotiates
/// down to the initiator's version, so a SYN-ACK with a higher version is a
/// protocol violation and fails the handshake with `DATA_LOSS`.
class InitiatorHandshakeFuture final : public HandshakeFutureImpl {
 public:
  constexpr InitiatorHandshakeFuture() = default;

  explicit InitiatorHandshakeFuture(
      transport::ReliableDatagramSocket connection,
      HandshakeInfo local_info = {})
      : HandshakeFutureImpl(std::move(connection), local_info) {}

  InitiatorHandshakeFuture(InitiatorHandshakeFuture&&) noexcept = default;
  InitiatorHandshakeFuture& operator=(InitiatorHandshakeFuture&&) noexcept =
      default;

  async2::Poll<Result<EstablishedConnection>> Pend(async2::Context& cx);
};

static_assert(async2::Future<InitiatorHandshakeFuture>);

/// Asynchronous future that coordinates the responder side of a three-way
/// protocol handshake.
///
/// Handshake flow (Responder):
/// 1. `kSyn`: Receives initiator's maximum version
///    (`HandshakePacket::Type::kSyn`) and negotiates `min(local, peer)`, where
///    `local` is `local_info`'s `negotiated_version`. Any non-zero peer version
///    is accepted, including versions newer than this implementation knows.
/// 2. `kSynAck`: Sends negotiated version (`HandshakePacket::Type::kSynAck`).
/// 3. `kAck`: Receives initiator's confirmation (`HandshakePacket::Type::kAck`)
///    and verifies that it carries the negotiated version.
class ResponderHandshakeFuture final : public HandshakeFutureImpl {
 public:
  constexpr ResponderHandshakeFuture() = default;

  explicit ResponderHandshakeFuture(
      transport::ReliableDatagramSocket connection,
      HandshakeInfo local_info = {})
      : HandshakeFutureImpl(std::move(connection), local_info) {}

  ResponderHandshakeFuture(ResponderHandshakeFuture&&) noexcept = default;
  ResponderHandshakeFuture& operator=(ResponderHandshakeFuture&&) noexcept =
      default;

  async2::Poll<Result<EstablishedConnection>> Pend(async2::Context& cx);
};

static_assert(async2::Future<ResponderHandshakeFuture>);

}  // namespace pw::rpc2::internal
