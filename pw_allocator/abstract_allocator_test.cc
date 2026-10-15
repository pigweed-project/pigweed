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

#include "pw_allocator/abstract_allocator.h"

#include <cstddef>
#include <optional>

#include "pw_allocator/layout.h"
#include "pw_unit_test/framework.h"

namespace {

class FakeAllocator : public ::pw::allocator::AbstractAllocator {
 private:
  void* DoAllocate(Layout) override { return nullptr; }
  void DoDeallocate(void*) override {}
};

TEST(AbstractAllocatorTest, GetAllocated) {
  FakeAllocator allocator;
  EXPECT_EQ(allocator.GetAllocated(), std::numeric_limits<size_t>::max());
}

TEST(AbstractAllocatorTest, MeasureFragmentation) {
  FakeAllocator allocator;
  EXPECT_EQ(allocator.MeasureFragmentation(), std::nullopt);
}

}  // namespace
