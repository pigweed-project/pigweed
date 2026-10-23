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
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

#include "pw_fuzzer/fuzztest.h"
#include "pw_hex_dump/log_bytes.h"
#include "pw_log/levels.h"
#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::experimental::websocket::frame_protocol {
namespace {

using namespace std::string_view_literals;

constexpr auto AsBytes(std::string_view value) { return as_bytes(span(value)); }

bool Compare(span<const std::byte> actual,
             span<const std::byte> expected,
             const char* name) {
  if (actual.size() != expected.size() ||
      !std::equal(actual.begin(), actual.end(), expected.begin())) {
    PW_LOG_ERROR("%s actual   (%zu bytes):", name, actual.size());
    dump::LogBytes(PW_LOG_LEVEL_ERROR, as_bytes(actual));
    PW_LOG_ERROR("%s expected (%zu bytes):", name, expected.size());
    dump::LogBytes(PW_LOG_LEVEL_ERROR, as_bytes(expected));
    return false;
  }

  return true;
}

// Used for curated test cases, where the encoded and decoded representations
// are known ahead of time.
struct FrameCodecCuratedTestCase {
  struct {
    Status status = Status::Code::PW_STATUS_OK;
    span<const std::byte> data;
  } encoded;

  struct {
    Status status = Status::Code::PW_STATUS_OK;
    size_t remaining = 0;

    bool final_fragment = true;
    Frame::Opcode opcode = Frame::Opcode::kText;
    std::optional<span<const std::byte, 4>> mask = std::nullopt;
    span<const std::byte> payload;
  } decoded;
};

void CheckEncode(const FrameCodecCuratedTestCase& test_case) {
  std::array<std::byte, 1024> output{};
  ASSERT_TRUE(test_case.encoded.data.size() <= output.size());
  memset(output.data(), 0xff, output.size());

  memcpy(output.data(),
         test_case.encoded.data.data(),
         test_case.encoded.data.size());

  const auto result =
      EncodeFrame({.final_fragment = test_case.decoded.final_fragment,
                   .opcode = test_case.decoded.opcode,
                   .mask = test_case.decoded.mask,
                   .payload = test_case.decoded.payload},
                  span(output));

  ASSERT_EQ(result.status(), test_case.encoded.status);

  if (!result.ok()) {
    return;
  }

  ASSERT_TRUE(test_case.decoded.remaining <= test_case.encoded.data.size());
  ASSERT_TRUE(Compare(
      *result,
      test_case.encoded.data.subspan(
          0, test_case.encoded.data.size() - test_case.decoded.remaining),
      "encoded"));
}

void CheckDecode(const FrameCodecCuratedTestCase& test_case) {
  // Copying is necessary since decoding in place may require modifying the
  // input to unmask it.
  std::array<std::byte, 1024> input{};
  ASSERT_TRUE(test_case.encoded.data.size() <= input.size());
  ASSERT_TRUE(test_case.decoded.remaining <= test_case.encoded.data.size());
  memset(input.data(), 0xff, input.size());
  memcpy(input.data(),
         test_case.encoded.data.data(),
         test_case.encoded.data.size());

  span<std::byte> remaining(
      as_writable_bytes(span(input.data(), test_case.encoded.data.size())));
  const auto result = DecodeFrame(remaining);

  ASSERT_EQ(result.status(), test_case.decoded.status);
  ASSERT_EQ(remaining.size(), test_case.decoded.remaining);

  if (!result.ok()) {
    return;
  }

  ASSERT_EQ(result->final_fragment, test_case.decoded.final_fragment);
  ASSERT_EQ(result->opcode, test_case.decoded.opcode);
  ASSERT_EQ(result->mask.has_value(), test_case.decoded.mask.has_value());
  if (result->mask.has_value()) {
    ASSERT_TRUE(
        Compare(result->mask.value(), test_case.decoded.mask.value(), "mask"));
  }
  ASSERT_TRUE(Compare(result->payload, test_case.decoded.payload, "decoded"));
}

void Check(const FrameCodecCuratedTestCase& test_case) {
  CheckEncode(test_case);
  CheckDecode(test_case);
}

TEST(WebsocketFrameProtocol, EncodeDecodeCuratedRFCSingleFrameUnmaskedText) {
  // From RFC 6455 "5.7 Examples": single-frame unmasked text message
  static const auto test_case = FrameCodecCuratedTestCase{
      .encoded{.data = AsBytes("\x81\x05\x48\x65\x6c\x6c\x6f"sv)},
      .decoded{.opcode = Frame::Opcode::kText, .payload = AsBytes("Hello"sv)}};
  Check(test_case);
}

TEST(WebsocketFrameProtocol, EncodeDecodeCuratedRFCSingleFrameUnmaskedPing) {
  // From RFC 6455 "5.7 Examples": single-frame unmasked ping
  static const auto test_case = FrameCodecCuratedTestCase{
      .encoded{.data = AsBytes("\x89\x05\x48\x65\x6c\x6c\x6f"sv)},
      .decoded{.opcode = Frame::Opcode::kPing, .payload = AsBytes("Hello"sv)}};
  Check(test_case);
}

TEST(WebsocketFrameProtocol, EncodeDecodeCuratedRFCSingleFrameMaskedPong) {
  // From RFC 6455 "5.7 Examples": single-frame unmasked ping
  static const auto test_case = FrameCodecCuratedTestCase{
      .encoded{.data =
                   AsBytes("\x8a\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58"sv)},
      .decoded{.opcode = Frame::Opcode::kPong,
               .mask = AsBytes("\x37\xfa\x21\x3d"sv).subspan<0, 4>(),
               .payload = AsBytes("Hello"sv)}};
  Check(test_case);
}

TEST(WebsocketFrameProtocol, EncodeDecodeCuratedRFCFragmentedUnmaskedText) {
  // From RFC 6455 "5.7 Examples": fragmented unmasked text message
  constexpr auto kEncodedData =
      // Frame 1 "Hel" ...
      "\x01\x03\x48\x65\x6c"
      // Frame 2 ... "lo"
      "\x80\x02\x6c\x6f"sv;

  static const auto frame1 = FrameCodecCuratedTestCase{
      .encoded{
          .data = AsBytes(kEncodedData),
      },
      .decoded{
          .remaining = 4,
          .final_fragment = false,
          .payload = AsBytes("Hel"sv),
      },
  };
  Check(frame1);

  static const auto frame2 = FrameCodecCuratedTestCase{
      .encoded{
          .data = AsBytes(kEncodedData).subspan(5),
      },
      .decoded{
          .opcode = Frame::Opcode::kContinuation,
          .payload = AsBytes("lo"sv),
      },
  };
  Check(frame2);
}

TEST(WebsocketFrameProtocol, EncodeDecodeCuratedSingleFrameMaxOneByteSize) {
  // Maximum payload size for a frame with one byte length.
  static std::array<uint8_t, 128> kEncodedData{0x82, 0x7d};
  kEncodedData[2 + 124] = 0x55;
  kEncodedData[2 + 125] = 0xff;

  static std::array<uint8_t, 125> kDecodedPayload{};
  kDecodedPayload[124] = 0x55;

  static const auto test_case = FrameCodecCuratedTestCase{
      .encoded{.data = as_bytes(span(kEncodedData))},
      .decoded{.remaining = 1,
               .opcode = Frame::Opcode::kBinary,
               .payload = as_bytes(span(kDecodedPayload))}};
  Check(test_case);
}

TEST(WebsocketFrameProtocol, EncodeDecodeCuratedSingleFrameMinTwoByteSize) {
  // Minimum payload size for a frame with a two byte length.
  static std::array<uint8_t, 131> kEncodedData{0x82, 0x7e, 0x00, 0x7e};
  kEncodedData[4 + 125] = 0x55;
  kEncodedData[4 + 126] = 0xff;

  static std::array<uint8_t, 126> kDecodedPayload{};
  kDecodedPayload[125] = 0x55;

  static const auto test_case = FrameCodecCuratedTestCase{
      .encoded{.data = as_bytes(span(kEncodedData))},
      .decoded{.remaining = 1,
               .opcode = Frame::Opcode::kBinary,
               .payload = as_bytes(span(kDecodedPayload))}};
  Check(test_case);
}

TEST(WebsocketFrameProtocol, DecodeOfControlFrameWithIllegalPayloadSizeFails) {
  // Control frames must have less than 126 bytes of payload.
  static std::array<uint8_t, 131> kEncodedData{0x88, 0x7e, 0x00, 0x7e};
  span<std::byte> remaining = as_writable_bytes(span(kEncodedData));
  const auto decoded = DecodeFrame(remaining);
  ASSERT_TRUE(!decoded.ok());
}

TEST(WebsocketFrameProtocol, DecodeOfControlFrameWithFragmentationFails) {
  // Control frames must be final (no fragments).
  static std::array<uint8_t, 131> kEncodedData{0x08, 0x7d};
  span<std::byte> remaining = as_writable_bytes(span(kEncodedData));
  const auto decoded = DecodeFrame(remaining);
  ASSERT_TRUE(!decoded.ok());
}

TEST(WebsocketFrameProtocol, EncodeOfControlFrameWithIllegalPayloadSizeFails) {
  // Control frames must have less than 126 bytes of payload.
  static std::array<std::byte, 126> payload;
  static std::array<std::byte, 256> output;
  const auto encoded = EncodeFrame(Frame{.final_fragment = true,
                                         .opcode = Frame::Opcode::kClose,
                                         .payload = span(payload)},
                                   span(output));
  ASSERT_TRUE(!encoded.ok());
}

TEST(WebsocketFrameProtocol, EncodeOfControlFrameWithFragmentationFails) {
  // Control frames must be final (no fragments).
  static std::array<std::byte, 125> payload;
  static std::array<std::byte, 256> output;
  const auto encoded = EncodeFrame(Frame{.final_fragment = false,
                                         .opcode = Frame::Opcode::kClose,
                                         .payload = span(payload)},
                                   span(output));
  ASSERT_TRUE(!encoded.ok());
}

void EncodeDecodeForFuzz(bool final_fragment,
                         Frame::Opcode opcode,
                         std::optional<std::array<std::byte, 4>> mask,
                         span<const std::byte> payload) {
  const auto frame = Frame{
      .final_fragment = final_fragment,
      .opcode = opcode,
      .mask = mask,
      .payload = payload,
  };

  std::array<std::byte, 2048> encoded{};

  auto result = EncodeFrame(frame, encoded, /* disable_logging */ true);
  if (!result.ok()) {
    return;
  }

  auto remaining = span(encoded.data(), result->size());
  auto decoded = DecodeFrame(remaining, /* disable_logging */ true);

  ASSERT_TRUE(decoded.ok());
  ASSERT_EQ(remaining.size(), 0U);

  ASSERT_EQ(decoded->final_fragment, frame.final_fragment);
  ASSERT_EQ(decoded->opcode, frame.opcode);
  ASSERT_EQ(decoded->mask.has_value(), frame.mask.has_value());
  if (decoded->mask.has_value()) {
    ASSERT_TRUE(Compare(decoded->mask.value(), frame.mask.value(), "mask"));
  }
  ASSERT_TRUE(Compare(decoded->payload, frame.payload, "decoded"));
}

FUZZ_TEST(WebsocketFrameProtocol, EncodeDecodeForFuzz)
    .WithDomains(
        fuzzer::Arbitrary<bool>(),
        fuzzer::ElementOf<Frame::Opcode>({
            Frame::Opcode::kBinary,
            Frame::Opcode::kClose,
            Frame::Opcode::kContinuation,
            Frame::Opcode::kPing,
            Frame::Opcode::kPong,
            Frame::Opcode::kText,
        }),
        fuzzer::OptionalOf(fuzzer::ArrayOf<4>(fuzzer::Arbitrary<std::byte>())),
        fuzzer::VectorOf<4>(fuzzer::Arbitrary<std::byte>()));

}  // namespace
}  // namespace pw::experimental::websocket::frame_protocol
