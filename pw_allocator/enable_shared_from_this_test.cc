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

#include "pw_allocator/enable_shared_from_this.h"

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#if PW_ALLOCATOR_HAS_ATOMICS

#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_allocator/testing.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_allocator/weak_ptr.h"
#include "pw_unit_test/framework.h"

namespace {

class EnableSharedFromThisTest : public ::testing::Test {
 protected:
  pw::allocator::test::AllocatorForTest<256> allocator_;
};

struct Item : public pw::EnableSharedFromThis<Item> {
  int value = 42;
};

TEST_F(EnableSharedFromThisTest, BasicSharedFromThis) {
  pw::SharedPtr<Item> item = allocator_.MakeShared<Item>();
  ASSERT_NE(item.get(), nullptr);
  EXPECT_EQ(item.use_count(), 1);

  pw::SharedPtr<Item> item2 = item->shared_from_this();
  EXPECT_EQ(item2.get(), item.get());
  EXPECT_EQ(item.use_count(), 2);
  EXPECT_EQ(item2.use_count(), 2);
  EXPECT_EQ(item2->value, 42);
}

TEST_F(EnableSharedFromThisTest, ConstSharedFromThis) {
  const pw::SharedPtr<Item> item = allocator_.MakeShared<Item>();
  ASSERT_NE(item.get(), nullptr);

  pw::SharedPtr<const Item> item_const = item->shared_from_this();
  EXPECT_EQ(item_const.get(), item.get());
  EXPECT_EQ(item_const->value, 42);
}

TEST_F(EnableSharedFromThisTest, MakeSharedConstItem) {
  pw::SharedPtr<const Item> item = allocator_.MakeShared<const Item>();
  ASSERT_NE(item.get(), nullptr);
  EXPECT_EQ(item.use_count(), 1);

  pw::SharedPtr<const Item> item2 = item->shared_from_this();
  EXPECT_EQ(item2.get(), item.get());
  EXPECT_EQ(item.use_count(), 2);
  EXPECT_EQ(item2.use_count(), 2);
  EXPECT_EQ(item2->value, 42);

  pw::WeakPtr<const Item> weak = item->weak_from_this();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(weak.use_count(), 2);
  EXPECT_EQ(weak.Lock().get(), item.get());
}

TEST_F(EnableSharedFromThisTest, WeakFromThisActive) {
  pw::SharedPtr<Item> item = allocator_.MakeShared<Item>();
  ASSERT_NE(item.get(), nullptr);

  pw::WeakPtr<Item> weak = item->weak_from_this();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(weak.use_count(), 1);

  pw::SharedPtr<Item> locked = weak.Lock();
  EXPECT_EQ(locked.get(), item.get());
}

TEST_F(EnableSharedFromThisTest, ConstWeakFromThisActive) {
  const pw::SharedPtr<Item> item = allocator_.MakeShared<Item>();
  ASSERT_NE(item.get(), nullptr);

  pw::WeakPtr<const Item> weak = item->weak_from_this();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(weak.use_count(), 1);

  pw::SharedPtr<const Item> locked = weak.Lock();
  EXPECT_EQ(locked.get(), item.get());
}

TEST_F(EnableSharedFromThisTest, WeakFromThisWhenUnmanaged) {
  // Stack allocation
  Item stack_item;
  EXPECT_TRUE(stack_item.weak_from_this().expired());
  EXPECT_EQ(stack_item.weak_from_this().Lock(), nullptr);

  // Managed by UniquePtr
  {
    pw::UniquePtr<Item> unique_item = allocator_.MakeUnique<Item>();
    ASSERT_NE(unique_item.get(), nullptr);
    EXPECT_TRUE(unique_item->weak_from_this().expired());
    EXPECT_EQ(unique_item->weak_from_this().Lock(), nullptr);
  }

  // After SharedPtr expiration
  pw::WeakPtr<Item> weak;
  {
    pw::SharedPtr<Item> shared_item = allocator_.MakeShared<Item>();
    weak = shared_item->weak_from_this();
    EXPECT_FALSE(weak.expired());
  }
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(weak.Lock(), nullptr);
  weak.reset();
  EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(EnableSharedFromThisTest, CopyAndMoveDoNotCopyWeakThis) {
  pw::SharedPtr<Item> original = allocator_.MakeShared<Item>();
  ASSERT_NE(original.get(), nullptr);
  original->value = 100;

  // Copy construction
  Item copy = *original;
  EXPECT_EQ(copy.value, 100);
  EXPECT_TRUE(copy.weak_from_this().expired());
  EXPECT_FALSE(original->weak_from_this().expired());

  // Copy assignment
  Item copy2;
  copy2 = *original;
  EXPECT_EQ(copy2.value, 100);
  EXPECT_TRUE(copy2.weak_from_this().expired());

  // Move construction
  Item moved = std::move(*original);
  EXPECT_EQ(moved.value, 100);
  EXPECT_TRUE(moved.weak_from_this().expired());
  EXPECT_FALSE(original->weak_from_this().expired());

  // Move assignment
  pw::SharedPtr<Item> original2 = allocator_.MakeShared<Item>();
  ASSERT_NE(original2.get(), nullptr);
  original2->value = 200;
  Item moved2;
  moved2 = std::move(*original2);
  EXPECT_EQ(moved2.value, 200);
  EXPECT_TRUE(moved2.weak_from_this().expired());
  EXPECT_FALSE(original2->weak_from_this().expired());
}

TEST_F(EnableSharedFromThisTest, ConstructFromUniquePtr) {
  pw::UniquePtr<Item> unique = allocator_.MakeUnique<Item>();
  ASSERT_NE(unique.get(), nullptr);
  EXPECT_TRUE(unique->weak_from_this().expired());

  pw::SharedPtr<Item> shared(unique);
  ASSERT_NE(shared.get(), nullptr);
  EXPECT_FALSE(shared->weak_from_this().expired());

  pw::SharedPtr<Item> from_this = shared->shared_from_this();
  EXPECT_EQ(from_this.get(), shared.get());
  EXPECT_EQ(shared.use_count(), 2);
}

TEST_F(EnableSharedFromThisTest, ConstructFromUniquePtrConstItem) {
  pw::UniquePtr<const Item> unique = allocator_.MakeUnique<const Item>();
  ASSERT_NE(unique.get(), nullptr);
  EXPECT_TRUE(unique->weak_from_this().expired());

  pw::SharedPtr<const Item> shared(unique);
  ASSERT_NE(shared.get(), nullptr);
  EXPECT_EQ(unique.get(), nullptr);
  EXPECT_EQ(shared.use_count(), 1);

  pw::SharedPtr<const Item> from_this = shared->shared_from_this();
  EXPECT_EQ(from_this.get(), shared.get());
  EXPECT_EQ(shared.use_count(), 2);
  EXPECT_EQ(from_this.use_count(), 2);

  pw::WeakPtr<const Item> weak = shared->weak_from_this();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(weak.use_count(), 2);
  EXPECT_EQ(weak.Lock().get(), shared.get());
}

TEST_F(EnableSharedFromThisTest, MultipleInheritance) {
  struct OtherBase {
    virtual ~OtherBase() = default;
    int x = 111;
  };
  struct MultiItem : public OtherBase,
                     public pw::EnableSharedFromThis<MultiItem> {
    int y = 222;
  };

  pw::SharedPtr<MultiItem> multi = allocator_.MakeShared<MultiItem>();
  ASSERT_NE(multi.get(), nullptr);

  pw::SharedPtr<MultiItem> from_this = multi->shared_from_this();
  EXPECT_EQ(from_this.get(), multi.get());
  EXPECT_EQ(from_this->x, 111);
  EXPECT_EQ(from_this->y, 222);
}

TEST_F(EnableSharedFromThisTest, BaseClassEnableSharedFromThis) {
  struct Base : public pw::EnableSharedFromThis<Base> {
    virtual ~Base() = default;
    int a = 1;
  };
  struct Derived : public Base {
    int b = 2;
  };

  pw::SharedPtr<Derived> derived = allocator_.MakeShared<Derived>();
  ASSERT_NE(derived.get(), nullptr);

  pw::SharedPtr<Base> base_sp = derived->shared_from_this();
  EXPECT_EQ(base_sp.get(), static_cast<Base*>(derived.get()));
  EXPECT_EQ(base_sp->a, 1);
  EXPECT_EQ(derived.use_count(), 2);
}

TEST_F(EnableSharedFromThisTest, MultipleInheritanceNonZeroOffsetBase) {
  struct LeftBase {
    virtual ~LeftBase() = default;
    int x = 111;
  };
  struct RightBase : public pw::EnableSharedFromThis<RightBase> {
    virtual ~RightBase() = default;
    int y = 222;
  };
  struct MultiDerived : public LeftBase, public RightBase {
    int z = 333;
  };

  pw::SharedPtr<MultiDerived> derived = allocator_.MakeShared<MultiDerived>();
  ASSERT_NE(derived.get(), nullptr);

  // RightBase is at a non-zero offset from MultiDerived.
  RightBase* right_raw = static_cast<RightBase*>(derived.get());
  EXPECT_NE(static_cast<void*>(right_raw), static_cast<void*>(derived.get()));

  pw::SharedPtr<RightBase> right_sp = derived->RightBase::shared_from_this();
  EXPECT_EQ(right_sp.get(), right_raw);
  EXPECT_EQ(right_sp->y, 222);
  EXPECT_EQ(derived.use_count(), 2);
}

TEST_F(EnableSharedFromThisTest, DestructorAccessesWeakFromThisSafely) {
  struct InspectInDtor : public pw::EnableSharedFromThis<InspectInDtor> {
    bool* dtor_ran;
    bool* was_expired;
    bool* lock_failed;

    InspectInDtor(bool* ran, bool* expired, bool* failed)
        : dtor_ran(ran), was_expired(expired), lock_failed(failed) {}

    ~InspectInDtor() {
      *dtor_ran = true;
      *was_expired = weak_from_this().expired();
      pw::SharedPtr<InspectInDtor> locked = weak_from_this().Lock();
      *lock_failed = (locked == nullptr);
    }
  };

  bool dtor_ran = false;
  bool was_expired = false;
  bool lock_failed = false;
  {
    pw::SharedPtr<InspectInDtor> ptr = allocator_.MakeShared<InspectInDtor>(
        &dtor_ran, &was_expired, &lock_failed);
    ASSERT_NE(ptr.get(), nullptr);
  }
  EXPECT_TRUE(dtor_ran);
  EXPECT_TRUE(was_expired);
  EXPECT_TRUE(lock_failed);
  EXPECT_EQ(allocator_.metrics().num_deallocations.value(), 1u);
  EXPECT_EQ(allocator_.metrics().allocated_bytes.value(), 0u);
}

TEST_F(EnableSharedFromThisTest, ArrayTypesDoNotEnableSharedFromThis) {
  pw::SharedPtr<Item[]> shared = allocator_.MakeShared<Item[]>(2);
  ASSERT_NE(shared.get(), nullptr);
  EXPECT_TRUE(shared[0].weak_from_this().expired());
  EXPECT_TRUE(shared[1].weak_from_this().expired());

  pw::UniquePtr<Item[]> unique = allocator_.MakeUnique<Item[]>(2);
  ASSERT_NE(unique.get(), nullptr);
  EXPECT_TRUE(unique[0].weak_from_this().expired());
  EXPECT_TRUE(unique[1].weak_from_this().expired());

  pw::SharedPtr<Item[]> from_unique(unique);
  ASSERT_NE(from_unique.get(), nullptr);
  EXPECT_TRUE(from_unique[0].weak_from_this().expired());
  EXPECT_TRUE(from_unique[1].weak_from_this().expired());
}

}  // namespace

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#endif  // PW_ALLOCATOR_HAS_ATOMICS
