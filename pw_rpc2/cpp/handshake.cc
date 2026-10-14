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

#include "pw_rpc2/internal/handshake.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/try.h"
#include "pw_bytes/endian.h"
#include "pw_enum/to_string.h"
#include "pw_log/log.h"
#include "pw_preprocessor/compiler.h"
#include "pw_rpc2/internal/packet.h"

namespace pw::rpc2::internal {

void HandshakeFutureBase::Cancel() {
  io_fut_ = std::monostate{};
  if (connection_) {
    connection_.Close();
    connection_ = transport::ReliableDatagramSocket{};
  }
}

async2::Poll<RpcResult<EstablishedConnection>> HandshakeFutureImpl::Fail(
    RpcStatus status) {
  stage_ = Stage::kCompleted;
  Cancel();
  return async2::Ready(RpcResult<EstablishedConnection>(status));
}

async2::Poll<RpcStatus> HandshakeFutureBase::PendWritePacket(
    async2::Context& cx, HandshakePacket::Type type) {
  if (!std::holds_alternative<transport::ReserveWriteFuture>(io_fut_)) {
    io_fut_.emplace<transport::ReserveWriteFuture>(
        connection_.ReserveWrite(HandshakePacket::kWireSizeBytes));
  }
  auto& write_fut = std::get<transport::ReserveWriteFuture>(io_fut_);
  PW_TRY_READY_ASSIGN(auto poll_opt, write_fut.Pend(cx));
  io_fut_ = std::monostate{};
  if (!poll_opt.has_value()) {
    return async2::Ready(RpcStatus::Disconnected());
  }
  auto reservation = std::move(*poll_opt);
  // Before the version is negotiated, `negotiated_version` holds the endpoint's
  // current version, which is what the initiator's SYN advertises. The SYN-ACK
  // and ACK carry the negotiated version. The reservation was sized to hold
  // the packet, so encoding cannot fail.
  const bool encoded = HandshakePacket(type, handshake_info_.negotiated_version)
                           .Encode(reservation);
  PW_CHECK(encoded, "Failed to encode a handshake packet into its reservation");
  if (!reservation.Commit(HandshakePacket::kWireSizeBytes)) {
    return async2::Ready(RpcStatus::Disconnected());
  }
  return async2::Ready(RpcStatus());
}

async2::Poll<RpcResult<HandshakePacket>> HandshakeFutureBase::PendReadPacket(
    async2::Context& cx, HandshakePacket::Type expected_type) {
  if (!std::holds_alternative<transport::ReadFuture>(io_fut_)) {
    io_fut_.emplace<transport::ReadFuture>(connection_.Read());
  }
  auto& read_fut = std::get<transport::ReadFuture>(io_fut_);
  PW_TRY_READY_ASSIGN(ConstBuf read_res, read_fut.Pend(cx));
  io_fut_ = std::monostate{};
  if (read_res == nullptr) {
    return async2::Ready(RpcResult<HandshakePacket>(RpcStatus::Disconnected()));
  }
  std::optional<HandshakePacket> dec_res =
      HandshakePacket::Decode(ConstByteSpan(read_res));
  if (!dec_res.has_value()) {
    // `Decode()` logged why the packet is invalid.
    PW_LOG_WARN("Handshake failed: received an invalid packet instead of %s",
                EnumToString(expected_type));
    return async2::Ready(
        RpcResult<HandshakePacket>(RpcStatus::ProtocolError()));
  }
  if (dec_res->type() != expected_type) {
    PW_LOG_WARN("Handshake failed: expected %s, received %s",
                EnumToString(expected_type),
                EnumToString(dec_res->type()));
    return async2::Ready(
        RpcResult<HandshakePacket>(RpcStatus::ProtocolError()));
  }
  return async2::Ready(RpcResult<HandshakePacket>(*dec_res));
}

async2::Poll<RpcResult<EstablishedConnection>> InitiatorHandshakeFuture::Pend(
    async2::Context& cx) {
  PW_CHECK(is_pendable());
  switch (stage_) {
    case Stage::kEmpty:
    case Stage::kCompleted:
      PW_CRASH("Invalid handshake stage");
    case Stage::kSyn: {
      PW_TRY_READY_ASSIGN(RpcStatus status,
                          PendWritePacket(cx, HandshakePacket::Type::kSyn));
      if (!status.ok()) {
        return Fail(status);
      }
      stage_ = Stage::kSynAck;
      [[fallthrough]];
    }
    case Stage::kSynAck: {
      PW_TRY_READY_ASSIGN(RpcResult<HandshakePacket> pkt,
                          PendReadPacket(cx, HandshakePacket::Type::kSynAck));
      if (!pkt.ok()) {
        return Fail(pkt.status());
      }
      if (pkt->version() > handshake_info_.negotiated_version) {
        PW_LOG_WARN(
            "Handshake failed: SYN_ACK negotiated version %u, but this "
            "endpoint offered at most version %u",
            static_cast<unsigned>(pkt->version()),
            static_cast<unsigned>(handshake_info_.negotiated_version));
        return Fail(RpcStatus::ProtocolError());
      }
      handshake_info_.negotiated_version = pkt->version();
      stage_ = Stage::kAck;
      [[fallthrough]];
    }
    case Stage::kAck: {
      PW_TRY_READY_ASSIGN(RpcStatus status,
                          PendWritePacket(cx, HandshakePacket::Type::kAck));
      if (!status.ok()) {
        return Fail(status);
      }
      return Complete();
    }
  }
  PW_UNREACHABLE;
}

async2::Poll<RpcResult<EstablishedConnection>> ResponderHandshakeFuture::Pend(
    async2::Context& cx) {
  PW_CHECK(is_pendable());
  switch (stage_) {
    case Stage::kEmpty:
    case Stage::kCompleted:
      PW_CRASH("Invalid handshake stage");
    case Stage::kSyn: {
      PW_TRY_READY_ASSIGN(RpcResult<HandshakePacket> pkt,
                          PendReadPacket(cx, HandshakePacket::Type::kSyn));
      if (!pkt.ok()) {
        return Fail(pkt.status());
      }
      handshake_info_.negotiated_version =
          std::min(handshake_info_.negotiated_version, pkt->version());
      stage_ = Stage::kSynAck;
      [[fallthrough]];
    }
    case Stage::kSynAck: {
      PW_TRY_READY_ASSIGN(RpcStatus status,
                          PendWritePacket(cx, HandshakePacket::Type::kSynAck));
      if (!status.ok()) {
        return Fail(status);
      }
      stage_ = Stage::kAck;
      [[fallthrough]];
    }
    case Stage::kAck: {
      PW_TRY_READY_ASSIGN(RpcResult<HandshakePacket> pkt,
                          PendReadPacket(cx, HandshakePacket::Type::kAck));
      if (!pkt.ok()) {
        return Fail(pkt.status());
      }
      if (pkt->version() != handshake_info_.negotiated_version) {
        PW_LOG_WARN(
            "Handshake failed: ACK has version %u, but version %u was "
            "negotiated",
            static_cast<unsigned>(pkt->version()),
            static_cast<unsigned>(handshake_info_.negotiated_version));
        return Fail(RpcStatus::ProtocolError());
      }
      return Complete();
    }
  }
  PW_UNREACHABLE;
}

}  // namespace pw::rpc2::internal
