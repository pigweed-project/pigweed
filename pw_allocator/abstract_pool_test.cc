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

#include "pw_allocator/abstract_pool.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/capability.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/layout.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::AbstractPool;
using ::pw::allocator::Capabilities;
using ::pw::allocator::Capability;
using ::pw::allocator::Layout;

constexpr Layout kLayout = Layout::Of<uint32_t>();

class FakePool : public AbstractPool {
 public:
  constexpr explicit FakePool(const Layout& layout) : AbstractPool(layout) {}

  constexpr FakePool(Capabilities capabilities, const Layout& layout)
      : AbstractPool(capabilities, layout) {}

  using AbstractPool::GetAllocatedLayout;
  using AbstractPool::GetRequestedLayout;
  using AbstractPool::GetUsableLayout;
  using AbstractPool::Recognizes;

  bool allocated() const { return allocated_; }
  void* deallocated() const { return deallocated_; }

 private:
  void* DoAllocate() override {
    if (layout().size() > buffer_.size() ||
        layout().alignment() > alignof(std::max_align_t)) {
      return nullptr;
    }
    allocated_ = true;
    return buffer_.data();
  }

  void DoDeallocate(void* ptr) override { deallocated_ = ptr; }

  alignas(std::max_align_t) std::array<std::byte, 64> buffer_{};
  bool allocated_ = false;
  void* deallocated_ = nullptr;
};

TEST(AbstractPoolTest, CapabilitiesDefault) {
  FakePool pool(kLayout);
  EXPECT_EQ(pool.capabilities(), Capabilities());
  EXPECT_EQ(pool.layout(), kLayout);
}

TEST(AbstractPoolTest, CapabilitiesExplicit) {
  FakePool pool(Capability::kSkipsDestroy, kLayout);
  EXPECT_EQ(pool.capabilities(), Capability::kSkipsDestroy);
  EXPECT_TRUE(pool.HasCapability(Capability::kSkipsDestroy));
  EXPECT_EQ(pool.layout(), kLayout);
}

TEST(AbstractPoolTest, AllocateAndDeallocate) {
  FakePool pool(kLayout);
  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(pool.allocated());

  pool.Deallocate(ptr);
  EXPECT_EQ(pool.deallocated(), ptr);
}

TEST(AbstractPoolTest, GetCapacity) {
  FakePool pool(kLayout);
  pw::StatusWithSize capacity = pool.GetCapacity();
  EXPECT_EQ(capacity.status(), pw::Status::Unimplemented());
  EXPECT_EQ(capacity.size(), 0U);
}

TEST(AbstractPoolTest, GetRequestedLayout) {
  FakePool pool(kLayout);
  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  auto result = pool.GetRequestedLayout(ptr);
  EXPECT_EQ(result.status(), pw::Status::Unimplemented());
}

TEST(AbstractPoolTest, GetUsableLayout) {
  FakePool pool(kLayout);
  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  auto result = pool.GetUsableLayout(ptr);
  EXPECT_EQ(result.status(), pw::Status::Unimplemented());
}

TEST(AbstractPoolTest, GetAllocatedLayout) {
  FakePool pool(kLayout);
  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  auto result = pool.GetAllocatedLayout(ptr);
  EXPECT_EQ(result.status(), pw::Status::Unimplemented());
}

TEST(AbstractPoolTest, Recognizes) {
  FakePool pool(kLayout);
  void* ptr = pool.Allocate();
  ASSERT_NE(ptr, nullptr);
  EXPECT_FALSE(pool.Recognizes(ptr));
}

}  // namespace
