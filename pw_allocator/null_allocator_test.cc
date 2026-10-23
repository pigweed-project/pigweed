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

#include "pw_allocator/null_allocator.h"

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::GetNullAllocator;
using ::pw::allocator::Layout;

TEST(NullAllocatorTest, Allocate) {
  pw::Allocator& allocator = GetNullAllocator();
  // Allocate should fail, regardless of size and alignment.
  for (size_t size = 1; size < 0x100; size <<= 1) {
    for (size_t alignment = 1; alignment < 0x100; alignment <<= 1) {
      EXPECT_EQ(allocator.Allocate(Layout(size, alignment)), nullptr);
    }
  }
}

}  // namespace
