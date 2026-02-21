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

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"

#include <cstddef>
#include <type_traits>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt {
namespace {

TEST(ByteBufferTest, StaticByteBuffer) {
  constexpr size_t kBufferSize = 5;
  StaticByteBuffer<kBufferSize> buffer;

  // The buffer is initialized to 0.
  constexpr std::array<uint8_t, kBufferSize> kExpectedDefault{
      {0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(ContainersEqual(kExpectedDefault, buffer));

  EXPECT_EQ(kBufferSize, buffer.size());
  buffer.SetToZeros();
  buffer[3] = 3;

  constexpr std::array<uint8_t, kBufferSize> kExpected{
      {0x00, 0x00, 0x00, 0x03, 0x00}};
  EXPECT_TRUE(ContainersEqual(kExpected, buffer));

  // Moving will result in a copy.
  StaticByteBuffer<kBufferSize> buffer_copy =
      std::move(buffer);                  // NOLINT(performance-move-const-arg)
  EXPECT_EQ(kBufferSize, buffer.size());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(kBufferSize, buffer_copy.size());
  EXPECT_TRUE(
      ContainersEqual(kExpected, buffer));  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(ContainersEqual(kExpected, buffer_copy));

  // Copying should also copy.
  StaticByteBuffer<kBufferSize> buffer_copy1 = buffer;
  EXPECT_EQ(kBufferSize, buffer.size());
  EXPECT_EQ(kBufferSize, buffer_copy1.size());
  EXPECT_TRUE(ContainersEqual(kExpected, buffer));
  EXPECT_TRUE(ContainersEqual(kExpected, buffer_copy1));

  // Const ByteBuffer should still permit operator[] access.
  const StaticByteBuffer const_buff(0x10);
  EXPECT_EQ(0x10, const_buff[0]);
}

TEST(ByteBufferTest, StaticByteBufferPackConstructor) {
  constexpr size_t kBufferSize = 3;
  StaticByteBuffer<kBufferSize> buffer0;
  buffer0[0] = 0x01;
  buffer0[1] = 0x02;
  buffer0[2] = 0x03;

  StaticByteBuffer<kBufferSize> buffer1{0x01, 0x02, 0x03};
  StaticByteBuffer buffer2(0x01, 0x02, 0x03);
  StaticByteBuffer buffer3{0x01, 0x02, 0x03};

  EXPECT_TRUE(ContainersEqual(buffer0, buffer1));
  EXPECT_TRUE(ContainersEqual(buffer0, buffer2));
  EXPECT_TRUE(ContainersEqual(buffer1, buffer2));
  EXPECT_TRUE(ContainersEqual(buffer1, buffer3));

  // The corresponding check is a DEBUG-only runtime check
  EXPECT_DEBUG_DEATH(StaticByteBuffer(-257), "ASSERT");
}

TEST(ByteBufferTest, DynamicByteBuffer) {
  constexpr size_t kBufferSize = 5;
  DynamicByteBuffer buffer(kBufferSize);
  EXPECT_EQ(kBufferSize, buffer.size());

  // The buffer is initialized to 0.
  constexpr std::array<uint8_t, kBufferSize> kExpectedDefault{
      {0x00, 0x00, 0x00, 0x00, 0x00}};
  EXPECT_TRUE(ContainersEqual(kExpectedDefault, buffer));

  buffer[3] = 3;
  constexpr std::array<uint8_t, kBufferSize> kExpected{
      {0x00, 0x00, 0x00, 0x03, 0x00}};
  EXPECT_TRUE(ContainersEqual(kExpected, buffer));

  // Moving will invalidate the source buffer.
  DynamicByteBuffer buffer_moved = std::move(buffer);
  EXPECT_EQ(0u, buffer.size());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(kBufferSize, buffer_moved.size());
  EXPECT_EQ(nullptr, buffer.data());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(ContainersEqual(kExpected, buffer_moved));

  // Const ByteBuffer copied from buffer_moved should permit operator[] access.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const DynamicByteBuffer const_buff(buffer_moved);
  EXPECT_EQ(0x03, const_buff[3]);
}

TEST(ByteBufferTest, DynamicByteBufferZeroSize) {
  DynamicByteBuffer buffer;

  EXPECT_EQ(0u, buffer.size());
  EXPECT_EQ(nullptr, buffer.data());

  DynamicByteBuffer zerosize(0);

  EXPECT_EQ(0u, zerosize.size());
  EXPECT_EQ(nullptr, buffer.data());
}

TEST(ByteBufferTest, DynamicByteBufferConstructFromBuffer) {
  constexpr size_t kBufferSize = 3;
  StaticByteBuffer<kBufferSize> buffer(1, 2, 3);

  DynamicByteBuffer dyn_buffer(buffer);
  EXPECT_EQ(kBufferSize, dyn_buffer.size());
  EXPECT_TRUE(ContainersEqual(buffer, dyn_buffer));
}

TEST(ByteBufferTest, DynamicByteBufferExplicitCopy) {
  DynamicByteBuffer src(1);
  src[0] = 'a';

  DynamicByteBuffer dst(src);
  EXPECT_EQ(1u, dst.size());
  EXPECT_TRUE(ContainersEqual(src, dst));
}

TEST(ByteBufferTest, DynamicByteBufferConstructFromBytes) {
  constexpr size_t kBufferSize = 3;
  std::array<uint8_t, kBufferSize> kExpected{{0, 1, 2}};

  pw::UniquePtr<std::byte[]> bytes =
      pw::allocator::GetLibCAllocator().MakeUnique<std::byte[]>(kBufferSize);
  std::memcpy(bytes.get(), kExpected.data(), kBufferSize);

  DynamicByteBuffer buffer(kBufferSize, std::move(bytes));
  EXPECT_EQ(nullptr, bytes.get());
  EXPECT_TRUE(ContainersEqual(kExpected, buffer));
}

TEST(ByteBufferTest, DynamicByteBufferConstructFromString) {
  constexpr size_t kBufferSize = 3;
  std::array<uint8_t, kBufferSize> kExpected{{0x41, 0x42, 0x43}};

  auto str = std::string("ABC");
  DynamicByteBuffer buffer(str);

  EXPECT_EQ(std::string("ABC"), str);
  EXPECT_TRUE(ContainersEqual(kExpected, buffer));
}

TEST(ByteBufferTest, DynamicByteBufferShrink) {
  std::string data = "abcdef";
  DynamicByteBuffer buffer(data);
  ASSERT_EQ(buffer.size(), data.size());

  std::string new_data = "abcde";
  ASSERT_FALSE(buffer.expand(new_data.size()));
  EXPECT_EQ(buffer.size(), data.size());
  EXPECT_EQ(buffer.ToString(), data);
}

TEST(ByteBufferTest, DynamicByteBufferNewSizeEqual) {
  std::string data = "abcdef";
  DynamicByteBuffer buffer(data);
  ASSERT_EQ(buffer.size(), data.size());
  ASSERT_FALSE(buffer.expand(data.size()));
  ASSERT_EQ(buffer.size(), data.size());
  EXPECT_EQ(buffer.ToString(), data);
}

TEST(ByteBufferTest, DynamicByteBufferExpand) {
  std::string data = "abcdef";
  DynamicByteBuffer buffer(data);
  ASSERT_EQ(buffer.size(), data.size());

  ASSERT_TRUE(buffer.expand(data.size() * 2));
  ASSERT_EQ(buffer.size(), data.size() * 2);
  EXPECT_EQ(buffer.view(0, data.size()).ToString(), data);

  // Verify the newly expanded portion is filled with zeros.
  for (size_t i = data.size(); i < buffer.size(); ++i) {
    EXPECT_EQ(0x00, buffer[i]) << " at index " << i;
  }
}

TEST(ByteBufferTest, DynamicByteBufferExpandUninitialized) {
  DynamicByteBuffer buffer;
  ASSERT_EQ(0u, buffer.size());
  ASSERT_EQ(nullptr, buffer.data());

  size_t new_size = 8;
  ASSERT_TRUE(buffer.expand(new_size));
  ASSERT_EQ(new_size, buffer.size());
  ASSERT_NE(nullptr, buffer.data());

  // Verify the newly expanded portion is filled with zeros.
  for (size_t i = 0; i < buffer.size(); ++i) {
    EXPECT_EQ(0x00, buffer[i]) << " at index " << i;
  }
}

TEST(ByteBufferTest, ByteBufferToStringAsHex) {
  StaticByteBuffer bytes(0x01, 0x02, 0x03);
  std::string bytesStringAsHex = bytes.ToString(/*as_hex*/ true);
  ASSERT_EQ(bytesStringAsHex, "01 02 03");
}

TEST(ByteBufferTest, BufferViewTest) {
  constexpr size_t kBufferSize = 5;
  DynamicByteBuffer buffer(kBufferSize);

  EXPECT_EQ(kBufferSize, buffer.size());
  buffer.SetToZeros();

  BufferView view(buffer);
  EXPECT_EQ(0x00, buffer[0]);
  EXPECT_EQ(0x00, view[0]);
  EXPECT_EQ(kBufferSize, buffer.size());
  EXPECT_EQ(kBufferSize, view.size());
}

TEST(ByteBufferTest, BufferViewFromVector) {
  const std::vector<uint8_t> kData{{1, 2, 3}};
  BufferView view(kData);
  EXPECT_TRUE(ContainersEqual(kData, view));
}

TEST(ByteBufferTest, MutableBufferViewTest) {
  constexpr size_t kBufferSize = 5;
  constexpr size_t kViewSize = 3;
  DynamicByteBuffer buffer(kBufferSize);

  EXPECT_EQ(kBufferSize, buffer.size());
  buffer.SetToZeros();

  MutableBufferView view(buffer.mutable_data(), kViewSize);
  view[0] = 0x01;
  EXPECT_EQ(0x01, buffer[0]);
  EXPECT_EQ(0x01, view[0]);
  EXPECT_EQ(kBufferSize, buffer.size());
  EXPECT_EQ(kViewSize, view.size());

  MutableBufferView view2(view);
  view2[0] = 0x00;
  EXPECT_EQ(0x00, buffer[0]);
  EXPECT_EQ(0x00, view[0]);
  EXPECT_EQ(0x00, view2[0]);
  EXPECT_EQ(kBufferSize, buffer.size());
  EXPECT_EQ(kViewSize, view.size());
}

TEST(ByteBufferDeathTest, Copy) {
  StaticByteBuffer buffer('T', 'e', 's', 't');
  BufferView empty_buffer;

  // Create a large enough buffer.
  StaticByteBuffer<10> target_buffer;

  // Copying an empty buffer should copy 0 bytes.
  empty_buffer.Copy(&target_buffer);

  // Copy all of |buffer|. The first |buffer.size()| octets of |target_buffer|
  // should match the contents of |buffer|.
  buffer.Copy(&target_buffer);
  EXPECT_TRUE(
      ContainersEqual(buffer, BufferView(target_buffer, buffer.size())));

  // Copy one octet of |buffer| starting at index 2
  target_buffer.SetToZeros();
  buffer.Copy(&target_buffer, 1, 1);
  EXPECT_EQ(buffer[1], target_buffer[0]);

  // Zero the buffer and copy its contents for later comparison.
  target_buffer.SetToZeros();
  auto target_buffer_copy = target_buffer;

  // Copy all remaining octets starting just past the end of |buffer|. This
  // should copy zero bytes and |target_buffer| should remain unchanged.
  buffer.Copy(&target_buffer, buffer.size(), 0);
  EXPECT_TRUE(ContainersEqual(target_buffer_copy, target_buffer));

  // Copied range must remain within buffer limits
  EXPECT_DEATH_IF_SUPPORTED(buffer.Copy(&target_buffer, buffer.size() + 1, 0),
                            "offset exceeds source range");
  EXPECT_DEATH_IF_SUPPORTED(buffer.Copy(&target_buffer, 0, buffer.size() + 1),
                            "end exceeds source range");
  EXPECT_DEATH_IF_SUPPORTED(
      buffer.Copy(&target_buffer, buffer.size() / 2 + 1, buffer.size() / 2 + 1),
      "end exceeds source range");

  StaticByteBuffer<1> insufficient_target_buffer;
  EXPECT_DEATH_IF_SUPPORTED(buffer.Copy(&insufficient_target_buffer),
                            "destination not large enough");

  // Range calculation overflow is fatal rather than silently erroneous
  MutableBufferView bogus_target_buffer(target_buffer.mutable_data(),
                                        std::numeric_limits<size_t>::max());
  EXPECT_DEATH_IF_SUPPORTED(
      buffer.Copy(&bogus_target_buffer, 1, std::numeric_limits<size_t>::max()),
      "end of source range overflows size_t");
}

TEST(ByteBufferTest, View) {
  StaticByteBuffer buffer('T', 'e', 's', 't');
  BufferView empty_buffer;

  BufferView view = empty_buffer.view();
  EXPECT_EQ(0u, view.size());

  view = empty_buffer.view(0, 200);
  EXPECT_EQ(0u, view.size());

  view = buffer.view();
  EXPECT_EQ("Test", view.AsString());

  view = buffer.view(4);
  EXPECT_EQ(0u, view.size());

  view = buffer.view(1);
  EXPECT_EQ("est", view.AsString());

  view = buffer.view(1, 1);
  EXPECT_EQ("e", view.AsString());

  view = buffer.view(1, 2);
  EXPECT_EQ("es", view.AsString());
}

TEST(ByteBufferTest, Span) {
  StaticByteBuffer buffer('T', 'e', 's', 't');
  BufferView empty_buffer;

  pw::span span = empty_buffer.subspan();
  EXPECT_EQ(0u, span.size());

  span = empty_buffer.subspan(0, 200);
  EXPECT_EQ(0u, span.size());

  span = buffer.subspan();
  EXPECT_THAT(
      span,
      ::testing::ElementsAreArray(
          {std::byte{'T'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}}));

  span = buffer.subspan(4);
  EXPECT_EQ(0u, span.size());

  span = buffer.subspan(1);
  EXPECT_THAT(span,
              ::testing::ElementsAreArray(
                  {std::byte{'e'}, std::byte{'s'}, std::byte{'t'}}));

  span = buffer.subspan(1, 1);
  EXPECT_THAT(span, ::testing::ElementsAreArray({std::byte{'e'}}));

  span = buffer.subspan(1, 2);
  EXPECT_THAT(span,
              ::testing::ElementsAreArray({std::byte{'e'}, std::byte{'s'}}));
}

TEST(ByteBufferTest, MutableView) {
  StaticByteBuffer buffer('T', 'e', 's', 't');
  MutableBufferView empty_buffer;

  MutableBufferView view;
  view = empty_buffer.mutable_view();
  EXPECT_EQ(0u, view.size());

  view = empty_buffer.mutable_view(0, 200);
  EXPECT_EQ(0u, view.size());

  view = buffer.mutable_view();
  EXPECT_EQ("Test", view.AsString());

  view = buffer.mutable_view(4);
  EXPECT_EQ(0u, view.size());

  view = buffer.mutable_view(1);
  EXPECT_EQ("est", view.AsString());

  view = buffer.mutable_view(1, 1);
  EXPECT_EQ("e", view.AsString());

  view = buffer.mutable_view(1, 2);
  EXPECT_EQ("es", view.AsString());

  // MutableView can modify the underlying buffer.
  view[0] = 'E';
  EXPECT_EQ("Es", view.AsString());
  EXPECT_EQ("TEst", buffer.AsString());
}

TEST(ByteBufferTest, ByteBufferEqualityFail) {
  const StaticByteBuffer kData0('T', 'e', 's', 't');
  const StaticByteBuffer kData1('F', 'o', 'o');
  EXPECT_FALSE(kData0 == kData1);
}

TEST(ByteBufferTest, ByteBufferEqualitySuccess) {
  const StaticByteBuffer kData0('T', 'e', 's', 't');
  const StaticByteBuffer kData1('T', 'e', 's', 't');
  EXPECT_TRUE(kData0 == kData1);
}

TEST(ByteBufferDeathTest, ByteBufferWithInsufficientBytesAssertsOnTo) {
  StaticByteBuffer buffer(0, 0, 0);
  ASSERT_GT(sizeof(float), buffer.size());
  EXPECT_DEATH_IF_SUPPORTED(
      [=] { [[maybe_unused]] auto _ = buffer.To<float>(); }(),
      "end exceeds source range");
}

template <typename T>
void TestByteBufferRoundtrip(std::initializer_list<T> values) {
  for (auto value : values) {
    BufferView value_view(&value, sizeof(value));
    auto to_value = value_view.To<T>();
    SCOPED_TRACE(value);
    EXPECT_EQ(0, std::memcmp(&value, &to_value, sizeof(T)));
  }
}

TEST(ByteBufferTest, ByteBufferToRoundtripOnPrimitives) {
  // Test values from absl::bit_cast tests
  {
    SCOPED_TRACE("bool");
    TestByteBufferRoundtrip<bool>({true, false});
  }
  {
    SCOPED_TRACE("array 1 of const bool");
    TestByteBufferRoundtrip<const bool[1]>({{true}, {false}});
  }
  {
    SCOPED_TRACE("int32_t");
    TestByteBufferRoundtrip<int32_t>(
        {0, 1, 100, 2147483647, -1, -100, -2147483647, -2147483647 - 1});
  }
  {
    SCOPED_TRACE("int64_t");
    TestByteBufferRoundtrip<int64_t>({0, 1, 1LL << 40, -1, -(1LL << 40)});
  }
  {
    SCOPED_TRACE("uint64_t");
    TestByteBufferRoundtrip<uint64_t>({0, 1, 1LLU << 40, 1LLU << 63});
  }
  {
    SCOPED_TRACE("float");
    TestByteBufferRoundtrip<float>({0.0f,
                                    1.0f,
                                    -1.0f,
                                    10.0f,
                                    -10.0f,
                                    1e10f,
                                    1e20f,
                                    1e-10f,
                                    1e-20f,
                                    2.71828f,
                                    3.14159f});
  }
  {
    SCOPED_TRACE("double");
    TestByteBufferRoundtrip<double>(
        {0.0,
         1.0,
         -1.0,
         10.0,
         -10.0,
         1e10,
         1e100,
         1e-10,
         1e-100,
         2.718281828459045,
         3.141592653589793238462643383279502884197169399375105820974944});
  }
}

TEST(ByteBufferTest, ByteBufferToStripsCvQualifiers) {
  auto to_value = StaticByteBuffer(2).To<const volatile char>();
  static_assert(std::is_same_v<char, decltype(to_value)>);
}

TEST(ByteBufferTest, ByteBufferWithAdditionalBytesToBasicType) {
  EXPECT_EQ(191u, StaticByteBuffer(191, 25).To<uint8_t>());
}

TEST(ByteBufferTest, ByteBufferToStruct) {
  const StaticByteBuffer data(10, 12);
  struct [[gnu::packed]] point {
    uint8_t x;
    uint8_t y;
  };
  EXPECT_EQ(10, data.To<point>().x);
  EXPECT_EQ(12, data.To<point>().y);
}

TEST(ByteBufferTest, ByteBufferToArray) {
  const StaticByteBuffer buf(191, 25);
  const auto array = buf.To<uint8_t[2]>();
  EXPECT_EQ(buf.size(), array.size());
  EXPECT_EQ(191, array[0]);
  EXPECT_EQ(25, array[1]);
}

TEST(ByteBufferTest, ByteBufferToDoesNotReadUnaligned) {
  const DynamicByteBuffer buf(2 * sizeof(float) - 1);

  // Advance past the float alignment boundary
  const size_t offset = std::distance(
      buf.begin(), std::find_if(buf.begin(), buf.end(), [](auto& b) {
        return reinterpret_cast<uintptr_t>(&b) % alignof(float) != 0;
      }));
  BufferView view = buf.view(offset, sizeof(float));

  // Prove that the data in the view isn't aligned for a float
  ASSERT_NE(0U, reinterpret_cast<uintptr_t>(view.data()) % alignof(float));

  // This cref binds to a new object that ByteBuffer::To creates, so the
  // alignment is correct
  const float& to_object = view.To<float>();
  EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(&to_object) % alignof(float));
}

TEST(ByteBufferTest, ByteBufferReadMember) {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC_CLANG(ignored, "-Wc99-extensions");
  struct [[gnu::packed]] Point {
    uint8_t x;
    const uint8_t y;
    const uint8_t array[2];
    char multi[2][1];
    uint8_t flex[];
  };
  PW_MODIFY_DIAGNOSTICS_POP();

  StaticByteBuffer data(0x01, 0x02, 0x03, 0x37, 0x7f, 0x02, 0x45);
  auto x = data.ReadMember<&Point::x>();
  static_assert(std::is_same_v<uint8_t, decltype(x)>);
  EXPECT_EQ(data[offsetof(Point, x)], x);

  // Top-level const qualifier is removed
  auto y = data.ReadMember<&Point::y>();
  static_assert(std::is_same_v<uint8_t, decltype(y)>);
  EXPECT_EQ(data[offsetof(Point, y)], y);

  // Returned array elements are const just like in the original struct
  auto array = data.ReadMember<&Point::array>();
  static_assert(std::is_same_v<std::array<const uint8_t, 2>, decltype(array)>);
  EXPECT_THAT(array, ::testing::ElementsAre(uint8_t{0x03}, uint8_t{0x37}));

  auto multi = data.ReadMember<&Point::multi>();
  static_assert(
      std::is_same_v<std::array<std::array<char, 1>, 2>, decltype(multi)>);
  EXPECT_THAT(
      multi,
      ::testing::ElementsAre(std::array{char{0x7f}}, std::array{char{0x02}}));
}

TEST(ByteBufferDeathTest, ByteBufferReadMemberOfFixedArrayType) {
  struct [[gnu::packed]] Point {
    float f;
    int8_t coordinates[3];
    char multi[2][1];
  };

  StaticByteBuffer data(
      // f
      0,
      0,
      0,
      0,

      // coordinates[3]
      0x01,
      0x02,
      0x03,

      // multi[2][1]
      0x37,
      0x45);
  ASSERT_LE(sizeof(Point), data.size());
  EXPECT_DEATH_IF_SUPPORTED(data.ReadMember<&Point::coordinates>(3),
                            "index past array bounds");

  auto view = data.view(0, 6);
  ASSERT_GT(sizeof(Point), view.size());
  EXPECT_DEATH_IF_SUPPORTED(view.ReadMember<&Point::coordinates>(0),
                            "insufficient buffer");

  EXPECT_EQ(data[offsetof(Point, coordinates)],
            data.ReadMember<&Point::coordinates>(0));
  EXPECT_EQ(data[offsetof(Point, coordinates) + 1],
            data.ReadMember<&Point::coordinates>(1));

  // Elements of a multi-dimensional C array are returned as std::arrays
  auto inner = data.ReadMember<&Point::multi>(1);
  EXPECT_EQ(data[offsetof(Point, multi) + 1], inner.at(0));
}

TEST(ByteBufferDeathTest, ByteBufferReadMemberOfStdArrayType) {
  struct [[gnu::packed]] Point {
    float f;
    std::array<int8_t, 3> coordinates;
  };

  StaticByteBuffer data(0, 0, 0, 0, 0x01, 0x02, 0x03, 0x37);
  ASSERT_LE(sizeof(Point), data.size());
  EXPECT_DEATH_IF_SUPPORTED(data.ReadMember<&Point::coordinates>(3),
                            "index past array bounds");

  EXPECT_EQ(data[offsetof(Point, coordinates)],
            data.ReadMember<&Point::coordinates>(0));
  EXPECT_EQ(data[offsetof(Point, coordinates) + 1],
            data.ReadMember<&Point::coordinates>(1));
}

TEST(ByteBufferDeathTest, ByteBufferReadMemberOfFlexibleArrayType) {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC_CLANG(ignored, "-Wc99-extensions");
  struct [[gnu::packed]] Point {
    uint16_t dimensions;
    int8_t coordinates[];
  };
  PW_MODIFY_DIAGNOSTICS_POP();

  StaticByteBuffer data(0, 0, 0x01, 0x02);
  ASSERT_LE(sizeof(Point), data.size());
  EXPECT_DEATH_IF_SUPPORTED(data.ReadMember<&Point::coordinates>(2),
                            "end exceeds source range");

  EXPECT_EQ(data[offsetof(Point, coordinates)],
            data.ReadMember<&Point::coordinates>(0));
  EXPECT_EQ(data[offsetof(Point, coordinates) + 1],
            data.ReadMember<&Point::coordinates>(1));
}

TEST(ByteBufferTest, ByteBufferReadMemberOfUnalignedArrayType) {
  struct [[gnu::packed]] Point {
    int8_t byte;
    float f[1];
  } point;

  BufferView view(&point, sizeof(point));
  static_assert(alignof(decltype(view.ReadMember<&Point::f>(0))) ==
                alignof(float));

  // This branch (that the second field of |point| is unaligned) does get taken
  // in manual testing but there's no way to guarantee it, so make it run-time
  // conditional.
  if (reinterpret_cast<uintptr_t>(&point.f) % alignof(float) != 0) {
    // Casting (which is UB here) a pointer into the buffer contents yields a
    // pointer that is not aligned to type requirements
    const float* unaligned_pointer =
        reinterpret_cast<const Point*>(view.data())->f;
    ASSERT_NE(0U,
              reinterpret_cast<uintptr_t>(unaligned_pointer) % alignof(float));
  }

  // The same cref binds to a temporary new object that ByteBuffer::ReadMember
  // creates, so the alignment is correct.
  const float& through_read_member = view.ReadMember<&Point::f>(0);
  EXPECT_EQ(0U,
            reinterpret_cast<uintptr_t>(&through_read_member) % alignof(float));
}

TEST(ByteBufferTest, MutableByteBufferAsMutableFundamental) {
  StaticByteBuffer data(10, 12);
  ++*data.AsMutable<uint8_t>();
  EXPECT_EQ(11, data[0]);
}

TEST(ByteBufferTest, MutableByteBufferAsMutableStruct) {
  StaticByteBuffer data(10, 12);
  struct point {
    uint8_t x;
    uint8_t y;
  };
  ++data.AsMutable<point>()->x;
  ++data.AsMutable<point>()->y;

  const StaticByteBuffer expected_data(11, 13);
  EXPECT_EQ(expected_data, data);
}

TEST(ByteBufferTest, MutableByteBufferAsMutableArray) {
  StaticByteBuffer buf(10, 12);
  uint8_t (&array)[2] = *buf.AsMutable<uint8_t[2]>();
  ++array[0];
  ++array[1];

  EXPECT_EQ(11, buf[0]);
  EXPECT_EQ(13, buf[1]);
}

TEST(ByteBufferDeathTest, MutableByteBufferWrite) {
  const StaticByteBuffer kData0('T', 'e', 's', 't');
  const StaticByteBuffer kData1('F', 'o', 'o');

  StaticByteBuffer buffer('X', 'X', 'X', 'X', 'X', 'X', 'X', 'X');
  EXPECT_EQ("XXXXXXXX", buffer.AsString());

  buffer.Write(kData0);
  EXPECT_EQ("TestXXXX", buffer.AsString());

  // Write from raw pointer.
  buffer = StaticByteBuffer('X', 'X', 'X', 'X', 'X', 'X', 'X', 'X');
  buffer.Write(kData0.data(), kData0.size());
  EXPECT_EQ("TestXXXX", buffer.AsString());

  // Write at offset.
  buffer.Write(kData1, 1);
  EXPECT_EQ("TFooXXXX", buffer.AsString());

  // Write at offset from raw pointer
  buffer.Write(kData1.data(), kData1.size(), 3);
  EXPECT_EQ("TFoFooXX", buffer.AsString());

  // Writing zero bytes should have no effect.
  buffer = StaticByteBuffer('X', 'X', 'X', 'X', 'X', 'X', 'X', 'X');
  buffer.Write(kData1.data(), 0u);
  buffer.Write(/*data=*/nullptr, 0u);  // Passing nullptr is OK when size is 0
  EXPECT_EQ("XXXXXXXX", buffer.AsString());

  // Writing zero bytes just past the buffer should be accepted (i.e. no
  // assertion) and have no effect.
  buffer.Write(kData1.data(), 0u, buffer.size());
  EXPECT_EQ("XXXXXXXX", buffer.AsString());

  // Buffer limits are strictly enforced
  EXPECT_DEATH_IF_SUPPORTED(buffer.Write(kData0.data(), buffer.size() + 1, 0),
                            "destination not large enough");
  EXPECT_DEATH_IF_SUPPORTED(buffer.Write(kData0.data(), 0, buffer.size() + 1),
                            "offset past buffer");
}

TEST(ByteBufferTest, AsString) {
  StaticByteBuffer buffer('T', 'e', 's', 't');
  EXPECT_EQ("Test", buffer.AsString());
}

TEST(ByteBufferTest, Fill) {
  StaticByteBuffer<5> buffer;
  buffer.Fill('A');
  EXPECT_EQ("AAAAA", buffer.AsString());
}

TEST(ByteBufferTest, ToVectorEmpty) {
  BufferView buffer;
  std::vector<uint8_t> vec = buffer.ToVector();
  EXPECT_TRUE(vec.empty());
}

TEST(ByteBufferTest, ToVector) {
  StaticByteBuffer buffer('h', 'e', 'l', 'l', 'o');
  std::vector<uint8_t> vec = buffer.ToVector();
  EXPECT_TRUE(ContainersEqual(vec, buffer));
}

TEST(ByteBufferTest, PrintableAscii) {
  StaticByteBuffer buffer('f', 'o', 'o', 'b', 'a', 'r', 'b', 'a', 'z');
  std::string result = buffer.Printable(3, 3);
  EXPECT_EQ("bar", result);
}

TEST(ByteBufferTest, PrintableUtf8) {
  StaticByteBuffer buffer(
      0xce, 0xba, 0xe1, 0xbd, 0xb9, 0xcf, 0x83, 0xce, 0xbc, 0xce, 0xB5);
  std::string result = buffer.Printable(0, buffer.size());
  EXPECT_EQ("κόσμε", result);
}

TEST(ByteBufferTest, PrintableInvalidUtf8) {
  StaticByteBuffer buffer(0xce, 0xba, 0x80);
  std::string result = buffer.Printable(0, buffer.size());
  EXPECT_EQ("...", result);
}

TEST(ByteBufferDeathTest, PrintableSizeTooLarge) {
  StaticByteBuffer buffer(0xce, 0xba, 0x80);
  ASSERT_DEATH({ buffer.Printable(0, buffer.size() + 1); }, "");
  ASSERT_DEATH({ buffer.Printable(1, buffer.size()); }, "");
  ASSERT_DEATH({ buffer.Printable(1, buffer.size() + 1); }, "");
}

}  // namespace
}  // namespace bt
