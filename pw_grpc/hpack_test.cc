// Copyright 2024 The Pigweed Authors
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

#include "pw_grpc_private/hpack.h"

#include "pw_bytes/array.h"
#include "pw_unit_test/framework.h"

namespace pw::grpc {
namespace {

void TestIntegerDecode(ConstByteSpan input, uint8_t bits, uint32_t expected) {
  auto result = HpackIntegerDecode(input, bits);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, expected);
  EXPECT_TRUE(input.empty());  // input has advanced past the integer
}

void TestHuffmanDecode(ConstByteSpan input, std::string_view expected) {
  auto result = HpackHuffmanDecode(input);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, expected);
}

// Integer test cases from RFC 7541 Appendix C.1.
TEST(HpackTest, HpackIntegerDecodeC11) {
  const auto kInput = bytes::Array<0b11101010>();
  TestIntegerDecode(kInput, /*bits_in_first_byte=*/5, /*expected=*/10U);
}
TEST(HpackTest, HpackIntegerDecodeC12) {
  const auto kInput = bytes::Array<0b11111111, 0b10011010, 0b00001010>();
  TestIntegerDecode(kInput, /*bits_in_first_byte=*/5, /*expected=*/1337U);
}
TEST(HpackTest, HpackIntegerDecodeC13) {
  const auto kInput = bytes::Array<0b00101010>();
  TestIntegerDecode(kInput, /*bits_in_first_byte=*/8, /*expected=*/42U);
}

// Huffman test cases from RFC 7541 Appendix C.4.
// clang-format off
const auto kHuffmanC41 = bytes::Array<0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff>();
const auto kHuffmanC42 = bytes::Array<0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf>();
const auto kHuffmanC43a = bytes::Array<0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xa9, 0x7d, 0x7f>();
const auto kHuffmanC43b = bytes::Array<0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xb8, 0xe8, 0xb4, 0xbf>();
// clang-format on

TEST(HpackTest, HpackHuffmanDecodeC41) {
  TestHuffmanDecode(kHuffmanC41, "www.example.com");
}
TEST(HpackTest, HpackHuffmanDecodeC42) {
  TestHuffmanDecode(kHuffmanC42, "no-cache");
}
TEST(HpackTest, HpackHuffmanDecodeC43a) {
  TestHuffmanDecode(kHuffmanC43a, "custom-key");
}
TEST(HpackTest, HpackHuffmanDecodeC43b) {
  TestHuffmanDecode(kHuffmanC43b, "custom-value");
}

// Header field test cases from RFC 7541 Appendix C.
TEST(HpackTest, HpackParseRequestHeadersFoundIndexedSlash) {
  // Appendix C.3.1.
  const auto kInput = bytes::Array<0x84>();
  auto result = HpackParseRequestHeaders(kInput);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, "/");
}
TEST(HpackTest, HpackParseRequestHeadersFoundIndexedHtml) {
  // Appendix C.3.3.
  const auto kInput = bytes::Array<0x85>();
  auto result = HpackParseRequestHeaders(kInput);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, "/index.html");
}
TEST(HpackTest, HpackParseRequestHeadersFoundNotIndexed) {
  // clang-format off
  const auto kInput = bytes::Array<
      // Appendix C.2.1.
      0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x6b, 0x65, 0x79, 0x0d, 0x63,
      0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x68, 0x65, 0x61, 0x64, 0x65, 0x72,
      // Appendix C.2.3.
      0x10, 0x08, 0x70, 0x61, 0x73, 0x73, 0x77, 0x6f, 0x72, 0x64, 0x06, 0x73, 0x65, 0x63,
      0x72, 0x65, 0x74,
      // Appendix C.2.2.
      0x04, 0x0c, 0x2f, 0x73, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2f, 0x70, 0x61, 0x74, 0x68
  >();
  // clang-format on
  auto result = HpackParseRequestHeaders(kInput);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, "/sample/path");
}
TEST(HpackTest, HpackParseRequestHeadersNotFound) {
  // clang-format off
  const auto kInput = bytes::Array<
      // Appendix C.2.1.
      0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x6b, 0x65, 0x79, 0x0d, 0x63,
      0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x68, 0x65, 0x61, 0x64, 0x65, 0x72,
      // Appendix C.2.3.
      0x10, 0x08, 0x70, 0x61, 0x73, 0x73, 0x77, 0x6f, 0x72, 0x64, 0x06, 0x73, 0x65, 0x63,
      0x72, 0x65, 0x74
  >();
  // clang-format on
  auto result = HpackParseRequestHeaders(kInput);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), PW_STATUS_NOT_FOUND);
}

}  // namespace
}  // namespace pw::grpc
