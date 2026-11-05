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
  void Exhaust() { exhausted_ = true; }

 private:
  void* DoAllocate() override {
    if (exhausted_ || layout().size() > buffer_.size() ||
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
  bool exhausted_ = false;
};

struct TestItem {
  static inline int count = 0;

  int value = 0;

  TestItem() { ++count; }
  explicit TestItem(int v) : value(v) { ++count; }
  ~TestItem() { --count; }
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

TEST(AbstractPoolTest, New) {
  FakePool pool(Layout::Of<TestItem>());
  TestItem::count = 0;

  TestItem* item = pool.New<TestItem>(42);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(pool.allocated());
  EXPECT_EQ(item->value, 42);
  EXPECT_EQ(TestItem::count, 1);

  pool.Delete(item);
  EXPECT_EQ(TestItem::count, 0);
  EXPECT_EQ(pool.deallocated(), item);

  pool.Exhaust();
  EXPECT_EQ(pool.New<TestItem>(10), nullptr);
}

TEST(AbstractPoolTest, NewBoundedArray) {
  constexpr size_t kCount = 4;
  FakePool pool(Layout::Of<TestItem[kCount]>());
  TestItem::count = 0;

  TestItem* items = pool.New<TestItem[kCount]>();
  ASSERT_NE(items, nullptr);
  EXPECT_TRUE(pool.allocated());
  EXPECT_EQ(TestItem::count, static_cast<int>(kCount));
  for (size_t i = 0; i < kCount; ++i) {
    items[i].value = static_cast<int>(i + 1);
  }
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(items[i].value, static_cast<int>(i + 1));
  }

  pool.Delete<TestItem[kCount]>(items);
  EXPECT_EQ(TestItem::count, 0);
  EXPECT_EQ(pool.deallocated(), items);

  pool.Exhaust();
  EXPECT_EQ(pool.New<TestItem[kCount]>(), nullptr);
}

TEST(AbstractPoolTest, NewUnboundedArray) {
  constexpr size_t kCount = 4;
  FakePool pool(Layout::Of<TestItem[kCount]>());
  TestItem::count = 0;

  TestItem* items = pool.New<TestItem[]>();
  ASSERT_NE(items, nullptr);
  EXPECT_TRUE(pool.allocated());
  EXPECT_EQ(TestItem::count, static_cast<int>(kCount));
  for (size_t i = 0; i < kCount; ++i) {
    items[i].value = static_cast<int>(i + 1);
  }
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(items[i].value, static_cast<int>(i + 1));
  }

  pool.Delete<TestItem[]>(items, kCount);
  EXPECT_EQ(TestItem::count, 0);
  EXPECT_EQ(pool.deallocated(), items);

  pool.Exhaust();
  EXPECT_EQ(pool.New<TestItem[]>(), nullptr);
}

TEST(AbstractPoolTest, MakeUnique) {
  FakePool pool(Layout::Of<TestItem>());
  TestItem::count = 0;

  {
    auto item = pool.MakeUnique<TestItem>(42);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(pool.allocated());
    EXPECT_EQ(item->value, 42);
    EXPECT_EQ(TestItem::count, 1);
  }
  EXPECT_EQ(TestItem::count, 0);
  EXPECT_NE(pool.deallocated(), nullptr);

  pool.Exhaust();
  auto item = pool.MakeUnique<TestItem>(10);
  EXPECT_EQ(item, nullptr);
}

TEST(AbstractPoolTest, MakeUniqueBoundedArray) {
  constexpr size_t kCount = 4;
  FakePool pool(Layout::Of<TestItem[kCount]>());
  TestItem::count = 0;

  {
    auto items = pool.MakeUnique<TestItem[kCount]>();
    ASSERT_NE(items, nullptr);
    EXPECT_TRUE(pool.allocated());
    EXPECT_EQ(TestItem::count, static_cast<int>(kCount));
    for (size_t i = 0; i < kCount; ++i) {
      items[i].value = static_cast<int>(i + 1);
    }
    for (size_t i = 0; i < kCount; ++i) {
      EXPECT_EQ(items[i].value, static_cast<int>(i + 1));
    }
  }
  EXPECT_EQ(TestItem::count, 0);
  EXPECT_NE(pool.deallocated(), nullptr);

  pool.Exhaust();
  auto items = pool.MakeUnique<TestItem[kCount]>();
  EXPECT_EQ(items, nullptr);
}

TEST(AbstractPoolTest, MakeUniqueUnboundedArray) {
  constexpr size_t kCount = 4;
  FakePool pool(Layout::Of<TestItem[kCount]>());
  TestItem::count = 0;

  {
    auto items = pool.MakeUnique<TestItem[]>();
    ASSERT_NE(items, nullptr);
    EXPECT_TRUE(pool.allocated());
    EXPECT_EQ(items.size(), kCount);
    EXPECT_EQ(TestItem::count, static_cast<int>(kCount));
    for (size_t i = 0; i < kCount; ++i) {
      items[i].value = static_cast<int>(i + 1);
    }
    for (size_t i = 0; i < kCount; ++i) {
      EXPECT_EQ(items[i].value, static_cast<int>(i + 1));
    }
  }
  EXPECT_EQ(TestItem::count, 0);
  EXPECT_NE(pool.deallocated(), nullptr);

  pool.Exhaust();
  auto items = pool.MakeUnique<TestItem[]>();
  EXPECT_EQ(items, nullptr);
}

}  // namespace
