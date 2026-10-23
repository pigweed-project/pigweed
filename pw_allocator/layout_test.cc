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

#include "pw_allocator/layout.h"

#include <cstddef>
#include <cstdint>

#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::Layout;

TEST(LayoutTest, TypeTraits) {
  static_assert(!pw::is_unbounded_array<int>::value);
  static_assert(pw::is_unbounded_array<int[]>::value);
  static_assert(!pw::is_unbounded_array<int[5]>::value);

  static_assert(!pw::is_bounded_array<int>::value);
  static_assert(!pw::is_bounded_array<int[]>::value);
  static_assert(pw::is_bounded_array<int[5]>::value);

  static_assert(!pw::is_unbounded_array_v<int>);
  static_assert(pw::is_unbounded_array_v<int[]>);
  static_assert(!pw::is_unbounded_array_v<int[5]>);

  static_assert(!pw::is_bounded_array_v<int>);
  static_assert(!pw::is_bounded_array_v<int[]>);
  static_assert(pw::is_bounded_array_v<int[5]>);
}

TEST(LayoutTest, DefaultConstructor) {
  Layout layout;
  EXPECT_EQ(layout.size(), 0U);
  EXPECT_EQ(layout.alignment(), alignof(std::max_align_t));
}

TEST(LayoutTest, SizeOnlyConstructor) {
  constexpr size_t kSize = 512;
  Layout layout(kSize);
  EXPECT_EQ(layout.size(), kSize);
  EXPECT_EQ(layout.alignment(), alignof(std::max_align_t));
}

TEST(LayoutTest, FullConstructor) {
  constexpr size_t kSize = 2048;
  constexpr size_t kAlignment = 4;
  Layout layout(kSize, kAlignment);
  EXPECT_EQ(layout.size(), kSize);
  EXPECT_EQ(layout.alignment(), kAlignment);
}

TEST(LayoutTest, ConstructUsingInitializer) {
  constexpr size_t kSize = 1024;
  constexpr size_t kAlignment = 8;
  Layout layout = {kSize, kAlignment};
  EXPECT_EQ(layout.size(), kSize);
  EXPECT_EQ(layout.alignment(), kAlignment);
}

TEST(LayoutTest, ConstructFromType) {
  struct Values {
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
  };
  Layout layout = Layout::Of<Values>();
  EXPECT_EQ(layout.size(), sizeof(Values));
  EXPECT_EQ(layout.alignment(), alignof(Values));
}

TEST(LayoutTest, Extend) {
  constexpr size_t kSize1 = 2048;
  constexpr size_t kSize2 = 1024;
  constexpr size_t kAlignment = 2;
  Layout layout1 = {kSize1, kAlignment};
  EXPECT_EQ(layout1.size(), kSize1);
  EXPECT_EQ(layout1.alignment(), kAlignment);

  Layout layout2 = layout1.Extend(kSize2);
  EXPECT_EQ(layout2.size(), kSize1 + kSize2);
  EXPECT_EQ(layout2.alignment(), kAlignment);
}

TEST(LayoutTest, UnwrapOk) {
  constexpr size_t kSize = 1024;
  constexpr size_t kAlignment = 8;
  pw::Result<Layout> result = Layout(kSize, kAlignment);
  Layout layout = Layout::Unwrap(result);
  EXPECT_EQ(layout.size(), kSize);
  EXPECT_EQ(layout.alignment(), kAlignment);
}

TEST(LayoutTest, UnwrapError) {
  pw::Result<Layout> result = pw::Status::Unimplemented();
  Layout layout = Layout::Unwrap(result);
  EXPECT_EQ(layout.size(), 0U);
  EXPECT_EQ(layout.alignment(), alignof(std::max_align_t));
}

}  // namespace
