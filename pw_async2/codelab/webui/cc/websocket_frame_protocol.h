// Copyright 2025 The Pigweed Authors
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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "pw_result/result.h"
#include "pw_span/span.h"

namespace pw::experimental::websocket::frame_protocol {

/// Defines the metadata for a basic Websocket frame
/// See RFC 6455 (December 2011) 5.2 : Base Framing Protocol
struct Frame {
  enum Opcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
  };

  /// True if the frame is the final fragment of a larger message.
  bool final_fragment = true;

  /// The opcode indicating the type of frame
  Opcode opcode = kText;

  /// The optional four-byte mask for this frame.
  std::optional<span<const std::byte, 4>> mask{};

  /// The payload for the frame.
  span<const std::byte> payload{};
};

/// Decodes a single Websocket frame, returning a
/// `pw::experimental::websocket::frame::protocol::Frame` or a status code on
/// error.
///
/// Only the base Websocket protocol frame is supported.
///
/// The returned frame uses spans that are sub-spans of the input span.
///
/// Note that decoding potentially requires modifying the input bytes to unmask
/// the payload. For efficiency, this modification is done in place, which is
/// why the input span must reference non-const bytes.
///
/// @param [in, out] in_out_remaining The span of bytes to decode, and the
/// span for any remaining bytes after decoding one frame on output.
Result<Frame> DecodeFrame(span<std::byte>& in_out_remaining,
                          bool disable_logging = false);

/// Encodes a single Websocket frame, returning the span of bytes used from
/// the start of the destination span, or a status code on error.
///
/// Only the base Websocket protocol frame is supported.
///
/// Note that the payload indicated by the metadata is included in the written
/// data.
///
/// @param [in] frame       The metadata for the frame to encode
/// @param [in] destination The buffer to write the encoded frame into
Result<span<const std::byte>> EncodeFrame(const Frame& frame,
                                          span<std::byte> destination,
                                          bool disable_logging = false);

}  // namespace pw::experimental::websocket::frame_protocol
