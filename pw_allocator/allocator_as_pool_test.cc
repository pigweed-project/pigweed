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

#include "pw_allocator/allocator_as_pool.h"

#include <cstddef>
#include <cstdint>

#include "pw_allocator/layout.h"
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fxitures.

using ::pw::allocator::AllocatorAsPool;
using ::pw::allocator::Layout;
using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<256>;

struct U64 {
  std::byte bytes[8];
};

class AllocatorAsPoolTest : public ::testing::Test {
 public:
  AllocatorAsPoolTest() : allocator_(), pool_(allocator_, Layout::Of<U64>()) {}

 protected:
  AllocatorForTest allocator_;
  AllocatorAsPool pool_;
};

// Unit tests.

TEST_F(AllocatorAsPoolTest, Capabilities) {
  EXPECT_EQ(pool_.capabilities(), allocator_.capabilities());
}

TEST_F(AllocatorAsPoolTest, AllocateDeallocate) {
  void* ptr = pool_.Allocate();
  ASSERT_NE(ptr, nullptr);
  pool_.Deallocate(ptr);
}

}  // namespace
