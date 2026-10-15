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

#include "pw_allocator/abstract_deallocator.h"

#include <cstddef>

#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace {

class FakeDeallocator : public ::pw::allocator::AbstractDeallocator {
 public:
  using AbstractDeallocator::GetAllocatedLayout;
  using AbstractDeallocator::GetRequestedLayout;
  using AbstractDeallocator::GetUsableLayout;
  using AbstractDeallocator::Recognizes;

 private:
  void DoDeallocate(void*) override {}
};

TEST(AbstractDeallocatorTest, GetCapacity) {
  FakeDeallocator deallocator;
  pw::StatusWithSize capacity = deallocator.GetCapacity();
  EXPECT_EQ(capacity.status(), pw::Status::Unimplemented());
  EXPECT_EQ(capacity.size(), 0U);
}

TEST(AbstractDeallocatorTest, GetRequestedLayout) {
  FakeDeallocator deallocator;
  std::byte byte{};
  auto result = deallocator.GetRequestedLayout(&byte);
  EXPECT_EQ(result.status(), pw::Status::Unimplemented());
}

TEST(AbstractDeallocatorTest, GetUsableLayout) {
  FakeDeallocator deallocator;
  std::byte byte{};
  auto result = deallocator.GetUsableLayout(&byte);
  EXPECT_EQ(result.status(), pw::Status::Unimplemented());
}

TEST(AbstractDeallocatorTest, GetAllocatedLayout) {
  FakeDeallocator deallocator;
  std::byte byte{};
  auto result = deallocator.GetAllocatedLayout(&byte);
  EXPECT_EQ(result.status(), pw::Status::Unimplemented());
}

TEST(AbstractDeallocatorTest, Recognizes) {
  FakeDeallocator deallocator;
  std::byte byte{};
  EXPECT_FALSE(deallocator.Recognizes(&byte));
}

}  // namespace
