// Copyright 2019 The Pigweed Authors
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

#include "pw_varint/varint.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <limits>

#include "pw_fuzzer/fuzztest.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace pw::varint {
namespace {

// Test fixtures.

extern "C" {

// Functions defined in varint_test.c which call the varint API from C.
size_t pw_varint_CallEncode32(uint32_t value,
                              void* out_encoded,
                              size_t out_encoded_size);
size_t pw_varint_CallEncode64(uint64_t value,
                              void* out_encoded,
                              size_t out_encoded_size);
size_t pw_varint_CallZigZagAndVarintEncode64(int64_t value,
                                             void* out_encoded,
                                             size_t out_encoded_size);
size_t pw_varint_CallDecode32(const void* encoded,
                              size_t encoded_size,
                              uint32_t* out_value);
size_t pw_varint_CallDecode64(const void* encoded,
                              size_t encoded_size,
                              uint64_t* out_value);
size_t pw_varint_CallZigZagAndVarintDecode64(const void* encoded,
                                             size_t encoded_size,
                                             int64_t* out_value);

}  // extern "C"

// Constant expression that writes `data` from a string to a span of `bytes`.
template <size_t kStringSize>
constexpr ConstByteSpan Write(const char (&data)[kStringSize], ByteSpan bytes) {
  constexpr size_t kSizeBytes = kStringSize - 1;
  static_assert(kSizeBytes <= kMaxVarint64SizeBytes);
  PW_ASSERT(kSizeBytes <= bytes.size());
  for (size_t i = 0; i < kSizeBytes; ++i) {
    bytes[i] = static_cast<std::byte>(data[i]);
  }
  return bytes.subspan(0, kSizeBytes);
}

// Constant expression that returns whether the `data` in a string matches a
// span of `bytes`.
template <size_t kStringSize>
[[nodiscard]] constexpr bool Compare(const char (&data)[kStringSize],
                                     ConstByteSpan bytes) {
  constexpr size_t kSizeBytes = kStringSize - 1;
  if (bytes.size() < kSizeBytes) {
    return false;
  }
  for (size_t i = 0; i < kSizeBytes; ++i) {
    if (data[i] != static_cast<char>(bytes[i])) {
      return false;
    }
  }
  return true;
}

// Unit tests.

PW_CONSTEXPR_TEST(Varint, EncodeSizeUnsigned32_SmallSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(0), buffer));
  PW_TEST_EXPECT_EQ(std::byte{0}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(1), buffer));
  PW_TEST_EXPECT_EQ(std::byte{1}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(2), buffer));
  PW_TEST_EXPECT_EQ(std::byte{2}, buffer[0]);
});

TEST(Varint, EncodeSizeUnsigned32_SmallSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(0), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{0}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(1), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{1}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(2), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{2}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeUnsigned32_LargeSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(63), buffer));
  PW_TEST_EXPECT_EQ(std::byte{63}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(64), buffer));
  PW_TEST_EXPECT_EQ(std::byte{64}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(126), buffer));
  PW_TEST_EXPECT_EQ(std::byte{126}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT32_C(127), buffer));
  PW_TEST_EXPECT_EQ(std::byte{127}, buffer[0]);
});

TEST(Varint, EncodeSizeUnsigned32_LargeSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(63), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{63}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(64), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{64}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(126), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{126}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT32_C(127), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{127}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeUnsigned32_MultiByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(2u, Encode(UINT32_C(128), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x80\x01", buffer));
  PW_TEST_ASSERT_EQ(2u, Encode(UINT32_C(129), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x81\x01", buffer));

  PW_TEST_ASSERT_EQ(5u,
                    Encode(std::numeric_limits<uint32_t>::max() - 1, buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xfe\xff\xff\xff\x0f", buffer));

  PW_TEST_ASSERT_EQ(5u, Encode(std::numeric_limits<uint32_t>::max(), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xff\xff\xff\xff\x0f", buffer));
});

TEST(Varint, EncodeSizeUnsigned32_MultiByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(2u, pw_varint_CallEncode64(UINT32_C(128), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x80\x01", buffer, 2), 0);
  ASSERT_EQ(2u, pw_varint_CallEncode64(UINT32_C(129), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x81\x01", buffer, 2), 0);

  ASSERT_EQ(
      5u,
      pw_varint_CallEncode32(
          std::numeric_limits<uint32_t>::max() - 1, buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(5u,
            pw_varint_CallEncode32(
                std::numeric_limits<uint32_t>::max(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\x0f", buffer, 5), 0);

  // Call with 64-bit function as well
  ASSERT_EQ(2u, pw_varint_CallEncode64(UINT32_C(128), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x80\x01", buffer, 2), 0);
  ASSERT_EQ(2u, pw_varint_CallEncode64(UINT32_C(129), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x81\x01", buffer, 2), 0);

  ASSERT_EQ(
      5u,
      pw_varint_CallEncode64(
          std::numeric_limits<uint32_t>::max() - 1, buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(5u,
            pw_varint_CallEncode64(
                std::numeric_limits<uint32_t>::max(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\x0f", buffer, 5), 0);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeSigned32_SmallSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(0), buffer));
  PW_TEST_EXPECT_EQ(std::byte{0}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(-1), buffer));
  PW_TEST_EXPECT_EQ(std::byte{1}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(1), buffer));
  PW_TEST_EXPECT_EQ(std::byte{2}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(-2), buffer));
  PW_TEST_EXPECT_EQ(std::byte{3}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(2), buffer));
  PW_TEST_EXPECT_EQ(std::byte{4}, buffer[0]);
});

TEST(Varint, EncodeSizeSigned32_SmallSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(0), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{0}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(-1), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{1}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(1), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{2}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(-2), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{3}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(2), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{4}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeSigned32_LargeSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(-63), buffer));
  PW_TEST_EXPECT_EQ(std::byte{125}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(63), buffer));
  PW_TEST_EXPECT_EQ(std::byte{126}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT32_C(-64), buffer));
  PW_TEST_EXPECT_EQ(std::byte{127}, buffer[0]);
});

TEST(Varint, EncodeSizeSigned32_LargeSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(-63), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{125}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(63), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{126}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(-64), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{127}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeSigned32_MultiByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(2u, Encode(INT32_C(64), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x80\x01", buffer));
  PW_TEST_ASSERT_EQ(2u, Encode(INT32_C(-65), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x81\x01", buffer));
  PW_TEST_ASSERT_EQ(2u, Encode(INT32_C(65), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x82\x01", buffer));

  PW_TEST_ASSERT_EQ(5u, Encode(std::numeric_limits<int32_t>::min(), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xff\xff\xff\xff\x0f", buffer));

  PW_TEST_ASSERT_EQ(5u, Encode(std::numeric_limits<int32_t>::max(), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xfe\xff\xff\xff\x0f", buffer));
});

TEST(Varint, EncodeSizeSigned32_MultiByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(2u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(64), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x80\x01", buffer, 2), 0);
  ASSERT_EQ(2u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(-65), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x81\x01", buffer, 2), 0);
  ASSERT_EQ(2u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT32_C(65), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x82\x01", buffer, 2), 0);

  ASSERT_EQ(5u,
            pw_varint_CallZigZagAndVarintEncode64(
                std::numeric_limits<int32_t>::min(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(5u,
            pw_varint_CallZigZagAndVarintEncode64(
                std::numeric_limits<int32_t>::max(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\x0f", buffer, 5), 0);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeUnsigned64_SmallSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(0), buffer));
  PW_TEST_EXPECT_EQ(std::byte{0}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(1), buffer));
  PW_TEST_EXPECT_EQ(std::byte{1}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(2), buffer));
  PW_TEST_EXPECT_EQ(std::byte{2}, buffer[0]);
});

TEST(Varint, EncodeSizeUnsigned64_SmallSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(0), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{0}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(1), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{1}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(2), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{2}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeUnsigned64_LargeSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(63), buffer));
  PW_TEST_EXPECT_EQ(std::byte{63}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(64), buffer));
  PW_TEST_EXPECT_EQ(std::byte{64}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(126), buffer));
  PW_TEST_EXPECT_EQ(std::byte{126}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(UINT64_C(127), buffer));
  PW_TEST_EXPECT_EQ(std::byte{127}, buffer[0]);
});

TEST(Varint, EncodeSizeUnsigned64_LargeSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(63), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{63}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(64), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{64}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(126), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{126}, buffer[0]);
  ASSERT_EQ(1u, pw_varint_CallEncode64(UINT64_C(127), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{127}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeUnsigned64_MultiByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(2u, Encode(UINT64_C(128), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x80\x01", buffer));
  PW_TEST_ASSERT_EQ(2u, Encode(UINT64_C(129), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x81\x01", buffer));

  PW_TEST_ASSERT_EQ(5u,
                    Encode(std::numeric_limits<uint32_t>::max() - 1, buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xfe\xff\xff\xff\x0f", buffer));

  PW_TEST_ASSERT_EQ(5u, Encode(std::numeric_limits<uint32_t>::max(), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xff\xff\xff\xff\x0f", buffer));

  PW_TEST_ASSERT_EQ(10u,
                    Encode(std::numeric_limits<uint64_t>::max() - 1, buffer));
  PW_TEST_EXPECT_TRUE(
      Compare("\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer));

  PW_TEST_ASSERT_EQ(10u, Encode(std::numeric_limits<uint64_t>::max(), buffer));
  PW_TEST_EXPECT_TRUE(
      Compare("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer));
});

TEST(Varint, EncodeSizeUnsigned64_MultiByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(2u, pw_varint_CallEncode64(UINT64_C(128), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x80\x01", buffer, 2), 0);
  ASSERT_EQ(2u, pw_varint_CallEncode64(UINT64_C(129), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x81\x01", buffer, 2), 0);

  ASSERT_EQ(
      5u,
      pw_varint_CallEncode64(
          std::numeric_limits<uint32_t>::max() - 1, buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(5u,
            pw_varint_CallEncode64(
                std::numeric_limits<uint32_t>::max(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(
      10u,
      pw_varint_CallEncode64(
          std::numeric_limits<uint64_t>::max() - 1, buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer, 10),
            0);

  ASSERT_EQ(10u,
            pw_varint_CallEncode64(
                std::numeric_limits<uint64_t>::max(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer, 10),
            0);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeSigned64_SmallSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(0), buffer));
  PW_TEST_EXPECT_EQ(std::byte{0}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(-1), buffer));
  PW_TEST_EXPECT_EQ(std::byte{1}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(1), buffer));
  PW_TEST_EXPECT_EQ(std::byte{2}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(-2), buffer));
  PW_TEST_EXPECT_EQ(std::byte{3}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(2), buffer));
  PW_TEST_EXPECT_EQ(std::byte{4}, buffer[0]);
});

TEST(Varint, EncodeSizeSigned64_SmallSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(0), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{0}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(-1), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{1}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(1), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{2}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(-2), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{3}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(2), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{4}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeSigned64_LargeSingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(-63), buffer));
  PW_TEST_EXPECT_EQ(std::byte{125}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(63), buffer));
  PW_TEST_EXPECT_EQ(std::byte{126}, buffer[0]);
  PW_TEST_ASSERT_EQ(1u, Encode(INT64_C(-64), buffer));
  PW_TEST_EXPECT_EQ(std::byte{127}, buffer[0]);
});

TEST(Varint, EncodeSizeSigned64_LargeSingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(-63), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{125}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(63), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{126}, buffer[0]);
  ASSERT_EQ(1u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(-64), buffer, sizeof(buffer)));
  EXPECT_EQ(std::byte{127}, buffer[0]);
}

PW_CONSTEXPR_TEST(Varint, EncodeSizeSigned64_MultiByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  PW_TEST_ASSERT_EQ(2u, Encode(INT64_C(64), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x80\x01", buffer));
  PW_TEST_ASSERT_EQ(2u, Encode(INT64_C(-65), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x81\x01", buffer));
  PW_TEST_ASSERT_EQ(2u, Encode(INT64_C(65), buffer));
  PW_TEST_EXPECT_TRUE(Compare("\x82\x01", buffer));

  PW_TEST_ASSERT_EQ(
      5u,
      Encode(static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
             buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xff\xff\xff\xff\x0f", buffer));

  PW_TEST_ASSERT_EQ(
      5u,
      Encode(static_cast<int64_t>(std::numeric_limits<int32_t>::max()),
             buffer));
  PW_TEST_EXPECT_TRUE(Compare("\xfe\xff\xff\xff\x0f", buffer));

  PW_TEST_ASSERT_EQ(10u, Encode(std::numeric_limits<int64_t>::min(), buffer));
  PW_TEST_EXPECT_TRUE(
      Compare("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer));

  PW_TEST_ASSERT_EQ(10u, Encode(std::numeric_limits<int64_t>::max(), buffer));
  PW_TEST_EXPECT_TRUE(
      Compare("\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer));
});

TEST(Varint, EncodeSizeSigned64_MultiByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(2u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(64), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x80\x01", buffer, 2), 0);
  ASSERT_EQ(2u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(-65), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x81\x01", buffer, 2), 0);
  ASSERT_EQ(2u,
            pw_varint_CallZigZagAndVarintEncode64(
                INT64_C(65), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\x82\x01", buffer, 2), 0);

  ASSERT_EQ(5u,
            pw_varint_CallZigZagAndVarintEncode64(
                static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                buffer,
                sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(5u,
            pw_varint_CallZigZagAndVarintEncode64(
                static_cast<int64_t>(std::numeric_limits<int32_t>::max()),
                buffer,
                sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\x0f", buffer, 5), 0);

  ASSERT_EQ(10u,
            pw_varint_CallZigZagAndVarintEncode64(
                std::numeric_limits<int64_t>::min(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer, 10),
            0);

  ASSERT_EQ(10u,
            pw_varint_CallZigZagAndVarintEncode64(
                std::numeric_limits<int64_t>::max(), buffer, sizeof(buffer)));
  EXPECT_EQ(std::memcmp("\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer, 10),
            0);
}

// How much to increment by for each iteration of the exhaustive encode/decode
// tests. Set the increment to 1 to test every number (this is slow).
constexpr int kIncrement = 100'000'009;

template <typename T, typename U = T>
void EncodeDecode(T value) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  size_t encoded = Encode(value, buffer);

  U result;
  size_t decoded = Decode(buffer, &result);

  EXPECT_EQ(encoded, decoded);
  ASSERT_EQ(value, result);
}

void EncodeDecodeSigned32(int32_t value) {
  EncodeDecode<int32_t, int64_t>(value);
}

void EncodeDecodeUnsigned32(uint32_t value) {
  EncodeDecode<uint32_t, uint64_t>(value);
}

TEST(Varint, EncodeDecodeSigned32Incremental) {
  int32_t i = std::numeric_limits<int32_t>::min();
  while (true) {
    EncodeDecodeSigned32(i);

    if (i > std::numeric_limits<int32_t>::max() - kIncrement) {
      break;
    }

    i += kIncrement;
  }
}

FUZZ_TEST(Varint, EncodeDecodeSigned32);

TEST(Varint, EncodeDecodeUnsigned32Incremental) {
  uint32_t i = 0;
  while (true) {
    EncodeDecodeUnsigned32(i);

    if (i > std::numeric_limits<uint32_t>::max() - kIncrement) {
      break;
    }

    i += kIncrement;
  }
}

FUZZ_TEST(Varint, EncodeDecodeUnsigned32);

template <typename T, typename U = T>
void EncodeDecode_C(T value) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  size_t encoded = 0;
  U result(0);
  size_t decoded = 0;
  if constexpr (sizeof(T) <= sizeof(uint32_t)) {
    encoded = pw_varint_CallEncode32(
        static_cast<uint32_t>(value), buffer, sizeof(buffer));
    decoded = pw_varint_CallDecode32(buffer, sizeof(buffer), &result);
  } else {
    encoded = pw_varint_CallEncode64(
        static_cast<uint64_t>(value), buffer, sizeof(buffer));
    decoded = pw_varint_CallDecode64(buffer, sizeof(buffer), &result);
  }
  EXPECT_EQ(encoded, decoded);
  ASSERT_EQ(value, static_cast<T>(result));
}

void EncodeDecodeSigned32_C(int32_t value) {
  EncodeDecode_C<int32_t, uint32_t>(value);
}

void EncodeDecodeUnsigned32_C(uint32_t value) {
  EncodeDecode_C<uint32_t, uint32_t>(value);
}

TEST(Varint, EncodeDecodeSigned32Incremental_C) {
  int32_t i = std::numeric_limits<int32_t>::min();
  while (true) {
    EncodeDecodeSigned32_C(i);

    if (i > std::numeric_limits<int32_t>::max() - kIncrement) {
      break;
    }

    i += kIncrement;
  }
}

FUZZ_TEST(Varint, EncodeDecodeSigned32_C);

TEST(Varint, EncodeDecodeUnsigned32Incremental_C) {
  uint32_t i = 0;
  while (true) {
    EncodeDecodeUnsigned32_C(i);

    if (i > std::numeric_limits<uint32_t>::max() - kIncrement) {
      break;
    }

    i += kIncrement;
  }
}

FUZZ_TEST(Varint, EncodeDecodeUnsigned32_C);

PW_CONSTEXPR_TEST(Varint, DecodeSigned64_SingleByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {};
  int64_t value = -1234;

  PW_TEST_EXPECT_EQ(Decode(Write("\x00", buffer), &value), 1u);
  PW_TEST_EXPECT_EQ(value, 0);

  PW_TEST_EXPECT_EQ(Decode(Write("\x01", buffer), &value), 1u);
  PW_TEST_EXPECT_EQ(value, -1);

  PW_TEST_EXPECT_EQ(Decode(Write("\x02", buffer), &value), 1u);
  PW_TEST_EXPECT_EQ(value, 1);

  PW_TEST_EXPECT_EQ(Decode(Write("\x03", buffer), &value), 1u);
  PW_TEST_EXPECT_EQ(value, -2);

  PW_TEST_EXPECT_EQ(Decode(Write("\x04", buffer), &value), 1u);
  PW_TEST_EXPECT_EQ(value, 2);

  PW_TEST_EXPECT_EQ(Decode(Write("\x04", buffer), &value), 1u);
  PW_TEST_EXPECT_EQ(value, 2);
});

TEST(Varint, DecodeSigned64_SingleByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {};
  int64_t value = -1234;

  auto bytes = Write("\x00", buffer);
  EXPECT_EQ(
      pw_varint_CallZigZagAndVarintDecode64(bytes.data(), bytes.size(), &value),
      1u);
  EXPECT_EQ(value, 0);

  bytes = Write("\x01", buffer);
  EXPECT_EQ(
      pw_varint_CallZigZagAndVarintDecode64(bytes.data(), bytes.size(), &value),
      1u);
  EXPECT_EQ(value, -1);

  bytes = Write("\x02", buffer);
  EXPECT_EQ(
      pw_varint_CallZigZagAndVarintDecode64(bytes.data(), bytes.size(), &value),
      1u);
  EXPECT_EQ(value, 1);

  bytes = Write("\x03", buffer);
  EXPECT_EQ(
      pw_varint_CallZigZagAndVarintDecode64(bytes.data(), bytes.size(), &value),
      1u);
  EXPECT_EQ(value, -2);

  bytes = Write("\x04", buffer);
  EXPECT_EQ(
      pw_varint_CallZigZagAndVarintDecode64(bytes.data(), bytes.size(), &value),
      1u);
  EXPECT_EQ(value, 2);

  bytes = Write("\x04", buffer);
  EXPECT_EQ(
      pw_varint_CallZigZagAndVarintDecode64(bytes.data(), bytes.size(), &value),
      1u);
  EXPECT_EQ(value, 2);
}

PW_CONSTEXPR_TEST(Varint, DecodeSigned64_MultiByte, {
  std::byte buffer[kMaxVarint64SizeBytes] = {};
  int64_t value = -1234;

  PW_TEST_EXPECT_EQ(Decode(Write("\x80\x01", buffer), &value), 2u);
  PW_TEST_EXPECT_EQ(value, 64);

  PW_TEST_EXPECT_EQ(Decode(Write("\x81\x01", buffer), &value), 2u);
  PW_TEST_EXPECT_EQ(value, -65);

  PW_TEST_EXPECT_EQ(Decode(Write("\x82\x01", buffer), &value), 2u);
  PW_TEST_EXPECT_EQ(value, 65);

  PW_TEST_EXPECT_EQ(Decode(Write("\xff\xff\xff\xff\x0f", buffer), &value), 5u);
  PW_TEST_EXPECT_EQ(value, std::numeric_limits<int32_t>::min());

  PW_TEST_EXPECT_EQ(Decode(Write("\xfe\xff\xff\xff\x0f", buffer), &value), 5u);
  PW_TEST_EXPECT_EQ(value, std::numeric_limits<int32_t>::max());

  PW_TEST_EXPECT_EQ(
      Decode(Write("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer), &value),
      10u);
  PW_TEST_EXPECT_EQ(value, std::numeric_limits<int64_t>::min());

  PW_TEST_EXPECT_EQ(
      Decode(Write("\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer), &value),
      10u);
  PW_TEST_EXPECT_EQ(value, std::numeric_limits<int64_t>::max());
});

TEST(Varint, DecodeSigned64_MultiByte_C) {
  std::byte buffer[kMaxVarint64SizeBytes] = {};
  int64_t value = -1234;

  auto bytes2 = Write("\x80\x01", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes2.data(), bytes2.size(), &value),
            2u);
  EXPECT_EQ(value, 64);

  bytes2 = Write("\x81\x01", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes2.data(), bytes2.size(), &value),
            2u);
  EXPECT_EQ(value, -65);

  bytes2 = Write("\x82\x01", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes2.data(), bytes2.size(), &value),
            2u);
  EXPECT_EQ(value, 65);

  auto bytes4 = Write("\xff\xff\xff\xff\x0f", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes4.data(), bytes4.size(), &value),
            5u);
  EXPECT_EQ(value, std::numeric_limits<int32_t>::min());

  bytes4 = Write("\xfe\xff\xff\xff\x0f", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes4.data(), bytes4.size(), &value),
            5u);
  EXPECT_EQ(value, std::numeric_limits<int32_t>::max());

  auto bytes8 = Write("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes8.data(), bytes8.size(), &value),
            10u);
  EXPECT_EQ(value, std::numeric_limits<int64_t>::min());

  bytes8 = Write("\xfe\xff\xff\xff\xff\xff\xff\xff\xff\x01", buffer);
  EXPECT_EQ(pw_varint_CallZigZagAndVarintDecode64(
                bytes8.data(), bytes8.size(), &value),
            10u);
  EXPECT_EQ(value, std::numeric_limits<int64_t>::max());
}

PW_CONSTEXPR_TEST(Varint, ZigZagEncode_Int8, {
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(0)), uint8_t(0));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(-1)), uint8_t(1));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(1)), uint8_t(2));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(-2)), uint8_t(3));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(2)), uint8_t(4));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(-33)), uint8_t(65));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int8_t(33)), uint8_t(66));
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int8_t>::min()),
                    std::numeric_limits<uint8_t>::max());
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int8_t>::max()),
                    std::numeric_limits<uint8_t>::max() - 1u);
});

PW_CONSTEXPR_TEST(Varint, ZigZagEncode_Int16, {
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(0)), uint16_t(0));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(-1)), uint16_t(1));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(1)), uint16_t(2));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(-2)), uint16_t(3));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(2)), uint16_t(4));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(-3333)), uint16_t(6665));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int16_t(3333)), uint16_t(6666));
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int16_t>::min()),
                    std::numeric_limits<uint16_t>::max());
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int16_t>::max()),
                    std::numeric_limits<uint16_t>::max() - 1u);
});

PW_CONSTEXPR_TEST(Varint, ZigZagEncode_Int32, {
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(0)), uint32_t(0));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(-1)), uint32_t(1));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(1)), uint32_t(2));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(-2)), uint32_t(3));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(2)), uint32_t(4));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(-128)), uint32_t(255));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(128)), uint32_t(256));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(-333333)), uint32_t(666665));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int32_t(333333)), uint32_t(666666));
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int32_t>::min()),
                    std::numeric_limits<uint32_t>::max());
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int32_t>::max()),
                    std::numeric_limits<uint32_t>::max() - 1u);
});

PW_CONSTEXPR_TEST(Varint, ZigZagEncode_Int64, {
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(0)), uint64_t(0));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(-1)), uint64_t(1));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(1)), uint64_t(2));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(-2)), uint64_t(3));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(2)), uint64_t(4));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(-3333333333)), uint64_t(6666666665));
  PW_TEST_EXPECT_EQ(ZigZagEncode(int64_t(3333333333)), uint64_t(6666666666));
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int64_t>::min()),
                    std::numeric_limits<uint64_t>::max());
  PW_TEST_EXPECT_EQ(ZigZagEncode(std::numeric_limits<int64_t>::max()),
                    std::numeric_limits<uint64_t>::max() - 1u);
});

PW_CONSTEXPR_TEST(Varint, ZigZagDecode_Int8, {
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(0)), int8_t(0));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(1)), int8_t(-1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(2)), int8_t(1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(3)), int8_t(-2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(4)), int8_t(2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(65)), int8_t(-33));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint8_t(66)), int8_t(33));
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint8_t>::max()),
                    std::numeric_limits<int8_t>::min());
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint8_t>::max() - 1u),
                    std::numeric_limits<int8_t>::max());
});

PW_CONSTEXPR_TEST(Varint, ZigZagDecode_Int16, {
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(0)), int16_t(0));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(1)), int16_t(-1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(2)), int16_t(1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(3)), int16_t(-2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(4)), int16_t(2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(6665)), int16_t(-3333));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint16_t(6666)), int16_t(3333));
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint16_t>::max()),
                    std::numeric_limits<int16_t>::min());
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint16_t>::max() - 1u),
                    std::numeric_limits<int16_t>::max());
});

PW_CONSTEXPR_TEST(Varint, ZigZagDecode_Int32, {
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(0)), int32_t(0));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(1)), int32_t(-1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(2)), int32_t(1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(3)), int32_t(-2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(4)), int32_t(2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(255)), int32_t(-128));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(256)), int32_t(128));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(666665)), int32_t(-333333));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint32_t(666666)), int32_t(333333));
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint32_t>::max()),
                    std::numeric_limits<int32_t>::min());
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint32_t>::max() - 1u),
                    std::numeric_limits<int32_t>::max());
});

PW_CONSTEXPR_TEST(Varint, ZigZagDecode_Int64, {
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(0)), int64_t(0));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(1)), int64_t(-1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(2)), int64_t(1));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(3)), int64_t(-2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(4)), int64_t(2));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(6666666665)), int64_t(-3333333333));
  PW_TEST_EXPECT_EQ(ZigZagDecode(uint64_t(6666666666)), int64_t(3333333333));
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint64_t>::max()),
                    std::numeric_limits<int64_t>::min());
  PW_TEST_EXPECT_EQ(ZigZagDecode(std::numeric_limits<uint64_t>::max() - 1llu),
                    std::numeric_limits<int64_t>::max());
});

PW_CONSTEXPR_TEST(Varint, ZigZagEncodeDecode, {
  PW_TEST_EXPECT_EQ(ZigZagDecode(ZigZagEncode(0)), 0);
  PW_TEST_EXPECT_EQ(ZigZagDecode(ZigZagEncode(1)), 1);
  PW_TEST_EXPECT_EQ(ZigZagDecode(ZigZagEncode(-1)), -1);
  PW_TEST_EXPECT_EQ(ZigZagDecode(ZigZagEncode(8675309)), 8675309);
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int8_t>::min())),
      std::numeric_limits<int8_t>::min());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int8_t>::max())),
      std::numeric_limits<int8_t>::max());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int16_t>::min())),
      std::numeric_limits<int16_t>::min());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int16_t>::max())),
      std::numeric_limits<int16_t>::max());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int32_t>::min())),
      std::numeric_limits<int32_t>::min());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int32_t>::max())),
      std::numeric_limits<int32_t>::max());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int64_t>::min())),
      std::numeric_limits<int64_t>::min());
  PW_TEST_EXPECT_EQ(
      ZigZagDecode(ZigZagEncode(std::numeric_limits<int64_t>::max())),
      std::numeric_limits<int64_t>::max());
});

TEST(Varint, EncodeWithOptions_SingleByte) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(Encode(0u, buffer, Format::kZeroTerminatedLeastSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x00});

  ASSERT_EQ(Encode(1u, buffer, Format::kZeroTerminatedLeastSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x02});

  ASSERT_EQ(Encode(0x7f, buffer, Format::kZeroTerminatedLeastSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0xfe});

  ASSERT_EQ(Encode(0u, buffer, Format::kOneTerminatedLeastSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x01});

  ASSERT_EQ(Encode(2u, buffer, Format::kOneTerminatedLeastSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x05});

  ASSERT_EQ(Encode(0x7f, buffer, Format::kOneTerminatedLeastSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0xff});

  ASSERT_EQ(Encode(0u, buffer, Format::kZeroTerminatedMostSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x00});

  ASSERT_EQ(Encode(7u, buffer, Format::kZeroTerminatedMostSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x07});

  ASSERT_EQ(Encode(0x7f, buffer, Format::kZeroTerminatedMostSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x7f});

  ASSERT_EQ(Encode(0u, buffer, Format::kOneTerminatedMostSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x80});

  ASSERT_EQ(Encode(15u, buffer, Format::kOneTerminatedMostSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0x8f});

  ASSERT_EQ(Encode(0x7f, buffer, Format::kOneTerminatedMostSignificant), 1u);
  EXPECT_EQ(buffer[0], std::byte{0xff});
}

TEST(Varint, EncodeWithOptions_MultiByte) {
  std::byte buffer[kMaxVarint64SizeBytes] = {std::byte('\xff')};
  ASSERT_EQ(Encode(128u, buffer, Format::kZeroTerminatedLeastSignificant), 2u);
  EXPECT_TRUE(Compare("\x01\x02", buffer));

  ASSERT_EQ(Encode(0xffffffff, buffer, Format::kZeroTerminatedLeastSignificant),
            5u);
  EXPECT_TRUE(Compare("\xff\xff\xff\xff\x1e", buffer));

  ASSERT_EQ(Encode(128u, buffer, Format::kOneTerminatedLeastSignificant), 2u);
  EXPECT_TRUE(Compare("\x00\x03", buffer));

  ASSERT_EQ(Encode(0xffffffff, buffer, Format::kOneTerminatedLeastSignificant),
            5u);
  EXPECT_TRUE(Compare("\xfe\xfe\xfe\xfe\x1f", buffer));

  ASSERT_EQ(Encode(128u, buffer, Format::kZeroTerminatedMostSignificant), 2u);
  EXPECT_TRUE(Compare("\x80\x01", buffer));

  ASSERT_EQ(Encode(0xffffffff, buffer, Format::kZeroTerminatedMostSignificant),
            5u);
  EXPECT_TRUE(Compare("\xff\xff\xff\xff\x0f", buffer));

  ASSERT_EQ(Encode(128u, buffer, Format::kOneTerminatedMostSignificant), 2u);
  EXPECT_TRUE(Compare("\x00\x81", buffer));

  ASSERT_EQ(Encode(0xffffffff, buffer, Format::kOneTerminatedMostSignificant),
            5u);
  EXPECT_TRUE(Compare("\x7f\x7f\x7f\x7f\x8f", buffer));
}

TEST(Varint, DecodeWithOptions_SingleByte) {
  std::byte buffer[kMaxVarint64SizeBytes] = {};
  uint64_t value;

  EXPECT_EQ(Decode(Write("\x00", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            1u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(Decode(Write("\x04", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            1u);
  EXPECT_EQ(value, 2u);

  EXPECT_EQ(Decode(Write("\xaa", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            1u);
  EXPECT_EQ(value, 85u);

  EXPECT_EQ(Decode(Write("\x01", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            0u);

  EXPECT_EQ(Decode(Write("\x01", buffer),
                   &value,
                   Format::kOneTerminatedLeastSignificant),
            1u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(Decode(Write("\x13", buffer),
                   &value,
                   Format::kOneTerminatedLeastSignificant),
            1u);
  EXPECT_EQ(value, 9u);

  EXPECT_EQ(Decode(Write("\x00", buffer),
                   &value,
                   Format::kOneTerminatedLeastSignificant),
            0u);

  EXPECT_EQ(Decode(Write("\x00", buffer),
                   &value,
                   Format::kZeroTerminatedMostSignificant),
            1u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(Decode(Write("\x04", buffer),
                   &value,
                   Format::kZeroTerminatedMostSignificant),
            1u);
  EXPECT_EQ(value, 4u);

  EXPECT_EQ(Decode(Write("\xff", buffer),
                   &value,
                   Format::kZeroTerminatedMostSignificant),
            0u);

  EXPECT_EQ(
      Decode(
          Write("\x80", buffer), &value, Format::kOneTerminatedMostSignificant),
      1u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(
      Decode(
          Write("\x83", buffer), &value, Format::kOneTerminatedMostSignificant),
      1u);
  EXPECT_EQ(value, 3u);

  EXPECT_EQ(
      Decode(
          Write("\xaa", buffer), &value, Format::kOneTerminatedMostSignificant),
      1u);
  EXPECT_EQ(value, 42u);

  EXPECT_EQ(
      Decode(
          Write("\xff", buffer), &value, Format::kOneTerminatedMostSignificant),
      1u);
  EXPECT_EQ(value, 127u);

  EXPECT_EQ(
      Decode(
          Write("\x00", buffer), &value, Format::kOneTerminatedMostSignificant),
      0u);
}

TEST(Varint, DecodeWithOptions_MultiByte) {
  std::byte buffer[kMaxVarint64SizeBytes] = {};
  uint64_t value;

  EXPECT_EQ(Decode(Write("\x01\x10", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            2u);
  EXPECT_EQ(value, 1024u);

  EXPECT_EQ(Decode(Write("\xff\xff\xff\xfe", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            4u);
  EXPECT_EQ(value, 0x0fffffffu);

  EXPECT_EQ(Decode(Write("\x01\x01\x01\x01\x00", buffer),
                   &value,
                   Format::kZeroTerminatedLeastSignificant),
            5u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(Decode(Write("\x82\x2d", buffer),
                   &value,
                   Format::kOneTerminatedLeastSignificant),
            2u);
  EXPECT_EQ(value, 2881u);

  EXPECT_EQ(Decode(Write("\xfe\xfe\xfe\xff", buffer),
                   &value,
                   Format::kOneTerminatedLeastSignificant),
            4u);
  EXPECT_EQ(value, 0x0fffffffu);

  EXPECT_EQ(Decode(Write("\x00\x00\x00\x00\x01", buffer),
                   &value,
                   Format::kOneTerminatedLeastSignificant),
            5u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(Decode(Write("\x83\x6a", buffer),
                   &value,
                   Format::kZeroTerminatedMostSignificant),
            2u);
  EXPECT_EQ(value, 0b1101010'0000011u);

  EXPECT_EQ(Decode(Write("\xff\xff\xff\x7f", buffer),
                   &value,
                   Format::kZeroTerminatedMostSignificant),
            4u);
  EXPECT_EQ(value, 0x0fffffffu);

  EXPECT_EQ(Decode(Write("\x80\x80\x80\x80\x00", buffer),
                   &value,
                   Format::kZeroTerminatedMostSignificant),
            5u);
  EXPECT_EQ(value, 0u);

  EXPECT_EQ(Decode(Write("\x6a\x83", buffer),
                   &value,
                   Format::kOneTerminatedMostSignificant),
            2u);
  EXPECT_EQ(value, 0b0000011'1101010u);

  EXPECT_EQ(Decode(Write("\x7f\x7f\x7f\xff", buffer),
                   &value,
                   Format::kOneTerminatedMostSignificant),
            4u);
  EXPECT_EQ(value, 0x0fffffffu);

  EXPECT_EQ(Decode(Write("\x00\x00\x00\x00\x80", buffer),
                   &value,
                   Format::kOneTerminatedMostSignificant),
            5u);
  EXPECT_EQ(value, 0u);
}

#define ENCODED_SIZE_TEST(function)                                           \
  TEST(Varint, function) {                                                    \
    EXPECT_EQ(function(uint64_t(0u)), 1u);                                    \
    EXPECT_EQ(function(uint64_t(1u)), 1u);                                    \
    EXPECT_EQ(function(uint64_t(127u)), 1u);                                  \
    EXPECT_EQ(function(uint64_t(128u)), 2u);                                  \
    EXPECT_EQ(function(uint64_t(16383u)), 2u);                                \
    EXPECT_EQ(function(uint64_t(16384u)), 3u);                                \
    EXPECT_EQ(function(uint64_t(2097151u)), 3u);                              \
    EXPECT_EQ(function(uint64_t(2097152u)), 4u);                              \
    EXPECT_EQ(function(uint64_t(268435455u)), 4u);                            \
    EXPECT_EQ(function(uint64_t(268435456u)), 5u);                            \
    EXPECT_EQ(function(uint64_t(34359738367u)), 5u);                          \
    EXPECT_EQ(function(uint64_t(34359738368u)), 6u);                          \
    EXPECT_EQ(function(uint64_t(4398046511103u)), 6u);                        \
    EXPECT_EQ(function(uint64_t(4398046511104u)), 7u);                        \
    EXPECT_EQ(function(uint64_t(562949953421311u)), 7u);                      \
    EXPECT_EQ(function(uint64_t(562949953421312u)), 8u);                      \
    EXPECT_EQ(function(uint64_t(72057594037927935u)), 8u);                    \
    EXPECT_EQ(function(uint64_t(72057594037927936u)), 9u);                    \
    EXPECT_EQ(function(uint64_t(9223372036854775807u)), 9u);                  \
    EXPECT_EQ(function(uint64_t(9223372036854775808u)), 10u);                 \
    EXPECT_EQ(function(std::numeric_limits<uint64_t>::max()), 10u);           \
    EXPECT_EQ(                                                                \
        static_cast<uint64_t>(function(std::numeric_limits<int64_t>::max())), \
        9u);                                                                  \
    EXPECT_EQ(function(uint64_t(-1)), 10u);                                   \
    EXPECT_EQ(                                                                \
        function(static_cast<uint64_t>(std::numeric_limits<int64_t>::min())), \
        10u);                                                                 \
  }                                                                           \
  static_assert(true)

ENCODED_SIZE_TEST(EncodedSize);
ENCODED_SIZE_TEST(pw_varint_EncodedSizeBytes);
ENCODED_SIZE_TEST(PW_VARINT_ENCODED_SIZE_BYTES);

constexpr uint64_t CalculateMaxValueInBytes(size_t bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes; ++i) {
    value |= uint64_t(0x7f) << (7 * i);
  }
  return value;
}

PW_CONSTEXPR_TEST(Varint, MaxValueInBytes, {
  static_assert(MaxValueInBytes(0) == 0);
  static_assert(MaxValueInBytes(1) == 0x7f);
  static_assert(MaxValueInBytes(2) == 0x3fff);
  static_assert(MaxValueInBytes(3) == 0x1fffff);
  static_assert(MaxValueInBytes(4) == 0x0fffffff);
  static_assert(MaxValueInBytes(5) == CalculateMaxValueInBytes(5));
  static_assert(MaxValueInBytes(6) == CalculateMaxValueInBytes(6));
  static_assert(MaxValueInBytes(7) == CalculateMaxValueInBytes(7));
  static_assert(MaxValueInBytes(8) == CalculateMaxValueInBytes(8));
  static_assert(MaxValueInBytes(9) == CalculateMaxValueInBytes(9));
  static_assert(MaxValueInBytes(10) == std::numeric_limits<uint64_t>::max());
  static_assert(MaxValueInBytes(11) == std::numeric_limits<uint64_t>::max());
  static_assert(MaxValueInBytes(100) == std::numeric_limits<uint64_t>::max());
});

}  // namespace
}  // namespace pw::varint
