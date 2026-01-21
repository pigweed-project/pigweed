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

#include "websocket_frame_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>

#include "pw_assert/check.h"
#include "pw_bytes/bit.h"
#include "pw_bytes/endian.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

#define PW_LOG_INFO_IF(cond, ...) \
  do {                            \
    if (cond) {                   \
      PW_LOG_INFO(__VA_ARGS__);   \
    }                             \
  } while (0)

#define PW_LOG_DEBUG_IF(cond, ...) \
  do {                             \
    if (cond) {                    \
      PW_LOG_INFO(__VA_ARGS__);    \
    }                              \
  } while (0)

namespace pw::experimental::websocket::frame_protocol {
namespace {

Result<Frame::Opcode> DecodeOpcode(std::byte opcode_bits) {
  switch (static_cast<uint8_t>(opcode_bits)) {
    case Frame::Opcode::kContinuation:
      return Frame::Opcode::kContinuation;
    case Frame::Opcode::kText:
      return Frame::Opcode::kText;
    case Frame::Opcode::kBinary:
      return Frame::Opcode::kBinary;
    case Frame::Opcode::kClose:
      return Frame::Opcode::kClose;
    case Frame::Opcode::kPing:
      return Frame::Opcode::kPing;
    case Frame::Opcode::kPong:
      return Frame::Opcode::kPong;
    default:
      return Status::InvalidArgument();
  }
}

bool IsBaseProtocolOpcode(Frame::Opcode opcode) {
  return DecodeOpcode(static_cast<std::byte>(opcode)).ok();
}

bool IsControl(Frame::Opcode opcode) {
  switch (opcode) {
    case Frame::Opcode::kContinuation:
    case Frame::Opcode::kText:
    case Frame::Opcode::kBinary:
      return false;
    case Frame::Opcode::kClose:
    case Frame::Opcode::kPing:
    case Frame::Opcode::kPong:
    default:
      return true;
  }
}

struct FrameEncoder {
  // AKA network byte order
  static constexpr auto kByteOrder = endian::big;

  // Frame header byte 1
  static constexpr auto kFinalMask = std::byte{0b1000'0000};
  static constexpr auto kReservedMask = std::byte{0b0111'0000};
  static constexpr auto kOpcodeMask = std::byte{0b0000'1111};

  // Frame header byte 2
  static constexpr auto kMaskedMask = std::byte{0b1000'0000};
  static constexpr auto kLengthMask = std::byte{0b0111'1111};

  // If the masked length is either of these values, then
  // the actual length is larger than 125, and requires either two
  // or eight bytes immediately following byte 2, with the length
  // encoded in network byte order (big endian).
  static constexpr auto kLengthEncodedNext2Bytes = uint8_t{126};
  static constexpr auto kLengthEncodedNext8Bytes = uint8_t{127};

  static constexpr size_t kMinSizeFor2ByteEncoding = 126;
  static constexpr size_t kMinSizeFor8ByteEncoding =
      std::numeric_limits<uint16_t>::max();

  static constexpr uint64_t kSizeBit63 = 0x8000'0000'0000'0000;

  static constexpr size_t kMinimumSize = 2;
  static constexpr size_t kExtraWithUInt16Size = 2;
  static constexpr size_t kExtraWithUInt64Size = 8;
  static constexpr size_t kExtraWithMaskingKey = 4;

  static constexpr size_t kMaxControlPayloadSize = 125;

  static constexpr auto kZero = std::byte{0};

  static size_t ComputeEncodedSize(const Frame& frame) {
    size_t required = kMinimumSize;

    if (frame.payload.size() >= kMinSizeFor2ByteEncoding) {
      if (frame.payload.size() > kMinSizeFor8ByteEncoding) {
        required += kExtraWithUInt64Size;
      } else {
        required += kExtraWithUInt16Size;
      }
    }

    if (frame.mask) {
      required += kExtraWithMaskingKey;
    }

    required += frame.payload.size();

    return required;
  }

  static bool CheckFrameIsValidForBaseProtocol(const Frame& frame,
                                               bool disable_logging) {
    if (static_cast<uint64_t>(frame.payload.size()) > kSizeBit63) {
      PW_LOG_DEBUG_IF(!disable_logging, "illegal payload length");
      return false;
    }

    if (!IsBaseProtocolOpcode(frame.opcode)) {
      PW_LOG_DEBUG_IF(!disable_logging, "invalid opcode");
      return false;
    }

    // From RFC 6455 "5.5 Control Frames"
    // All control frames MUST have a payload length of 125 bytes or less
    // and MUST NOT be fragmented.
    if (IsControl(frame.opcode)) {
      if (frame.payload.size() > kMaxControlPayloadSize) {
        PW_LOG_DEBUG_IF(!disable_logging, "illegal control frame size");
        return false;
      }

      if (!frame.final_fragment) {
        PW_LOG_DEBUG_IF(!disable_logging, "illegal fragmented control frame ");
        return false;
      }
    }

    return true;
  }

  static Result<Frame> DecodeFrame(span<std::byte>& in_out_remaining,
                                   bool disable_logging) {
    if (in_out_remaining.size() < 2) {
      PW_LOG_DEBUG_IF(!disable_logging, "not enough remaining for base");
      in_out_remaining = {};
      return Status::InvalidArgument();
    }

    const auto byte1 = in_out_remaining[0];
    const auto byte2 = in_out_remaining[1];
    in_out_remaining = in_out_remaining.subspan(2);

    if ((byte1 & kReservedMask) != kZero) {
      PW_LOG_DEBUG_IF(!disable_logging, "RSV bits not 0");
      in_out_remaining = {};
      return Status::InvalidArgument();
    }

    const auto opcode = DecodeOpcode(byte1 & kOpcodeMask);
    if (!opcode.ok()) {
      PW_LOG_DEBUG_IF(!disable_logging, "Invalid opcode");
      in_out_remaining = {};
      return opcode.status();
    }

    const bool masked = (byte2 & kMaskedMask) != kZero;

    auto payload_length = static_cast<uint64_t>(byte2 & kLengthMask);
    if (payload_length == kLengthEncodedNext2Bytes) {
      constexpr size_t kLengthBytes = 2;
      if (in_out_remaining.size() < kLengthBytes) {
        PW_LOG_DEBUG_IF(!disable_logging, "not enough remaining for length16");
        in_out_remaining = {};
        return Status::InvalidArgument();
      }

      payload_length = bytes::ReadInOrder<uint16_t>(
          kByteOrder, in_out_remaining.subspan<0, kLengthBytes>());
      in_out_remaining = in_out_remaining.subspan(kLengthBytes);
    } else if (payload_length == kLengthEncodedNext8Bytes) {
      constexpr size_t kLengthBytes = 8;
      if (in_out_remaining.size() < kLengthBytes) {
        PW_LOG_DEBUG_IF(!disable_logging, "not enough remaining for length64");
        in_out_remaining = {};
        return Status::InvalidArgument();
      }

      payload_length = bytes::ReadInOrder<uint64_t>(
          kByteOrder, in_out_remaining.subspan<0, kLengthBytes>());
      in_out_remaining = in_out_remaining.subspan(kLengthBytes);

      // Per RFC, the high bit must not be set.
      if ((payload_length & kSizeBit63) != 0) {
        PW_LOG_DEBUG_IF(!disable_logging, "bad length64");
        in_out_remaining = {};
        return Status::InvalidArgument();
      }
    }

    std::optional<span<const std::byte, kExtraWithMaskingKey>> mask;
    if (masked) {
      if (in_out_remaining.size() < kExtraWithMaskingKey) {
        PW_LOG_DEBUG_IF(!disable_logging,
                        "not enough remaining to read mask key");
        in_out_remaining = {};
        return Status::InvalidArgument();
      }
      mask = in_out_remaining.subspan<0, kExtraWithMaskingKey>();
      in_out_remaining = in_out_remaining.subspan(kExtraWithMaskingKey);
    }

    if (in_out_remaining.size() < payload_length) {
      PW_LOG_DEBUG_IF(!disable_logging, "not enough remaining for payload");
      in_out_remaining = {};
      return Status::InvalidArgument();
    }

    auto payload = in_out_remaining.subspan(0, payload_length);
    in_out_remaining = in_out_remaining.subspan(payload_length);

    if (mask) {
      for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] ^= (*mask)[i % 4];
      }
    }

    Frame decoded_frame{
        .final_fragment = (byte1 & kFinalMask) != kZero,
        .opcode = *opcode,
        .mask = mask,
        .payload = payload,
    };

    if (!CheckFrameIsValidForBaseProtocol(decoded_frame, disable_logging)) {
      return Status::InvalidArgument();
    }

    return decoded_frame;
  }

  static Result<span<const std::byte>> EncodeFrame(const Frame& frame,
                                                   span<std::byte> destination,
                                                   bool disable_logging) {
    const size_t required = ComputeEncodedSize(frame);
    if (required > destination.size()) {
      PW_LOG_DEBUG_IF(!disable_logging, "not enough space for encoding");
      return Status::FailedPrecondition();
    }

    if (!CheckFrameIsValidForBaseProtocol(frame, disable_logging)) {
      return Status::FailedPrecondition();
    }

    const span<std::byte> remaining = destination;
    size_t used = 0;

    std::byte byte1 = kZero;
    byte1 |= frame.final_fragment ? kFinalMask : kZero;
    byte1 |= static_cast<std::byte>(frame.opcode) & kOpcodeMask;

    uint8_t size_uint8 = 0;
    if (frame.payload.size() < kMinSizeFor2ByteEncoding) {
      size_uint8 = static_cast<uint8_t>(frame.payload.size());
    } else if (frame.payload.size() < kMinSizeFor8ByteEncoding) {
      size_uint8 = kLengthEncodedNext2Bytes;
    } else {
      size_uint8 = kLengthEncodedNext8Bytes;
    }

    std::byte byte2 = kZero;
    byte2 |= frame.mask ? kMaskedMask : kZero;
    byte2 |= static_cast<std::byte>(size_uint8) & kLengthMask;

    remaining.subspan<0, kMinimumSize>()[0] = byte1;
    remaining.subspan<0, kMinimumSize>()[1] = byte2;
    used += kMinimumSize;

    if (size_uint8 == kLengthEncodedNext2Bytes) {
      PW_CHECK(used + kExtraWithUInt16Size <= remaining.size());
      const auto dest = remaining.subspan(used, kExtraWithUInt16Size)
                            .subspan<0, kExtraWithUInt16Size>();
      bytes::CopyInOrder(
          kByteOrder, static_cast<uint16_t>(frame.payload.size()), dest.data());
      used += kExtraWithUInt16Size;
    } else if (size_uint8 == kLengthEncodedNext8Bytes) {
      PW_CHECK(used + kExtraWithUInt64Size <= remaining.size());
      const auto dest = remaining.subspan(used, kExtraWithUInt64Size)
                            .subspan<0, kExtraWithUInt64Size>();
      bytes::CopyInOrder(
          kByteOrder, static_cast<uint64_t>(frame.payload.size()), dest.data());
      used += kExtraWithUInt64Size;
    }

    if (frame.mask) {
      std::copy(frame.mask->begin(),
                frame.mask->end(),
                remaining.subspan(used).data());
      used += kExtraWithMaskingKey;
    }

    auto payload = remaining.subspan(used, frame.payload.size());
    if (!frame.mask) {
      std::copy(frame.payload.begin(), frame.payload.end(), payload.begin());
    } else {
      for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = frame.payload[i] ^ (*frame.mask)[i % 4];
      }
    }

    used += frame.payload.size();

    PW_CHECK_INT_EQ(required, used);

    return destination.subspan(0, used);
  }
};

}  // namespace

Result<Frame> DecodeFrame(span<std::byte>& in_out_remaining,
                          bool disable_logging) {
  return FrameEncoder::DecodeFrame(in_out_remaining, disable_logging);
}

Result<span<const std::byte>> EncodeFrame(const Frame& frame,
                                          span<std::byte> destination,
                                          bool disable_logging) {
  return FrameEncoder::EncodeFrame(frame, destination, disable_logging);
}

}  // namespace pw::experimental::websocket::frame_protocol
