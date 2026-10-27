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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_cobs/cobs.h"
#include "pw_perf_test/perf_test.h"

namespace pw::cobs {
namespace {

template <size_t kSize>
constexpr std::array<std::byte, kSize> MakeNonZeroPayload() {
  std::array<std::byte, kSize> data{};
  for (size_t i = 0; i < kSize; ++i) {
    data[i] = static_cast<std::byte>(1 + (i % 254));
  }
  return data;
}

template <size_t kSize>
constexpr std::array<std::byte, kSize> MakeMixedPayload() {
  std::array<std::byte, kSize> data{};
  for (size_t i = 0; i < kSize; ++i) {
    // Full-period linear congruence (gcd(13, 256) == 1): hits 0x00 once every
    // 256 bytes, exercising both 254-byte max-run blocks and zero-terminated
    // blocks.
    data[i] = static_cast<std::byte>((i * 13) & 0xFF);
  }
  return data;
}

constexpr auto kNonZero254 = MakeNonZeroPayload<254>();
constexpr auto kNonZero1024 = MakeNonZeroPayload<1024>();
constexpr auto kMixed1024 = MakeMixedPayload<1024>();

void Encode254NonZero(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kNonZero254.size())> encoded{};
  while (state.KeepRunning()) {
    Result<ByteSpan> res = Encode(kNonZero254, encoded);
    PW_CHECK_OK(res.status());
  }
}
PW_PERF_TEST(CobsEncode_254NonZero, Encode254NonZero);

void Encode1024NonZero(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kNonZero1024.size())> encoded{};
  while (state.KeepRunning()) {
    Result<ByteSpan> res = Encode(kNonZero1024, encoded);
    PW_CHECK_OK(res.status());
  }
}
PW_PERF_TEST(CobsEncode_1024NonZero, Encode1024NonZero);

void Encode1024Mixed(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kMixed1024.size())> encoded{};
  while (state.KeepRunning()) {
    Result<ByteSpan> res = Encode(kMixed1024, encoded);
    PW_CHECK_OK(res.status());
  }
}
PW_PERF_TEST(CobsEncode_1024Mixed, Encode1024Mixed);

void EncodeInPlace254NonZero(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kNonZero254.size())> buffer{};
  std::memcpy(buffer.data() + 1, kNonZero254.data(), kNonZero254.size());
  while (state.KeepRunning()) {
    Result<ByteSpan> res = EncodeInPlace(buffer, kNonZero254.size());
    PW_CHECK_OK(res.status());
  }
}
PW_PERF_TEST(CobsEncodeInPlace_254NonZero, EncodeInPlace254NonZero);

void Decode254NonZero(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kNonZero254.size())> encoded_buf{};
  const Result<ByteSpan> encoded = Encode(kNonZero254, encoded_buf);
  PW_CHECK_OK(encoded.status());
  std::array<std::byte, kNonZero254.size()> decoded{};
  while (state.KeepRunning()) {
    Result<ByteSpan> res = Decode(*encoded, decoded);
    PW_CHECK_OK(res.status());
  }
}
PW_PERF_TEST(CobsDecode_254NonZero, Decode254NonZero);

void Decode1024Mixed(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kMixed1024.size())> encoded_buf{};
  const Result<ByteSpan> encoded = Encode(kMixed1024, encoded_buf);
  PW_CHECK_OK(encoded.status());
  std::array<std::byte, kMixed1024.size()> decoded{};
  while (state.KeepRunning()) {
    Result<ByteSpan> res = Decode(*encoded, decoded);
    PW_CHECK_OK(res.status());
  }
}
PW_PERF_TEST(CobsDecode_1024Mixed, Decode1024Mixed);

void StreamingDecoder1024Mixed(pw::perf_test::State& state) {
  std::array<std::byte, MaxEncodedSize(kMixed1024.size())> encoded_buf{};
  const Result<ByteSpan> encoded = Encode(kMixed1024, encoded_buf);
  PW_CHECK_OK(encoded.status());
  DecoderBuffer<kMixed1024.size()> decoder;
  while (state.KeepRunning()) {
    size_t frames = 0;
    decoder.Process(*encoded, [&](Result<ByteSpan> frame) {
      PW_CHECK_OK(frame.status());
      ++frames;
    });
    PW_CHECK_UINT_EQ(frames, 1u);
  }
}
PW_PERF_TEST(CobsStreamingDecoder_1024Mixed, StreamingDecoder1024Mixed);

}  // namespace
}  // namespace pw::cobs
