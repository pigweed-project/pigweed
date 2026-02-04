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

#include "pw_multibuf/from_span.h"

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v1 {
namespace {

using ::pw::allocator::test::AllocatorForTest;

constexpr size_t kArbitraryBufferSize = 1024;
constexpr size_t kArbitraryMetaSize = 1024;

struct DestroyState {
  size_t count = 0;
  ByteSpan last_region = {};
};

TEST(SimpleAllocator, AllocateWholeDataAreaSizeSucceeds) {
  std::array<std::byte, kArbitraryBufferSize> data_area;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc;
  DestroyState destroy_state;
  std::optional<MultiBuf> buf = pw::multibuf::FromSpan(
      meta_alloc, data_area, [&destroy_state](ByteSpan span) {
        ++destroy_state.count;
        destroy_state.last_region = span;
      });
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->size(), kArbitraryBufferSize);
  ASSERT_TRUE(buf->IsContiguous());
  EXPECT_EQ(buf->ContiguousSpan()->data(), data_area.data());
  EXPECT_EQ(destroy_state.count, 0u);
  buf = std::nullopt;
  EXPECT_EQ(destroy_state.count, 1u);
  EXPECT_EQ(destroy_state.last_region.size(), kArbitraryBufferSize);
  EXPECT_EQ(destroy_state.last_region.data(), data_area.data());
}

}  // namespace
}  // namespace pw::multibuf::v1
