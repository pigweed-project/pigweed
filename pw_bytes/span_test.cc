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

#include "pw_bytes/span.h"

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

// TODO: https://pwbug.dev/395945006 - DRY these macros with
// pw_numeric/checked_arithmetic_test.cc
// pw_span/cast_test.cc

// Emits a TEST() for a given templated function with the given type.
//
// suite - The name of the test suite (the first argument to TEST()).
// test_name - The full name of the test.
// func - The templated function called by the test.
// type - The type passed to the function template.
#define TEST_FUNC_FOR_TYPE(suite, test_name, func, type) \
  TEST(suite, test_name) { func<type>(); }

// Emits a TEST() for a templated function named by the concatention of the
// test suite and test name, invoked with the given type.
//
// suite - The name of the test suite (the first argument to TEST())
// name - The base name of the test.
//        The full name of the test includes _suffix.
//        The called function is the concatenation of suite and name.
// suffix - Appened to name (with an underscore) to generate the test name.
// type - The template type of the called function.
#define TEST_FOR_TYPE(suite, name, suffix, type) \
  TEST_FUNC_FOR_TYPE(suite, name##_##suffix, suite##name, type)

// Emits a TEST_FOR_TYPE() for all common <cstdint> types.
#define TEST_FOR_STDINT_TYPES(suite, name)  \
  TEST_FOR_TYPE(suite, name, u8, uint8_t)   \
  TEST_FOR_TYPE(suite, name, i8, int8_t)    \
  TEST_FOR_TYPE(suite, name, u16, uint16_t) \
  TEST_FOR_TYPE(suite, name, i16, int16_t)  \
  TEST_FOR_TYPE(suite, name, u32, uint32_t) \
  TEST_FOR_TYPE(suite, name, i32, int32_t)  \
  TEST_FOR_TYPE(suite, name, u64, uint64_t) \
  TEST_FOR_TYPE(suite, name, i64, int64_t)

#define TEST_FOR_ALL_INT_TYPES(suite, name)        \
  TEST_FOR_TYPE(suite, name, char, char)           \
  TEST_FOR_TYPE(suite, name, uchar, unsigned char) \
  TEST_FOR_STDINT_TYPES(suite, name)

#define EXPECT_SEQ_EQ(seq1, seq2) \
  EXPECT_TRUE(std::equal(seq1.begin(), seq1.end(), seq2.begin(), seq2.end()))

namespace {

constexpr auto kSomeValue = 0xDEADBEEF2B84F00D;

TEST(AsBytes, FromConstVoidPointer) {
  const uint32_t val = 0x12345678;
  const void* ptr = &val;

  pw::ConstByteSpan bytes = pw::AsBytes(ptr, sizeof(val));
  EXPECT_EQ(bytes.data(), reinterpret_cast<const std::byte*>(&val));
  EXPECT_EQ(bytes.size(), sizeof(val));

  const void* const const_ptr = &val;
  pw::ConstByteSpan const_ptr_bytes = pw::AsBytes(const_ptr, sizeof(val));
  EXPECT_EQ(const_ptr_bytes.data(), reinterpret_cast<const std::byte*>(&val));
  EXPECT_EQ(const_ptr_bytes.size(), sizeof(val));

  pw::ConstByteSpan empty = pw::AsBytes(static_cast<const void*>(nullptr), 0);
  EXPECT_EQ(empty.data(), nullptr);
  EXPECT_TRUE(empty.empty());
}

TEST(AsBytes, FromMutableVoidPointer) {
  uint32_t val = 0x12345678;
  void* ptr = &val;

  pw::ConstByteSpan bytes = pw::AsBytes(ptr, sizeof(val));
  static_assert(std::is_const_v<decltype(bytes)::element_type>);
  EXPECT_EQ(bytes.data(), reinterpret_cast<const std::byte*>(&val));
  EXPECT_EQ(bytes.size(), sizeof(val));

  pw::ConstByteSpan obj_bytes = pw::ObjectAsBytes(val);
  EXPECT_SEQ_EQ(bytes, obj_bytes);
}

TEST(AsWritableBytes, FromMutableVoidPointer) {
  uint32_t src = 0x12345678;
  uint32_t dst = 0;
  void* dst_ptr = &dst;

  pw::ByteSpan dst_bytes = pw::AsWritableBytes(dst_ptr, sizeof(dst));
  static_assert(!std::is_const_v<decltype(dst_bytes)::element_type>);
  EXPECT_EQ(dst_bytes.data(), reinterpret_cast<std::byte*>(&dst));
  EXPECT_EQ(dst_bytes.size(), sizeof(dst));

  pw::ConstByteSpan src_bytes =
      pw::AsBytes(static_cast<const void*>(&src), sizeof(src));
  std::copy(src_bytes.begin(), src_bytes.end(), dst_bytes.begin());
  EXPECT_EQ(src, dst);

  pw::ByteSpan obj_bytes = pw::ObjectAsWritableBytes(dst);
  EXPECT_SEQ_EQ(dst_bytes, obj_bytes);

  pw::ByteSpan empty = pw::AsWritableBytes(static_cast<void*>(nullptr), 0);
  EXPECT_EQ(empty.data(), nullptr);
  EXPECT_TRUE(empty.empty());
}

#if PW_NC_TEST(AsBytesFailsWithConstTypedPointer)
PW_NC_EXPECT("cannot pass typed pointer to 2-argument pw::AsBytes");
pw::ConstByteSpan FailsWithConstTypedPointer(const uint32_t* ptr) {
  return pw::AsBytes(ptr, 4);
}
#elif PW_NC_TEST(AsBytesFailsWithMutableTypedPointer)
PW_NC_EXPECT("cannot pass typed pointer to 2-argument pw::AsBytes");
pw::ConstByteSpan FailsWithMutableTypedPointer(uint32_t* ptr) {
  return pw::AsBytes(ptr, 4);
}
#elif PW_NC_TEST(AsWritableBytesFailsWithConstVoidPointer)
PW_NC_EXPECT(
    "cannot pass typed or const pointer to 2-argument pw::AsWritableBytes");
pw::ByteSpan FailsWithConstVoidPointer(const void* ptr) {
  return pw::AsWritableBytes(ptr, 4);
}
#elif PW_NC_TEST(AsWritableBytesFailsWithTypedPointer)
PW_NC_EXPECT(
    "cannot pass typed or const pointer to 2-argument pw::AsWritableBytes");
pw::ByteSpan FailsWithTypedPointer(uint32_t* ptr) {
  return pw::AsWritableBytes(ptr, 4);
}
#endif  // PW_NC_TEST

template <class T>
void ObjectAsBytesWorksInt() {
  const T val = static_cast<T>(kSomeValue);

  // N.B. We use an `auto` variable, rather than pw::ConstByteSpan, which
  // always has a dynamic extent.
  auto val_bytes = pw::ObjectAsBytes(val);
  static_assert(std::is_const_v<typename decltype(val_bytes)::element_type>);
  static_assert(val_bytes.extent == sizeof(T));  // Ensure static extent

  std::array<std::byte, sizeof(val)> buf;
  std::memcpy(&buf, &val, sizeof(val));

  EXPECT_SEQ_EQ(val_bytes, buf);
}

TEST_FOR_ALL_INT_TYPES(ObjectAsBytes, WorksInt)

template <class T>
void ObjectAsWritableBytesWorksInt() {
  const T src = static_cast<T>(kSomeValue);
  pw::ConstByteSpan src_bytes = pw::ObjectAsBytes(src);

  T dst{};

  // N.B. We use an `auto` variable, rather than pw::ByteSpan, which
  // always has a dynamic extent.
  auto dst_bytes = pw::ObjectAsWritableBytes(dst);
  static_assert(!std::is_const_v<typename decltype(dst_bytes)::element_type>);
  static_assert(dst_bytes.extent == sizeof(T));  // Ensure static extent

  std::copy(src_bytes.begin(), src_bytes.end(), dst_bytes.begin());

  EXPECT_EQ(src, dst);
}

TEST_FOR_ALL_INT_TYPES(ObjectAsWritableBytes, WorksInt)

struct Serializable {
  char c;
  unsigned char uc;
  uint8_t u8;
  int8_t i8;
  uint16_t u16;
  int16_t i16;
  uint32_t u32;
  int32_t i32;
  uint64_t u64;
  int64_t i64;

  // In C++20:
  // bool operator==(const Serializable& other) const = default;
};
static_assert(sizeof(Serializable) == 1 + 1 + 1 + 1 + 2 + 2 + 4 + 4 + 8 + 8);

constexpr bool operator==(const Serializable& lhs, const Serializable& rhs) {
  // clang-format off
  return (lhs.c == rhs.c)
      && (lhs.uc == rhs.uc)
      && (lhs.u8 == rhs.u8)
      && (lhs.i8 == rhs.i8)
      && (lhs.u16 == rhs.u16)
      && (lhs.i16 == rhs.i16)
      && (lhs.u32 == rhs.u32)
      && (lhs.i32 == rhs.i32)
      && (lhs.u64 == rhs.u64)
      && (lhs.i64 == rhs.i64);
  // clang-format on
}

constexpr Serializable kSerializableInit = {
    .c = 'A',
    .uc = 'Z',
    .u8 = 243,
    .i8 = -17,
    .u16 = 43512,
    .i16 = -31337,
    .u32 = 8675309,
    .i32 = -2870104,
    .u64 = 3141592653589793,
    .i64 = -2718281828459045,
};

TEST(ObjectAsBytes, WorksWithSerializable) {
  const Serializable val = kSerializableInit;

  std::array<std::byte, sizeof(val)> buf;
  std::memcpy(&buf, &val, sizeof(val));

  EXPECT_SEQ_EQ(pw::ObjectAsBytes(val), buf);
}

TEST(ObjectAsWritableBytes, WorksWithSerializable) {
  Serializable src = kSerializableInit;
  pw::ConstByteSpan src_bytes = pw::ObjectAsBytes(src);

  Serializable dst{};
  pw::ByteSpan dst_bytes = pw::ObjectAsWritableBytes(dst);

  std::copy(src_bytes.begin(), src_bytes.end(), dst_bytes.begin());

  EXPECT_EQ(src, dst);
}

struct NotTriviallyCopyable {
  uint64_t u64 = 0;

  NotTriviallyCopyable() = default;

  // The explicit copy-constructor and copy-assignment operators make this type
  // not trivially copyable.
  NotTriviallyCopyable(const NotTriviallyCopyable& other) { *this = other; }
  NotTriviallyCopyable& operator=(const NotTriviallyCopyable& other) {
    u64 = other.u64;
    return *this;
  }
};

static_assert(!std::is_trivially_copyable_v<NotTriviallyCopyable>);

#if PW_NC_TEST(ObjectAsBytesFailsWithNotTriviallyCopyable)
PW_NC_EXPECT(
    "cannot treat object as bytes: "
    "copying bytes may result in an invalid object");
pw::ConstByteSpan FailsWithNotTriviallyCopyable(
    const NotTriviallyCopyable& obj) {
  return pw::ObjectAsBytes(obj);
}
#elif PW_NC_TEST(ObjectAsWritableBytesFailsWithNotTriviallyCopyable)
PW_NC_EXPECT(
    "cannot treat object as bytes: "
    "copying bytes may result in an invalid object");
pw::ByteSpan FailsWithNotTriviallyCopyable(NotTriviallyCopyable& obj) {
  return pw::ObjectAsWritableBytes(obj);
}
#endif  // PW_NC_TEST

struct NonUniqueRepresentation {
  // For alignment, 7 padding bytes follow this field, making this type have
  // multiple possible represantations for the same value.
  uint8_t u8;

  uint64_t u64;
};

static_assert(
    !std::has_unique_object_representations_v<NonUniqueRepresentation>);

#if PW_NC_TEST(ObjectAsBytesFailsWithNonUniqueRepresentation)
PW_NC_EXPECT(
    "cannot treat object as bytes: "
    "type includes indeterminate bytes which may leak information "
    "or result in incorrect object hashing");
pw::ConstByteSpan FailsWithNonUniqueRepresentation(
    const NonUniqueRepresentation& obj) {
  return pw::ObjectAsBytes(obj);
}
#elif PW_NC_TEST(ObjectAsWritableBytesFailsWithNonUniqueRepresentation)
PW_NC_EXPECT(
    "cannot treat object as bytes: "
    "type includes indeterminate bytes which may leak information "
    "or result in incorrect object hashing");
pw::ByteSpan FailsWithNonUniqueRepresentation(NonUniqueRepresentation& obj) {
  return pw::ObjectAsWritableBytes(obj);
}
#endif  // PW_NC_TEST

}  // namespace
