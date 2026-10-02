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

#include "pw_allocator/allocator.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/protocol_status.h"
#include "pw_status/status.h"

namespace pw::rpc2::internal {

/// Frames packets as a peer would send them, for tests.
class PacketFramer {
 public:
  /// Frames a packet of any `type` that neither starts a call nor carries an
  /// error code. `payload` must be empty unless `type` has a payload.
  static pw::Result<pw::Buf> FramePacket(pw::Allocator& allocator,
                                         PacketType type,
                                         uint32_t call_id,
                                         pw::ConstByteSpan payload = {});

  /// Frames a packet of `type`, which must start a call. `payload` must be
  /// empty unless `type` has a payload.
  static pw::Result<pw::Buf> FrameStartPacket(pw::Allocator& allocator,
                                              PacketType type,
                                              uint32_t call_id,
                                              uint32_t service_id,
                                              uint32_t method_id,
                                              pw::ConstByteSpan payload = {});

  /// Frames a unary or server-streaming request, which carries `payload` and
  /// closes the client's stream.
  static pw::Result<pw::Buf> FrameStartUnaryPacket(pw::Allocator& allocator,
                                                   uint32_t call_id,
                                                   uint32_t service_id,
                                                   uint32_t method_id,
                                                   pw::ConstByteSpan payload) {
    return Frame(allocator,
                 OutboundPacket::StartUnary(call_id, service_id, method_id),
                 payload);
  }

  /// Frames the packet that starts a client-streaming or bidirectional-
  /// streaming call without a message.
  static pw::Result<pw::Buf> FrameStartStreamPacket(pw::Allocator& allocator,
                                                    uint32_t call_id,
                                                    uint32_t service_id,
                                                    uint32_t method_id) {
    return Frame(allocator,
                 OutboundPacket::StartStream(call_id, service_id, method_id),
                 {});
  }

  static pw::Result<pw::Buf> FrameMessagePacket(pw::Allocator& allocator,
                                                EndpointRole sender,
                                                uint32_t call_id,
                                                pw::ConstByteSpan payload) {
    return Frame(allocator, OutboundPacket::Message(sender, call_id), payload);
  }
  static pw::Result<pw::Buf> FrameClientMessagePacket(
      pw::Allocator& allocator, uint32_t call_id, pw::ConstByteSpan payload) {
    return FrameMessagePacket(
        allocator, EndpointRole::kClient, call_id, payload);
  }
  static pw::Result<pw::Buf> FrameServerMessagePacket(
      pw::Allocator& allocator, uint32_t call_id, pw::ConstByteSpan payload) {
    return FrameMessagePacket(
        allocator, EndpointRole::kServer, call_id, payload);
  }
  static pw::Result<pw::Buf> FrameResponsePacket(pw::Allocator& allocator,
                                                 uint32_t call_id,
                                                 pw::ConstByteSpan payload) {
    return Frame(allocator, OutboundPacket::Response(call_id), payload);
  }

  /// Frames the packet that finishes `sender`'s stream normally: a client
  /// stream end, or a server's successful end of the RPC. See
  /// `OutboundPacket::Finish()`.
  static pw::Result<pw::Buf> FrameFinishPacket(pw::Allocator& allocator,
                                               EndpointRole sender,
                                               uint32_t call_id) {
    return Frame(allocator, OutboundPacket::Finish(sender, call_id), {});
  }
  static pw::Result<pw::Buf> FrameClientStreamEndPacket(
      pw::Allocator& allocator, uint32_t call_id) {
    return Frame(allocator, OutboundPacket::ClientStreamEnd(call_id), {});
  }
  static pw::Result<pw::Buf> FrameServerFinishPacket(pw::Allocator& allocator,
                                                     uint32_t call_id) {
    return Frame(allocator, OutboundPacket::ServerFinish(call_id), {});
  }
  static pw::Result<pw::Buf> FrameServerStreamEndPacket(
      pw::Allocator& allocator, uint32_t call_id) {
    return FrameServerFinishPacket(allocator, call_id);
  }

  static pw::Result<pw::Buf> FrameErrorPacket(pw::Allocator& allocator,
                                              EndpointRole sender,
                                              uint32_t call_id,
                                              ProtocolStatus error) {
    const PacketType type =
        sender == EndpointRole::kServer
            ? PacketType::Make<flags::kServer, flags::kErrorTerminal>()
            : PacketType::Make<flags::kErrorTerminal>();
    return Frame(
        allocator,
        OutboundPacket(type,
                       call_id,
                       OutboundPacket::Fields(static_cast<uint16_t>(error))),
        {});
  }
  static pw::Result<pw::Buf> FrameClientErrorPacket(pw::Allocator& allocator,
                                                    uint32_t call_id,
                                                    ProtocolStatus error) {
    return FrameErrorPacket(allocator, EndpointRole::kClient, call_id, error);
  }
  static pw::Result<pw::Buf> FrameServerErrorPacket(pw::Allocator& allocator,
                                                    uint32_t call_id,
                                                    ProtocolStatus error) {
    return FrameErrorPacket(allocator, EndpointRole::kServer, call_id, error);
  }

 private:
  // Allocates a buffer for `packet` and `payload` and encodes them into it.
  static pw::Result<pw::Buf> Frame(pw::Allocator& allocator,
                                   const OutboundPacket& packet,
                                   pw::ConstByteSpan payload);
};

}  // namespace pw::rpc2::internal
