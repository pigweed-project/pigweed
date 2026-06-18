// Copyright 2023 The Pigweed Authors
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

#include "pw_protobuf/find.h"

#include <string_view>

#include "pw_bytes/array.h"
#include "pw_status/status.h"
#include "pw_stream/memory_stream.h"
#include "pw_string/string.h"
#include "pw_unit_test/framework.h"

namespace pw::protobuf {
namespace {

constexpr auto kEncodedProto = bytes::Array<  // clang-format off
    // type=int32, k=1, v=42
    0x08, 0x2a,  // 0-1
    // type=sint32, k=2, v=-13
    0x10, 0x19,  // 2-3
    // type=bool, k=3, v=false
    0x18, 0x00,  // 4-5
    // type=double, k=4, v=3.14159
    0x21, 0x6e, 0x86, 0x1b, 0xf0, 0xf9, 0x21, 0x09, 0x40, // 6-14
    // type=fixed32, k=5, v=0xdeadbeef
    0x2d, 0xef, 0xbe, 0xad, 0xde,  // 15-19
    // type=string, k=6, v="Hello world"
    0x32, 0x0b, 'H', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd',  // 20-32

    // type=message, k=7, len=2
    0x3a, 0x02,  // 33-34
    // (nested) type=uint32, k=1, v=3
    0x08, 0x03   // 35-36
>();  // clang-format on

static_assert(kEncodedProto.size() == 37);

TEST(Find, PresentField) {
  EXPECT_EQ(FindInt32(kEncodedProto, 1, Occurrence::kFirst).value(), 42);
  EXPECT_EQ(FindSint32(kEncodedProto, 2, Occurrence::kFirst).value(), -13);
  EXPECT_EQ(FindBool(kEncodedProto, 3, Occurrence::kFirst).value(), false);
  EXPECT_EQ(FindDouble(kEncodedProto, 4, Occurrence::kFirst).value(), 3.14159);
  EXPECT_EQ(FindFixed32(kEncodedProto, 5, Occurrence::kFirst).value(),
            0xdeadbeef);

  Result<std::string_view> result =
      FindString(kEncodedProto, 6, Occurrence::kFirst);
  ASSERT_EQ(result.status(), OkStatus());
  InlineString<32> str(*result);
  EXPECT_STREQ(str.c_str(), "Hello world");
}

TEST(Find, MissingField) {
  EXPECT_EQ(FindUint32(kEncodedProto, 8, Occurrence::kFirst).status(),
            Status::NotFound());
  EXPECT_EQ(FindUint32(kEncodedProto, 8, Occurrence::kLast).status(),
            Status::NotFound());
  EXPECT_EQ(FindUint32(kEncodedProto, 66, Occurrence::kFirst).status(),
            Status::NotFound());
  EXPECT_EQ(FindUint32(kEncodedProto, 123456789, Occurrence::kFirst).status(),
            Status::NotFound());
  EXPECT_EQ(FindRaw(kEncodedProto, 123456789, Occurrence::kFirst).status(),
            Status::NotFound());
  EXPECT_EQ(FindRaw(kEncodedProto, 123456789, Occurrence::kLast).status(),
            Status::NotFound());
}

TEST(Find, InvalidFieldNumber) {
  EXPECT_EQ(FindUint32(kEncodedProto, 0, Occurrence::kFirst).status(),
            Status::InvalidArgument());
  EXPECT_EQ(
      FindUint32(kEncodedProto, uint32_t(-1), Occurrence::kFirst).status(),
      Status::InvalidArgument());
}

TEST(Find, WrongWireType) {
  // Field 5 is a fixed32, but we request a uint32 (varint).
  EXPECT_EQ(FindUint32(kEncodedProto, 5, Occurrence::kFirst).status(),
            Status::FailedPrecondition());
}

TEST(Find, MultiLevel) {
  Result<ConstByteSpan> submessage =
      FindSubmessage(kEncodedProto, 7, Occurrence::kFirst);
  ASSERT_EQ(submessage.status(), OkStatus());
  EXPECT_EQ(submessage->size(), 2u);

  // Read a field from the submessage.
  EXPECT_EQ(FindUint32(*submessage, 1, Occurrence::kFirst).value(), 3u);
}

TEST(FindStream, PresentField) {
  stream::MemoryReader reader(kEncodedProto);

  EXPECT_EQ(FindInt32(reader, 1, Occurrence::kFirst).value(), 42);
  EXPECT_EQ(FindSint32(reader, 2, Occurrence::kFirst).value(), -13);
  EXPECT_EQ(FindBool(reader, 3, Occurrence::kFirst).value(), false);
  EXPECT_EQ(FindDouble(kEncodedProto, 4, Occurrence::kFirst).value(), 3.14159);

  EXPECT_EQ(FindFixed32(reader, 5, Occurrence::kFirst).value(), 0xdeadbeef);

  char str[32];
  StatusWithSize sws = FindString(reader, 6, str, Occurrence::kFirst);
  ASSERT_EQ(sws.status(), OkStatus());
  ASSERT_EQ(sws.size(), 11u);
  str[sws.size()] = '\0';
  EXPECT_STREQ(str, "Hello world");
}

TEST(FindStream, MissingField) {
  stream::MemoryReader reader(kEncodedProto);
  EXPECT_EQ(FindUint32(reader, 8, Occurrence::kFirst).status(),
            Status::NotFound());

  reader = stream::MemoryReader(kEncodedProto);
  EXPECT_EQ(FindUint32(reader, 8, Occurrence::kLast).status(),
            Status::NotFound());

  reader = stream::MemoryReader(kEncodedProto);
  EXPECT_EQ(FindUint32(reader, 66, Occurrence::kFirst).status(),
            Status::NotFound());

  reader = stream::MemoryReader(kEncodedProto);
  EXPECT_EQ(FindUint32(reader, 123456789, Occurrence::kFirst).status(),
            Status::NotFound());
}

TEST(FindStream, InvalidFieldNumber) {
  stream::MemoryReader reader(kEncodedProto);
  EXPECT_EQ(FindUint32(reader, 0, Occurrence::kFirst).status(),
            Status::InvalidArgument());

  reader = stream::MemoryReader(kEncodedProto);
  EXPECT_EQ(FindUint32(reader, uint32_t(-1), Occurrence::kFirst).status(),
            Status::InvalidArgument());
}

TEST(FindStream, WrongWireType) {
  stream::MemoryReader reader(kEncodedProto);

  // Field 5 is a fixed32, but we request a uint32 (varint).
  EXPECT_EQ(FindUint32(reader, 5, Occurrence::kFirst).status(),
            Status::FailedPrecondition());
}

enum class Fields : uint32_t {
  kField1 = 1,
  kField2 = 2,
  kField3 = 3,
  kField4 = 4,
  kField5 = 5,
  kField6 = 6,
  kField7 = 7,
};

TEST(FindEnum, PresentField) {
  EXPECT_EQ(
      FindInt32(kEncodedProto, Fields::kField1, Occurrence::kFirst).value(),
      42);
  EXPECT_EQ(
      FindSint32(kEncodedProto, Fields::kField2, Occurrence::kFirst).value(),
      -13);
  EXPECT_EQ(
      FindBool(kEncodedProto, Fields::kField3, Occurrence::kFirst).value(),
      false);
  EXPECT_EQ(
      FindDouble(kEncodedProto, Fields::kField4, Occurrence::kFirst).value(),
      3.14159);
  EXPECT_EQ(
      FindFixed32(kEncodedProto, Fields::kField5, Occurrence::kFirst).value(),
      0xdeadbeef);

  stream::MemoryReader reader(kEncodedProto);
  InlineString<32> str;
  StatusWithSize result =
      FindString(reader, Fields::kField6, str, Occurrence::kFirst);
  ASSERT_EQ(result.status(), OkStatus());
  EXPECT_STREQ(str.c_str(), "Hello world");
}

TEST(FindRaw, PresentField) {
  ConstByteSpan field1 =
      FindRaw(kEncodedProto, Fields::kField1, Occurrence::kFirst).value();
  EXPECT_EQ(field1.data(), kEncodedProto.data() + 1);
  EXPECT_EQ(field1.size(), 1u);

  ConstByteSpan field2 =
      FindRaw(kEncodedProto, Fields::kField2, Occurrence::kFirst).value();
  EXPECT_EQ(field2.data(), kEncodedProto.data() + 3);
  EXPECT_EQ(field2.size(), 1u);

  ConstByteSpan field3 =
      FindRaw(kEncodedProto, Fields::kField3, Occurrence::kFirst).value();
  EXPECT_EQ(field3.data(), kEncodedProto.data() + 5);
  EXPECT_EQ(field3.size(), 1u);

  ConstByteSpan field4 =
      FindRaw(kEncodedProto, Fields::kField4, Occurrence::kFirst).value();
  EXPECT_EQ(field4.data(), kEncodedProto.data() + 7);
  EXPECT_EQ(field4.size(), sizeof(double));

  ConstByteSpan field5 =
      FindRaw(kEncodedProto, Fields::kField5, Occurrence::kFirst).value();
  EXPECT_EQ(field5.data(), kEncodedProto.data() + 16);
  EXPECT_EQ(field5.size(), sizeof(uint32_t));

  ConstByteSpan field6 =
      FindRaw(kEncodedProto, Fields::kField6, Occurrence::kFirst).value();
  EXPECT_EQ(field6.data(), kEncodedProto.data() + 22);
  EXPECT_EQ(field6.size(), sizeof("Hello world") - 1 /* null */);

  ConstByteSpan field7 =
      FindRaw(kEncodedProto, Fields::kField7, Occurrence::kFirst).value();
  EXPECT_EQ(field7.data(), kEncodedProto.data() + 35);
  EXPECT_EQ(field7.size(), 2u);
}

enum class Boolean {
  kTrue = 0,
  kFalse = 1,
  kFileNotFound = 2,
};

constexpr auto kEncodedRepeatedProto = bytes::Array<  // clang-format off
    // type=int32, k=1, v=42
    0x08, 0x2a,
    // type=int32, k=1, v=32
    0x08, 0x20,
    // type=int32, k=1, v=16
    0x08, 0x10,
    // type=int32, k=1, v=0
    0x08, 0x00,
    // type=uint32, k=2, v=1
    0x10, 0x1,
    // type=uint32, k=2, v=2
    0x10, 0x2,
    // type=string, k=6, v="Hello, "
    0x32, 0x07, 'H', 'e', 'l', 'l', 'o', ',', ' ',
    // type=string, k=6, v="world"
    0x32, 0x05, 'w', 'o', 'r', 'l', 'd',
    // type=string, k=6, v="!"
    0x32, 0x01, '!'
>();  // clang-format on

TEST(Find, NonRepeatedShadowedField) {
  // Field 1 (int32) appears multiple times with values 42, 32, 16, 0.
  EXPECT_EQ(FindInt32(kEncodedRepeatedProto, 1, Occurrence::kFirst).value(),
            42);
  EXPECT_EQ(FindInt32(kEncodedRepeatedProto, 1, Occurrence::kLast).value(), 0);

  // Field 2 (uint32) appears multiple times with values 1, 2.
  EXPECT_EQ(FindUint32(kEncodedRepeatedProto, 2, Occurrence::kFirst).value(),
            1u);
  EXPECT_EQ(FindUint32(kEncodedRepeatedProto, 2, Occurrence::kLast).value(),
            2u);

  // Field 6 (string) appears multiple times with values "Hello, ", "world",
  // "!".
  Result<std::string_view> string_first =
      FindString(kEncodedRepeatedProto, 6, Occurrence::kFirst);
  ASSERT_EQ(string_first.status(), OkStatus());
  EXPECT_EQ(*string_first, "Hello, ");

  Result<std::string_view> string_last =
      FindString(kEncodedRepeatedProto, 6, Occurrence::kLast);
  ASSERT_EQ(string_last.status(), OkStatus());
  EXPECT_EQ(*string_last, "!");

  // Field 6 (string/bytes) using FindRaw.
  Result<ConstByteSpan> raw_first =
      FindRaw(kEncodedRepeatedProto, 6, Occurrence::kFirst);
  ASSERT_EQ(raw_first.status(), OkStatus());
  EXPECT_EQ(raw_first->size(), 7u);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(raw_first->data()),
                             raw_first->size()),
            "Hello, ");

  Result<ConstByteSpan> raw_last =
      FindRaw(kEncodedRepeatedProto, 6, Occurrence::kLast);
  ASSERT_EQ(raw_last.status(), OkStatus());
  EXPECT_EQ(raw_last->size(), 1u);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(raw_last->data()),
                             raw_last->size()),
            "!");
}

TEST(FindStream, ShadowedField) {
  {
    stream::MemoryReader reader(kEncodedRepeatedProto);
    EXPECT_EQ(FindInt32(reader, 1, Occurrence::kFirst).value(), 42);
  }
  {
    stream::MemoryReader reader(kEncodedRepeatedProto);
    EXPECT_EQ(FindInt32(reader, 1, Occurrence::kLast).value(), 0);
  }
  {
    stream::MemoryReader reader(kEncodedRepeatedProto);
    char str[32];
    StatusWithSize sws = FindString(reader, 6, span(str), Occurrence::kFirst);
    ASSERT_EQ(sws.status(), OkStatus());
    EXPECT_EQ(std::string_view(str, sws.size()), "Hello, ");
  }
  {
    stream::MemoryReader reader(kEncodedRepeatedProto);
    char str[32];
    StatusWithSize sws = FindString(reader, 6, span(str), Occurrence::kLast);
    ASSERT_EQ(sws.status(), OkStatus());
    EXPECT_EQ(std::string_view(str, sws.size()), "!");
  }
}

TEST(Finder, RepeatedField) {
  StringFinder finder(kEncodedRepeatedProto, 6);
  Result<std::string_view> result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), std::string_view("Hello, "));
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), std::string_view("world"));
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), std::string_view("!"));
  result = finder.Next();
  EXPECT_EQ(result.status(), Status::NotFound());
}

TEST(StreamFinder, RepeatedField) {
  stream::MemoryReader reader(kEncodedRepeatedProto);
  Int32StreamFinder finder(reader, 1);
  Result<int32_t> result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), 42);
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), 32);
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), 16);
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), 0);
  result = finder.Next();
  EXPECT_EQ(result.status(), Status::NotFound());
}

TEST(EnumFinder, RepeatedField) {
  EnumFinder<Boolean> finder(kEncodedRepeatedProto, 2);
  Result<Boolean> result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), Boolean::kFalse);
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), Boolean::kFileNotFound);
  result = finder.Next();
  EXPECT_EQ(result.status(), Status::NotFound());
}

TEST(EnumStreamFinder, RepeatedField) {
  stream::MemoryReader reader(kEncodedRepeatedProto);
  EnumStreamFinder<Boolean> finder(reader, 2);
  Result<Boolean> result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), Boolean::kFalse);
  result = finder.Next();
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result.value(), Boolean::kFileNotFound);
  result = finder.Next();
  EXPECT_EQ(result.status(), Status::NotFound());
}

}  // namespace
}  // namespace pw::protobuf
