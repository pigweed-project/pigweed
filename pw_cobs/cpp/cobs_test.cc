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

#include "pw_cobs/cobs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::cobs {
namespace {

TEST(Cobs, EncodeAndDecodeEmpty) {
  std::array<std::byte, 4> encoded{};
  auto enc = Encode(ConstByteSpan(), encoded);
  ASSERT_EQ(enc.status(), OkStatus());
  constexpr auto kExpected = bytes::Array<0x01, 0x00>();
  ASSERT_EQ(enc->size(), kExpected.size());
  EXPECT_EQ(std::memcmp(enc->data(), kExpected.data(), kExpected.size()), 0);

  std::array<std::byte, 4> decoded{};
  auto dec = Decode(*enc, decoded);
  ASSERT_EQ(dec.status(), OkStatus());
  EXPECT_EQ(dec->size(), 0u);
}

TEST(Cobs, EncodeAndDecodeSingleZero) {
  constexpr auto kInput = bytes::Array<0x00>();
  std::array<std::byte, 8> encoded{};
  auto enc = Encode(kInput, encoded);
  ASSERT_EQ(enc.status(), OkStatus());
  constexpr auto kExpected = bytes::Array<0x01, 0x01, 0x00>();
  ASSERT_EQ(enc->size(), kExpected.size());
  EXPECT_EQ(std::memcmp(enc->data(), kExpected.data(), kExpected.size()), 0);

  std::array<std::byte, 8> decoded{};
  auto dec = Decode(*enc, decoded);
  ASSERT_EQ(dec.status(), OkStatus());
  ASSERT_EQ(dec->size(), kInput.size());
  EXPECT_EQ(std::memcmp(dec->data(), kInput.data(), kInput.size()), 0);
}

TEST(Cobs, EncodeAndDecodeMultipleZeros) {
  constexpr auto kInput = bytes::Array<0x00, 0x00>();
  std::array<std::byte, 8> encoded{};
  auto enc = Encode(kInput, encoded);
  ASSERT_EQ(enc.status(), OkStatus());
  constexpr auto kExpected = bytes::Array<0x01, 0x01, 0x01, 0x00>();
  ASSERT_EQ(enc->size(), kExpected.size());
  EXPECT_EQ(std::memcmp(enc->data(), kExpected.data(), kExpected.size()), 0);

  std::array<std::byte, 8> decoded{};
  auto dec = Decode(*enc, decoded);
  ASSERT_EQ(dec.status(), OkStatus());
  ASSERT_EQ(dec->size(), kInput.size());
  EXPECT_EQ(std::memcmp(dec->data(), kInput.data(), kInput.size()), 0);
}

TEST(Cobs, EncodeAndDecodeMixedZeroAndNonZeroBytes) {
  constexpr auto kInput = bytes::Array<0x11, 0x22, 0x00, 0x33>();
  std::array<std::byte, 8> encoded{};
  auto enc = Encode(kInput, encoded);
  ASSERT_EQ(enc.status(), OkStatus());
  constexpr auto kExpected = bytes::Array<0x03, 0x11, 0x22, 0x02, 0x33, 0x00>();
  ASSERT_EQ(enc->size(), kExpected.size());
  EXPECT_EQ(std::memcmp(enc->data(), kExpected.data(), kExpected.size()), 0);

  std::array<std::byte, 8> decoded{};
  auto dec = Decode(*enc, decoded);
  ASSERT_EQ(dec.status(), OkStatus());
  ASSERT_EQ(dec->size(), kInput.size());
  EXPECT_EQ(std::memcmp(dec->data(), kInput.data(), kInput.size()), 0);
}

TEST(Cobs, BoundaryRunLengths253To510) {
  for (size_t len : {253u, 254u, 255u, 256u, 507u, 508u, 509u, 510u}) {
    std::vector<std::byte> input(len);
    for (size_t i = 0; i < len; ++i) {
      input[i] = static_cast<std::byte>((i % 255) + 1);
    }

    std::vector<std::byte> encoded(MaxEncodedSize(len));
    auto enc = Encode(input, encoded);
    ASSERT_EQ(enc.status(), OkStatus());
    EXPECT_EQ(enc->back(), std::byte{0x00});

    if (len == 253u) {
      EXPECT_EQ(enc->size(), 255u);
      EXPECT_EQ((*enc)[0], std::byte{0xFE});
    } else if (len == 254u) {
      EXPECT_EQ(enc->size(), 256u);
      EXPECT_EQ((*enc)[0], std::byte{0xFF});
    } else if (len == 255u) {
      EXPECT_EQ(enc->size(), 258u);
      EXPECT_EQ((*enc)[0], std::byte{0xFF});
      EXPECT_EQ((*enc)[255], std::byte{0x02});
    } else if (len == 508u) {
      EXPECT_EQ(enc->size(), 511u);
      EXPECT_EQ((*enc)[0], std::byte{0xFF});
      EXPECT_EQ((*enc)[255], std::byte{0xFF});
    }

    std::vector<std::byte> decoded(len);
    auto dec = Decode(*enc, decoded);
    ASSERT_EQ(dec.status(), OkStatus());
    ASSERT_EQ(dec->size(), len);
    EXPECT_EQ(std::memcmp(dec->data(), input.data(), len), 0);

    input.push_back(std::byte{0x00});
    std::vector<std::byte> encoded_with_zero(MaxEncodedSize(input.size()));
    auto enc_z = Encode(input, encoded_with_zero);
    ASSERT_EQ(enc_z.status(), OkStatus());

    std::vector<std::byte> decoded_z(input.size());
    auto dec_z = Decode(*enc_z, decoded_z);
    ASSERT_EQ(dec_z.status(), OkStatus());
    ASSERT_EQ(dec_z->size(), input.size());
    EXPECT_EQ(std::memcmp(dec_z->data(), input.data(), input.size()), 0);
  }
}

TEST(Cobs, EncodeBufferEmpty) {
  EncodeBuffer<16> buffer;
  EXPECT_TRUE(buffer.empty());
  ConstByteSpan encoded = buffer.Encode();
  constexpr auto kExpected = bytes::Array<0x01, 0x00>();
  ASSERT_EQ(encoded.size(), kExpected.size());
  EXPECT_EQ(std::memcmp(encoded.data(), kExpected.data(), kExpected.size()), 0);
}

TEST(Cobs, EncodeBufferSingleBlockFastPath) {
  EncodeBuffer<64> buffer;
  for (size_t i = 0; i < 64; ++i) {
    buffer.push_back((i % 17 == 5) ? std::byte{0}
                                   : static_cast<std::byte>(i + 1));
  }
  std::array<std::byte, 64> original{};
  std::memcpy(original.data(), buffer.data(), 64);

  ConstByteSpan encoded = buffer.Encode();
  EXPECT_EQ(encoded.size(), 66u);
  EXPECT_EQ(encoded.back(), std::byte{0x00});

  std::array<std::byte, 64> decoded{};
  auto dec = Decode(encoded, decoded);
  ASSERT_EQ(dec.status(), OkStatus());
  ASSERT_EQ(dec->size(), 64u);
  EXPECT_EQ(std::memcmp(dec->data(), original.data(), 64), 0);
}

TEST(Cobs, EncodeBufferMultiBlockGeneralPath) {
  constexpr size_t kLen = 300;
  EncodeBuffer<kLen> buffer;
  buffer.resize(kLen);
  for (size_t i = 0; i < kLen; ++i) {
    buffer[i] = static_cast<std::byte>((i % 250) + 1);
  }
  std::array<std::byte, kLen> original{};
  std::memcpy(original.data(), buffer.data(), kLen);

  ConstByteSpan encoded = buffer.Encode();
  EXPECT_EQ(encoded.size(), kLen + 3);
  EXPECT_EQ(encoded.back(), std::byte{0x00});

  std::array<std::byte, kLen> decoded{};
  auto dec = Decode(encoded, decoded);
  ASSERT_EQ(dec.status(), OkStatus());
  ASSERT_EQ(dec->size(), kLen);
  EXPECT_EQ(std::memcmp(dec->data(), original.data(), kLen), 0);
}

TEST(Cobs, StreamerPiecewiseLargePayloadMatchesEncode) {
  constexpr auto kHeader = bytes::Array<0x38, 0x00, 0x01, 0x00>();
  constexpr auto kTrailer = bytes::Array<0xDE, 0xAD, 0xBE, 0xEF>();
  std::array<std::byte, 1024> body{};
  for (size_t i = 0; i < body.size(); ++i) {
    body[i] =
        (i % 73 == 12) ? std::byte{0} : static_cast<std::byte>((i % 254) + 1);
  }

  StreamerBuffer<> streamer;
  EXPECT_TRUE(streamer.AbortSequence().empty());

  std::vector<std::byte> streamed;
  size_t flushes = 0;
  for (ConstByteSpan seg :
       {ConstByteSpan(kHeader), ConstByteSpan(body), ConstByteSpan(kTrailer)}) {
    while (!seg.empty()) {
      seg = streamer.Write(seg);
      if (!seg.empty()) {
        ConstByteSpan chunk = streamer.Flush();
        ASSERT_FALSE(chunk.empty());
        ++flushes;
        EXPECT_FALSE(streamer.AbortSequence().empty());
        streamed.insert(streamed.end(), chunk.begin(), chunk.end());
      }
    }
  }
  ConstByteSpan tail = streamer.Finish();
  ASSERT_FALSE(tail.empty());
  streamed.insert(streamed.end(), tail.begin(), tail.end());
  EXPECT_GE(flushes, 3u);
  EXPECT_TRUE(streamer.AbortSequence().empty());

  std::vector<std::byte> combined;
  combined.insert(combined.end(), kHeader.begin(), kHeader.end());
  combined.insert(combined.end(), body.begin(), body.end());
  combined.insert(combined.end(), kTrailer.begin(), kTrailer.end());
  std::vector<std::byte> expected(MaxEncodedSize(combined.size()));
  auto enc = Encode(combined, expected);
  ASSERT_EQ(enc.status(), OkStatus());
  ASSERT_EQ(streamed.size(), enc->size());
  EXPECT_EQ(std::memcmp(streamed.data(), enc->data(), streamed.size()), 0);
}

TEST(Cobs, StreamerPiecewiseBoundaryLengthsWithLeadingDelimiter) {
  StreamerBuffer<> streamer;
  for (size_t total_len : {253u, 254u, 255u, 256u, 507u, 508u, 509u, 510u}) {
    std::vector<std::byte> full(total_len);
    for (size_t i = 0; i < total_len; ++i) {
      full[i] = static_cast<std::byte>((i % 254) + 1);
    }
    ConstByteSpan full_span(full);
    ConstByteSpan s0 = full_span.first(2);
    ConstByteSpan s1 = full_span.subspan(2, total_len - 6);
    ConstByteSpan s2 = full_span.subspan(total_len - 4);

    streamer.Reset(/*emit_leading_delimiter=*/true);
    std::vector<std::byte> out;
    for (ConstByteSpan seg : {s0, s1, s2}) {
      while (!seg.empty()) {
        seg = streamer.Write(seg);
        if (!seg.empty()) {
          ConstByteSpan chunk = streamer.Flush();
          ASSERT_FALSE(chunk.empty());
          out.insert(out.end(), chunk.begin(), chunk.end());
        }
      }
    }
    ConstByteSpan final_chunk = streamer.Finish();
    out.insert(out.end(), final_chunk.begin(), final_chunk.end());

    std::vector<std::byte> exp(MaxEncodedSize(total_len));
    auto exp_res = Encode(full_span, exp);
    ASSERT_EQ(exp_res.status(), OkStatus());
    ASSERT_EQ(out.size(), exp_res->size() + 1);
    EXPECT_EQ(out[0], std::byte{0x00});
    EXPECT_EQ(std::memcmp(out.data() + 1, exp_res->data(), exp_res->size()), 0);
  }
}

TEST(Cobs, DecodeRejectsEmptyAndMissingDelimiterInputs) {
  std::array<std::byte, 16> out{};
  EXPECT_EQ(Decode(ConstByteSpan(), out).status(), Status::DataLoss());

  constexpr auto kMissingDelimiter = bytes::Array<0x01>();
  EXPECT_EQ(Decode(kMissingDelimiter, out).status(), Status::DataLoss());

  constexpr auto kMissingDelimiterNonEmpty = bytes::Array<0x02, 0x11>();
  EXPECT_EQ(Decode(kMissingDelimiterNonEmpty, out).status(),
            Status::DataLoss());
}

TEST(Cobs, DecodeRejectsBareLeadingOrEmbeddedZeroDelimiters) {
  std::array<std::byte, 16> out{};
  constexpr auto kBareDelimiter = bytes::Array<0x00>();
  EXPECT_EQ(Decode(kBareDelimiter, out).status(), Status::DataLoss());

  constexpr auto kLeadingZero = bytes::Array<0x00, 0x01, 0x00>();
  EXPECT_EQ(Decode(kLeadingZero, out).status(), Status::DataLoss());

  constexpr auto kEmbeddedZero = bytes::Array<0x03, 0x11, 0x00, 0x00>();
  EXPECT_EQ(Decode(kEmbeddedZero, out).status(), Status::DataLoss());

  constexpr auto kTwoFrames =
      bytes::Array<0x02, 0x11, 0x00, 0x02, 0x22, 0x00>();
  EXPECT_EQ(Decode(kTwoFrames, out).status(), Status::DataLoss());
}

TEST(Cobs, DecodeRejectsTruncatedRuns) {
  std::array<std::byte, 16> out{};
  constexpr auto kTruncatedRun = bytes::Array<0x05, 0x11, 0x22, 0x00>();
  EXPECT_EQ(Decode(kTruncatedRun, out).status(), Status::DataLoss());

  std::vector<std::byte> truncated_max_run(255, std::byte{0x42});
  truncated_max_run[0] = std::byte{0xFF};
  truncated_max_run[254] = std::byte{0x00};
  std::vector<std::byte> big_out(260);
  EXPECT_EQ(Decode(truncated_max_run, big_out).status(), Status::DataLoss());
}

TEST(Cobs, DecodeAcceptsNonCanonicalTrailingEmptyBlockAfterMaxRun) {
  std::vector<std::byte> non_canon(257, std::byte{0x42});
  non_canon[0] = std::byte{0xFF};
  non_canon[255] = std::byte{0x01};
  non_canon[256] = std::byte{0x00};
  std::vector<std::byte> big_out(260);
  auto nc_res = Decode(non_canon, big_out);
  ASSERT_EQ(nc_res.status(), OkStatus());
  EXPECT_EQ(nc_res->size(), 254u);
}

TEST(CobsDecoder, ProcessSpanDecodesCompleteFrame) {
  std::array<std::byte, 300> payload{};
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  std::array<std::byte, MaxEncodedSize(payload.size())> wire_buf{};
  auto enc = Encode(payload, wire_buf);
  ASSERT_EQ(enc.status(), OkStatus());

  DecoderBuffer<300> decoder;
  size_t frames_received = 0;
  decoder.Process(*enc, [&](Result<ByteSpan> result) {
    ASSERT_EQ(result.status(), OkStatus());
    ++frames_received;
    ASSERT_EQ(result->size(), payload.size());
    EXPECT_EQ(std::memcmp(result->data(), payload.data(), payload.size()), 0);
  });
  EXPECT_EQ(frames_received, 1u);
}

TEST(CobsDecoder, ProcessByteByByteRecoversAfterIdleBytesAndTruncatedFrame) {
  std::array<std::byte, 300> payload{};
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFF);
  }

  std::array<std::byte, MaxEncodedSize(payload.size())> wire_buf{};
  auto enc = Encode(payload, wire_buf);
  ASSERT_EQ(enc.status(), OkStatus());

  DecoderBuffer<300> decoder;
  constexpr auto kPrefix = bytes::Array<0x00, 0x00, 0x05, 0x11, 0x00>();
  size_t error_count = 0;
  size_t frames_received = 0;
  for (std::byte b : kPrefix) {
    Result<ByteSpan> res = decoder.Process(b);
    if (!res.status().IsUnavailable()) {
      EXPECT_EQ(res.status(), Status::DataLoss());
      ++error_count;
    }
  }
  for (std::byte b : *enc) {
    Result<ByteSpan> res = decoder.Process(b);
    if (!res.status().IsUnavailable()) {
      ASSERT_EQ(res.status(), OkStatus());
      ++frames_received;
      ASSERT_EQ(res->size(), payload.size());
      EXPECT_EQ(std::memcmp(res->data(), payload.data(), payload.size()), 0);
    }
  }
  EXPECT_EQ(error_count, 1u);
  EXPECT_EQ(frames_received, 1u);
}

TEST(Cobs, EncodeTrailingZeroWithInsufficientSpaceForDelimiterFailsSafely) {
  constexpr auto kInput = bytes::Array<0x11, 0x00>();
  std::array<std::byte, 8> guard{};
  std::fill(guard.begin(), guard.end(), std::byte{0xAA});
  auto res = Encode(kInput, ByteSpan(guard.data(), 3));
  EXPECT_EQ(res.status(), Status::ResourceExhausted());
  for (size_t i = 3; i < guard.size(); ++i) {
    EXPECT_EQ(guard[i], std::byte{0xAA});
  }
}

TEST(Cobs, EncodeZeroFollowedByNonZeroWithInsufficientBufferFailsSafely) {
  constexpr auto kInput = bytes::Array<0x11, 0x00, 0x22>();
  std::array<std::byte, 8> guard{};
  std::fill(guard.begin(), guard.end(), std::byte{0xAA});
  auto res = Encode(kInput, ByteSpan(guard.data(), 3));
  EXPECT_EQ(res.status(), Status::ResourceExhausted());
  for (size_t i = 3; i < guard.size(); ++i) {
    EXPECT_EQ(guard[i], std::byte{0xAA});
  }
}

TEST(Cobs, EncodeNonZeroRunExceedingOutputBufferReturnsResourceExhausted) {
  constexpr auto kInput = bytes::
      Array<0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A>();
  std::array<std::byte, 8> out{};
  EXPECT_EQ(Encode(kInput, out).status(), Status::ResourceExhausted());
}

TEST(Cobs, StreamerWriteFlushFinishWithLeadingZeroBlock) {
  constexpr auto kInput = bytes::Array<0x00, 0x11>();
  std::array<std::byte, 16> buf{};
  Streamer streamer(buf);
  std::vector<std::byte> out;
  ConstByteSpan rem = kInput;
  while (!rem.empty()) {
    rem = streamer.Write(rem);
    if (!rem.empty()) {
      ByteSpan chunk = streamer.Flush();
      ASSERT_FALSE(chunk.empty());
      out.insert(out.end(), chunk.begin(), chunk.end());
    }
  }
  ByteSpan tail = streamer.Finish();
  out.insert(out.end(), tail.begin(), tail.end());
  constexpr auto kExpected = bytes::Array<0x01, 0x02, 0x11, 0x00>();
  ASSERT_EQ(out.size(), kExpected.size());
  EXPECT_EQ(std::memcmp(out.data(), kExpected.data(), kExpected.size()), 0);
}

TEST(Cobs, DecodeAndDecoderProcessWithInsufficientOutputBuffer) {
  constexpr auto kInput = bytes::Array<0x11, 0x22, 0x33, 0x00>();
  std::array<std::byte, 8> encoded_buf{};
  auto enc = Encode(kInput, encoded_buf);
  ASSERT_EQ(enc.status(), OkStatus());

  for (size_t out_cap : {0u, 1u, 2u, 3u}) {
    std::array<std::byte, 4> dec_buf{};
    EXPECT_EQ(Decode(*enc, ByteSpan(dec_buf.data(), out_cap)).status(),
              Status::ResourceExhausted());

    Decoder decoder(ByteSpan(dec_buf.data(), out_cap));
    size_t count = 0;
    decoder.Process(*enc, [&](Result<ByteSpan> r) {
      EXPECT_EQ(r.status(), Status::ResourceExhausted());
      ++count;
    });
    EXPECT_EQ(count, 1u);
  }
}

TEST(Cobs, EncodeInPlaceWithInsufficientBufferReturnsResourceExhausted) {
  std::array<std::byte, 4> buf{
      std::byte{0}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  EXPECT_EQ(EncodeInPlace(buf, 3).status(), Status::ResourceExhausted());
}

TEST(Cobs, EncodeBufferPartialPayloadInLargeCapacityBuffer) {
  EncodeBuffer<300> buffer;
  constexpr auto kSmall = bytes::Array<0x11, 0x00, 0x22, 0x33>();
  buffer.append(kSmall);
  EXPECT_EQ(buffer.size(), kSmall.size());
  ConstByteSpan encoded = buffer.Encode();
  constexpr auto kExpected = bytes::Array<0x02, 0x11, 0x03, 0x22, 0x33, 0x00>();
  ASSERT_EQ(encoded.size(), kExpected.size());
  EXPECT_EQ(std::memcmp(encoded.data(), kExpected.data(), kExpected.size()), 0);
}

TEST(Cobs, EncodeAndDecodeExactOutputBufferCapacitySweep) {
  const std::vector<std::vector<std::byte>> payloads = {
      {},
      {std::byte{0x00}},
      {std::byte{0x11}},
      {std::byte{0x11}, std::byte{0x00}},
      {std::byte{0x00}, std::byte{0x11}},
      {std::byte{0x11}, std::byte{0x00}, std::byte{0x22}},
      {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x00}},
      std::vector<std::byte>(254, std::byte{0x42}),
      std::vector<std::byte>(255, std::byte{0x42}),
  };
  for (const auto& payload : payloads) {
    std::vector<std::byte> full_enc_buf(MaxEncodedSize(payload.size()));
    auto full_enc = Encode(payload, full_enc_buf);
    ASSERT_EQ(full_enc.status(), OkStatus());
    const size_t actual_enc_len = full_enc->size();

    for (size_t cap = 0; cap < actual_enc_len; ++cap) {
      std::vector<std::byte> guard(cap + 8, std::byte{0xAA});
      auto r = Encode(payload, ByteSpan(guard.data(), cap));
      EXPECT_EQ(r.status(), Status::ResourceExhausted());
      for (size_t i = cap; i < guard.size(); ++i) {
        EXPECT_EQ(guard[i], std::byte{0xAA});
      }
    }
    {
      std::vector<std::byte> exact(actual_enc_len + 8, std::byte{0xAA});
      auto r = Encode(payload, ByteSpan(exact.data(), actual_enc_len));
      ASSERT_EQ(r.status(), OkStatus());
      EXPECT_EQ(r->size(), actual_enc_len);
      for (size_t i = actual_enc_len; i < exact.size(); ++i) {
        EXPECT_EQ(exact[i], std::byte{0xAA});
      }
    }

    for (size_t cap = 0; cap < payload.size(); ++cap) {
      std::vector<std::byte> guard(cap + 8, std::byte{0xAA});
      auto r = Decode(*full_enc, ByteSpan(guard.data(), cap));
      EXPECT_EQ(r.status(), Status::ResourceExhausted());
      for (size_t i = cap; i < guard.size(); ++i) {
        EXPECT_EQ(guard[i], std::byte{0xAA});
      }
    }
  }
}

TEST(Cobs, SizeHelpersAreConstexprAndExact) {
  static_assert(MaxEncodedSize(0) == 2);
  static_assert(MaxEncodedSize(1) == 3);
  static_assert(MaxEncodedSize(253) == 255);
  static_assert(MaxEncodedSize(254) == 256);
  static_assert(MaxEncodedSize(255) == 258);
  static_assert(MaxEncodedSize(508) == 511);
  static_assert(MaxEncodedSize(509) == 513);

  static_assert(MaxDecodedSize(0) == 0);
  static_assert(MaxDecodedSize(1) == 0);
  static_assert(MaxDecodedSize(2) == 0);
  static_assert(MaxDecodedSize(3) == 1);
  static_assert(MaxDecodedSize(256) == 254);

  EXPECT_EQ(MaxEncodedSize(0), 2u);
  EXPECT_EQ(MaxDecodedSize(2), 0u);
}

TEST(Cobs, MultiBlockCapacitySweepAndEncodeInPlaceBoundaries) {
  std::vector<std::byte> run_254(254, std::byte{0x42});
  std::vector<std::byte> run_254_then_zero = run_254;
  run_254_then_zero.push_back(std::byte{0x00});

  std::vector<std::byte> run_254_zero_254 = run_254_then_zero;
  run_254_zero_254.insert(
      run_254_zero_254.end(), run_254.begin(), run_254.end());

  std::vector<std::byte> run_508(508, std::byte{0x42});
  std::vector<std::byte> run_509(509, std::byte{0x42});

  for (const auto& payload :
       {run_254_then_zero, run_254_zero_254, run_508, run_509}) {
    std::vector<std::byte> full_enc_buf(MaxEncodedSize(payload.size()));
    auto full_enc = Encode(payload, full_enc_buf);
    ASSERT_EQ(full_enc.status(), OkStatus());
    const size_t actual_enc_len = full_enc->size();

    for (size_t cap = 0; cap < actual_enc_len; ++cap) {
      std::vector<std::byte> guard(cap + 4, std::byte{0xAA});
      auto r = Encode(payload, ByteSpan(guard.data(), cap));
      EXPECT_EQ(r.status(), Status::ResourceExhausted()) << "cap=" << cap;
      for (size_t i = cap; i < guard.size(); ++i) {
        ASSERT_EQ(guard[i], std::byte{0xAA}) << "cap=" << cap << " i=" << i;
      }
    }

    for (size_t extra : {0u, 1u, 37u}) {
      std::vector<std::byte> in_place(MaxEncodedSize(payload.size()) + extra,
                                      std::byte{0xEE});
      std::memcpy(in_place.data() + 1, payload.data(), payload.size());
      auto ip = EncodeInPlace(in_place, payload.size());
      ASSERT_EQ(ip.status(), OkStatus());
      ASSERT_EQ(ip->size(), actual_enc_len);
      EXPECT_EQ(std::memcmp(ip->data(), full_enc->data(), actual_enc_len), 0);
    }
  }
}

TEST(Cobs, EncodeInPlaceEmptyPayloadCapacityBoundaries) {
  std::array<std::byte, 10> buf{};
  EXPECT_EQ(EncodeInPlace(ByteSpan(buf.data(), 0), 0).status(),
            Status::ResourceExhausted());
  EXPECT_EQ(EncodeInPlace(ByteSpan(buf.data(), 1), 0).status(),
            Status::ResourceExhausted());
  auto ip2 = EncodeInPlace(ByteSpan(buf.data(), 2), 0);
  ASSERT_EQ(ip2.status(), OkStatus());
  ASSERT_EQ(ip2->size(), 2u);
  EXPECT_EQ((*ip2)[0], std::byte{0x01});
  EXPECT_EQ((*ip2)[1], std::byte{0x00});

  auto ip10 = EncodeInPlace(buf, 0);
  ASSERT_EQ(ip10.status(), OkStatus());
  ASSERT_EQ(ip10->size(), 2u);
  EXPECT_EQ((*ip10)[0], std::byte{0x01});
  EXPECT_EQ((*ip10)[1], std::byte{0x00});
}

TEST(Cobs, EncodeBufferIteratorsClearAndReuse) {
  EncodeBuffer<300> buf;
  constexpr auto kFirst = bytes::Array<0x01, 0x00, 0x02, 0x03>();
  buf.append(kFirst);
  EXPECT_EQ(buf.size(), 4u);
  EXPECT_EQ(buf.max_size(), 300u);
  EXPECT_EQ(buf.capacity(), 300u);
  EXPECT_FALSE(buf.empty());

  EXPECT_EQ(static_cast<size_t>(buf.end() - buf.begin()), 4u);
  EXPECT_EQ(static_cast<size_t>(buf.cend() - buf.cbegin()), 4u);
  auto it = buf.begin();
  EXPECT_EQ(*it, std::byte{0x01});
  ++it;
  EXPECT_EQ(*it, std::byte{0x00});
  it += 2;
  EXPECT_EQ(*it, std::byte{0x03});
  --it;
  EXPECT_EQ(*it, std::byte{0x02});

  ConstByteSpan enc1 = buf.Encode();
  constexpr auto kExp1 = bytes::Array<0x02, 0x01, 0x03, 0x02, 0x03, 0x00>();
  ASSERT_EQ(enc1.size(), kExp1.size());
  EXPECT_EQ(std::memcmp(enc1.data(), kExp1.data(), kExp1.size()), 0);

  buf.clear();
  EXPECT_TRUE(buf.empty());
  buf.data()[0] = std::byte{0x55};
  buf.data()[1] = std::byte{0x00};
  ConstByteSpan enc2 = buf.Encode(2);
  constexpr auto kExp2 = bytes::Array<0x02, 0x55, 0x01, 0x00>();
  ASSERT_EQ(enc2.size(), kExp2.size());
  EXPECT_EQ(std::memcmp(enc2.data(), kExp2.data(), kExp2.size()), 0);
}

TEST(Cobs, StreamerTinyBuffersNeverWriteOutOfBounds) {
  for (bool lead : {false, true}) {
    for (size_t cap : {0u, 1u, 2u, 3u}) {
      std::array<std::byte, 8> guard{};
      std::fill(guard.begin(), guard.end(), std::byte{0xAA});
      Streamer s(ByteSpan(guard.data(), cap), lead);

      constexpr auto kData = bytes::Array<0x11, 0x00, 0x22>();
      ConstByteSpan rem = s.Write(kData);
      const size_t min_needed = lead ? 3u : 2u;
      if (cap < min_needed) {
        EXPECT_EQ(rem.size(), kData.size());
        EXPECT_TRUE(s.Flush().empty());
        EXPECT_TRUE(s.Finish().empty());
      }
      for (size_t i = cap; i < guard.size(); ++i) {
        EXPECT_EQ(guard[i], std::byte{0xAA})
            << "lead=" << lead << " cap=" << cap << " i=" << i;
      }
    }
  }
}

TEST(Cobs, StreamerFlushBeforeBlockCompletesReturnsEmpty) {
  StreamerBuffer<> streamer(/*emit_leading_delimiter=*/true);
  EXPECT_TRUE(streamer.Flush().empty());
  EXPECT_TRUE(streamer.AbortSequence().empty());

  constexpr auto kPart = bytes::Array<0x11, 0x22>();
  EXPECT_TRUE(streamer.Write(kPart).empty());
  EXPECT_TRUE(streamer.Flush().empty());
  EXPECT_TRUE(streamer.AbortSequence().empty());

  EXPECT_TRUE(streamer.Write(ConstByteSpan()).empty());

  ByteSpan fin = streamer.Finish();
  constexpr auto kExpected = bytes::Array<0x00, 0x03, 0x11, 0x22, 0x00>();
  ASSERT_EQ(fin.size(), kExpected.size());
  EXPECT_EQ(std::memcmp(fin.data(), kExpected.data(), kExpected.size()), 0);
}

TEST(Cobs, StreamerByteByByteAcrossBlockBoundariesMatchesEncode) {
  std::vector<std::byte> payload(600);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = (i == 254 || i == 255 || i == 509)
                     ? std::byte{0x00}
                     : static_cast<std::byte>((i % 254) + 1);
  }
  StreamerBuffer<> streamer;
  std::vector<std::byte> streamed;
  for (size_t i = 0; i < payload.size(); ++i) {
    ConstByteSpan seg(&payload[i], 1);
    while (!seg.empty()) {
      seg = streamer.Write(seg);
      if (!seg.empty()) {
        ByteSpan chunk = streamer.Flush();
        ASSERT_FALSE(chunk.empty());
        streamed.insert(streamed.end(), chunk.begin(), chunk.end());
      }
    }
    EXPECT_TRUE(streamer.Write(ConstByteSpan()).empty());
  }
  ByteSpan tail = streamer.Finish();
  streamed.insert(streamed.end(), tail.begin(), tail.end());

  std::vector<std::byte> expected(MaxEncodedSize(payload.size()));
  auto enc = Encode(payload, expected);
  ASSERT_EQ(enc.status(), OkStatus());
  ASSERT_EQ(streamed.size(), enc->size());
  EXPECT_EQ(std::memcmp(streamed.data(), enc->data(), streamed.size()), 0);
}

TEST(CobsDecoder, ZeroCapacityBufferDecodesEmptyFrameAndRejectsNonEmptyFrame) {
  Decoder zero_dec(ByteSpan{});
  EXPECT_FALSE(zero_dec.in_progress());
  EXPECT_EQ(zero_dec.Process(std::byte{0x00}).status(), Status::Unavailable());
  EXPECT_FALSE(zero_dec.in_progress());

  EXPECT_EQ(zero_dec.Process(std::byte{0x01}).status(), Status::Unavailable());
  EXPECT_TRUE(zero_dec.in_progress());
  auto empty_res = zero_dec.Process(std::byte{0x00});
  ASSERT_EQ(empty_res.status(), OkStatus());
  EXPECT_TRUE(empty_res->empty());
  EXPECT_FALSE(zero_dec.in_progress());

  EXPECT_EQ(zero_dec.Process(std::byte{0x01}).status(), Status::Unavailable());
  EXPECT_EQ(zero_dec.Process(std::byte{0x01}).status(), Status::Unavailable());
  EXPECT_EQ(zero_dec.Process(std::byte{0x00}).status(),
            Status::ResourceExhausted());
  EXPECT_FALSE(zero_dec.in_progress());
}

TEST(CobsDecoder, ProcessSpanHandlesMultipleBackToBackValidAndInvalidFrames) {
  constexpr auto kMultiStream =
      bytes::Array<0x00,  // idle delimiter
                   0x02,
                   0xAA,
                   0x00,  // valid 1-byte frame {0xAA}
                   0x04,
                   0xBB,
                   0x00,  // truncated frame -> DataLoss
                   0x04,
                   0x01,
                   0x02,
                   0x03,
                   0x00,  // 3-byte frame -> ResourceExhausted
                   0x01,
                   0x01,
                   0x00  // valid 1-byte frame {0x00}
                   >();
  DecoderBuffer<2> dec;
  std::vector<Status> statuses;
  std::vector<std::byte> first_bytes;
  dec.Process(kMultiStream, [&](Result<ByteSpan> res) {
    statuses.push_back(res.status());
    if (res.ok() && !res->empty()) {
      first_bytes.push_back((*res)[0]);
    }
  });
  ASSERT_EQ(statuses.size(), 4u);
  EXPECT_EQ(statuses[0], OkStatus());
  EXPECT_EQ(statuses[1], Status::DataLoss());
  EXPECT_EQ(statuses[2], Status::ResourceExhausted());
  EXPECT_EQ(statuses[3], OkStatus());
  ASSERT_EQ(first_bytes.size(), 2u);
  EXPECT_EQ(first_bytes[0], std::byte{0xAA});
  EXPECT_EQ(first_bytes[1], std::byte{0x00});
}

}  // namespace
}  // namespace pw::cobs
