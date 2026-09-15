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

#include "pw_allocator/maybe_shared_ptr.h"

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#if PW_ALLOCATOR_HAS_ATOMICS

#include <cstddef>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/internal/counter.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::allocator::test::Counter;

class MaybeSharedPtrTest : public pw::allocator::test::TestWithCounters {
 protected:
  pw::allocator::test::AllocatorForTest<256> allocator_;
};

struct Foo {
  virtual ~Foo() = default;
  int foo() const { return val + 1; }
  int val = 0;
};

struct Bar : public Foo {
  int bar() const { return val + 2; }
};

struct Baz : public Bar {
  int baz() const { return val + 3; }
};

TEST_F(MaybeSharedPtrTest, DefaultInitializationIsNullptr) {
  pw::MaybeSharedPtr<int> empty;
  EXPECT_EQ(empty.get(), nullptr);
  EXPECT_EQ(empty, nullptr);
  EXPECT_EQ(empty.allocator(), nullptr);
}

TEST_F(MaybeSharedPtrTest, ConstructFromOwnedSharedPtr) {
  pw::SharedPtr<Counter> shared = allocator_.MakeShared<Counter>(42u);
  ASSERT_NE(shared, nullptr);

  {
    pw::MaybeSharedPtr<Counter> maybe(shared);
    EXPECT_NE(maybe, nullptr);
    EXPECT_EQ(maybe.get(), shared.get());
    EXPECT_EQ(maybe->value(), 42u);
    EXPECT_EQ((*maybe).value(), 42u);
    EXPECT_EQ(maybe.allocator(), &allocator_);
  }

  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, MoveConstructFromOwnedSharedPtr) {
  pw::SharedPtr<Counter> shared = allocator_.MakeShared<Counter>(42u);
  Counter* raw = shared.get();

  pw::MaybeSharedPtr<Counter> maybe(std::move(shared));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(shared, nullptr);
  EXPECT_NE(maybe, nullptr);
  EXPECT_EQ(maybe.get(), raw);
  EXPECT_EQ(maybe->value(), 42u);
}

TEST_F(MaybeSharedPtrTest, ConstructFromUnownedReference) {
  Counter counter(99u);
  {
    pw::MaybeSharedPtr<Counter> maybe =
        pw::MaybeSharedPtr<Counter>::Unowned(counter);
    EXPECT_NE(maybe, nullptr);
    EXPECT_EQ(maybe.get(), &counter);
    EXPECT_EQ(maybe->value(), 99u);
    EXPECT_EQ((*maybe).value(), 99u);
    EXPECT_EQ(maybe.allocator(), nullptr);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, ResetUnownedPointerDoesNotDestroyObject) {
  Counter counter(99u);
  pw::MaybeSharedPtr<Counter> maybe = pw::Unowned(counter);
  maybe.reset();
  EXPECT_EQ(maybe, nullptr);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, ConstructFromUnownedHelper) {
  Counter counter(123u);
  {
    auto maybe = pw::Unowned(counter);
    EXPECT_NE(maybe, nullptr);
    EXPECT_EQ(maybe.get(), &counter);
    EXPECT_EQ(maybe->value(), 123u);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, OwnedDestructionFreesObject) {
  {
    pw::MaybeSharedPtr<Counter> maybe(allocator_.MakeShared<Counter>(1u));
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, CopyOwned) {
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(10u));

  pw::MaybeSharedPtr<Counter> ptr2 = ptr1;
  EXPECT_EQ(ptr1.get(), ptr2.get());

  ptr1.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  ptr2.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, MoveOwned) {
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(10u));
  Counter* raw = ptr1.get();

  pw::MaybeSharedPtr<Counter> ptr2 = std::move(ptr1);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_NE(ptr2, nullptr);
  EXPECT_EQ(ptr2.get(), raw);

  ptr2.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, CopyUnowned) {
  Counter counter(55u);
  {
    pw::MaybeSharedPtr<Counter> ptr1 = pw::Unowned(counter);
    pw::MaybeSharedPtr<Counter> ptr2 = ptr1;
    EXPECT_EQ(ptr1.get(), &counter);
    EXPECT_EQ(ptr2.get(), &counter);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, MoveUnowned) {
  Counter counter(55u);
  {
    pw::MaybeSharedPtr<Counter> ptr1 = pw::Unowned(counter);
    pw::MaybeSharedPtr<Counter> ptr2 = std::move(ptr1);
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_EQ(ptr1, nullptr);
    EXPECT_EQ(ptr2.get(), &counter);
  }
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, ConvertingConstructorsForOwned) {
  pw::SharedPtr<Baz> baz = allocator_.MakeShared<Baz>();
  pw::MaybeSharedPtr<Bar> bar = baz;
  pw::MaybeSharedPtr<Foo> foo = bar;
  EXPECT_EQ(foo.get(), baz.get());
}

TEST_F(MaybeSharedPtrTest, ConvertingConstructorsForUnowned) {
  Baz static_baz;
  pw::MaybeSharedPtr<Baz> unowned_baz = pw::Unowned(static_baz);
  pw::MaybeSharedPtr<Foo> unowned_foo = unowned_baz;
  EXPECT_EQ(unowned_foo.get(), &static_baz);
}

TEST_F(MaybeSharedPtrTest, StaticPointerCast) {
  pw::SharedPtr<Baz> baz = allocator_.MakeShared<Baz>();
  pw::MaybeSharedPtr<Foo> foo = baz;

  pw::MaybeSharedPtr<Baz> cast_baz = pw::static_pointer_cast<Baz>(foo);
  EXPECT_EQ(cast_baz.get(), baz.get());

  Baz static_baz;
  pw::MaybeSharedPtr<Foo> unowned_foo = pw::Unowned<Foo>(static_baz);
  pw::MaybeSharedPtr<Baz> unowned_baz =
      pw::static_pointer_cast<Baz>(unowned_foo);
  EXPECT_EQ(unowned_baz.get(), &static_baz);
}

TEST_F(MaybeSharedPtrTest, ConstPointerCast) {
  pw::SharedPtr<Baz> baz = allocator_.MakeShared<Baz>();
  pw::MaybeSharedPtr<Baz> foo = baz;

  pw::MaybeSharedPtr<const Baz> const_baz = foo;
  pw::MaybeSharedPtr<Baz> non_const_baz =
      pw::const_pointer_cast<Baz>(const_baz);
  EXPECT_EQ(non_const_baz.get(), foo.get());
}

TEST_F(MaybeSharedPtrTest, CompareWithNullptr) {
  pw::MaybeSharedPtr<Counter> empty;
  EXPECT_EQ(empty, nullptr);
  EXPECT_EQ(nullptr, empty);

  Counter counter(1u);
  pw::MaybeSharedPtr<Counter> unowned = pw::Unowned(counter);
  EXPECT_NE(unowned, nullptr);
  EXPECT_NE(nullptr, unowned);
}

TEST_F(MaybeSharedPtrTest, CompareMaybeSharedPtrInstances) {
  Counter counter1(1u);
  Counter counter2(2u);

  pw::MaybeSharedPtr<Counter> unowned1 = pw::Unowned(counter1);
  pw::MaybeSharedPtr<Counter> unowned2 = pw::Unowned(counter2);
  pw::MaybeSharedPtr<Counter> unowned1_copy = pw::Unowned(counter1);

  EXPECT_EQ(unowned1, unowned1_copy);
  EXPECT_NE(unowned1, unowned2);
}

TEST_F(MaybeSharedPtrTest, CompareOwnedMaybeSharedPtrWithSharedPtr) {
  pw::SharedPtr<Counter> shared1 = allocator_.MakeShared<Counter>(10u);
  pw::MaybeSharedPtr<Counter> maybe_shared = shared1;
  EXPECT_EQ(maybe_shared, shared1);
  EXPECT_EQ(shared1, maybe_shared);

  pw::SharedPtr<Counter> shared2 = allocator_.MakeShared<Counter>(20u);
  EXPECT_NE(maybe_shared, shared2);
  EXPECT_NE(shared2, maybe_shared);
}

TEST_F(MaybeSharedPtrTest, CompareUnownedMaybeSharedPtrWithSharedPtr) {
  pw::SharedPtr<Counter> shared = allocator_.MakeShared<Counter>(10u);
  pw::MaybeSharedPtr<Counter> unowned_to_shared = pw::Unowned(*shared);
  pw::MaybeSharedPtr<Counter> maybe_shared = shared;

  EXPECT_EQ(unowned_to_shared, shared);
  EXPECT_EQ(shared, unowned_to_shared);
  EXPECT_EQ(unowned_to_shared, maybe_shared);
}

TEST_F(MaybeSharedPtrTest, Swap) {
  Counter counter(1u);
  pw::MaybeSharedPtr<Counter> unowned = pw::Unowned(counter);
  pw::MaybeSharedPtr<Counter> owned(allocator_.MakeShared<Counter>(2u));

  Counter* unowned_ptr = unowned.get();
  Counter* owned_ptr = owned.get();

  unowned.swap(owned);
  EXPECT_EQ(unowned.get(), owned_ptr);
  EXPECT_EQ(owned.get(), unowned_ptr);
}

TEST_F(MaybeSharedPtrTest, IsOwned) {
  pw::MaybeSharedPtr<int> empty;
  EXPECT_FALSE(empty.is_owned());

  pw::MaybeSharedPtr<int> null_init(nullptr);
  EXPECT_FALSE(null_init.is_owned());

  int val = 10;
  pw::MaybeSharedPtr<int> unowned = pw::Unowned(val);
  EXPECT_FALSE(unowned.is_owned());

  pw::SharedPtr<int> shared = allocator_.MakeShared<int>(20);
  pw::MaybeSharedPtr<int> owned(shared);
  EXPECT_TRUE(owned.is_owned());

  pw::MaybeSharedPtr<int> moved = std::move(owned);
  EXPECT_TRUE(moved.is_owned());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(owned.is_owned());

  moved.reset();
  EXPECT_FALSE(moved.is_owned());
}

TEST_F(MaybeSharedPtrTest, CopyAssignOwned) {
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(10u));
  pw::MaybeSharedPtr<Counter> ptr2;
  ptr2 = ptr1;
  EXPECT_EQ(ptr1.get(), ptr2.get());
  EXPECT_TRUE(ptr2.is_owned());

  ptr1.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  ptr2.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, CopyAssignDecreasesOldUseCount) {
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(10u));
  pw::MaybeSharedPtr<Counter> ptr2(allocator_.MakeShared<Counter>(20u));
  EXPECT_EQ(Counter::TakeNumCtorCalls(), 2u);

  ptr2 = ptr1;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
  EXPECT_EQ(ptr1.get(), ptr2.get());

  ptr1.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  ptr2.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, CopyAssignUnowned) {
  Counter counter1(10u);
  Counter counter2(20u);
  pw::MaybeSharedPtr<Counter> ptr1 = pw::Unowned(counter1);
  pw::MaybeSharedPtr<Counter> ptr2 = pw::Unowned(counter2);

  ptr2 = ptr1;
  EXPECT_EQ(ptr2.get(), &counter1);
  EXPECT_FALSE(ptr2.is_owned());
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, CopyAssignUnownedToOwnedFreesOldObject) {
  Counter counter(10u);
  pw::MaybeSharedPtr<Counter> ptr1 = pw::Unowned(counter);
  pw::MaybeSharedPtr<Counter> ptr2(allocator_.MakeShared<Counter>(20u));
  EXPECT_TRUE(ptr2.is_owned());

  ptr2 = ptr1;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
  EXPECT_EQ(ptr2.get(), &counter);
  EXPECT_FALSE(ptr2.is_owned());
}

TEST_F(MaybeSharedPtrTest, CopyAssignOwnedToUnownedDoesNotFreeUnowned) {
  Counter counter(10u);
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(20u));
  pw::MaybeSharedPtr<Counter> ptr2 = pw::Unowned(counter);
  EXPECT_FALSE(ptr2.is_owned());

  ptr2 = ptr1;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
  EXPECT_EQ(ptr2.get(), ptr1.get());
  EXPECT_TRUE(ptr2.is_owned());
}

TEST_F(MaybeSharedPtrTest, SelfCopyAssignOwned) {
  pw::MaybeSharedPtr<Counter> ptr(allocator_.MakeShared<Counter>(42u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(ptr.is_owned());
  EXPECT_EQ(ptr->value(), 42u);
  EXPECT_EQ(Counter::TakeNumCtorCalls(), 1u);

  ptr = *&ptr;

  EXPECT_NE(ptr, nullptr);
  EXPECT_TRUE(ptr.is_owned());
  EXPECT_EQ(ptr->value(), 42u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  ptr.reset();
  EXPECT_EQ(ptr, nullptr);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, SelfCopyAssignUnowned) {
  Counter counter(99u);
  pw::MaybeSharedPtr<Counter> ptr = pw::Unowned(counter);
  ASSERT_NE(ptr, nullptr);
  EXPECT_FALSE(ptr.is_owned());
  EXPECT_EQ(ptr.get(), &counter);
  EXPECT_EQ(ptr->value(), 99u);

  ptr = *&ptr;

  EXPECT_NE(ptr, nullptr);
  EXPECT_FALSE(ptr.is_owned());
  EXPECT_EQ(ptr.get(), &counter);
  EXPECT_EQ(ptr->value(), 99u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, ConvertingSelfCopyAssignOwned) {
  pw::MaybeSharedPtr<Counter> ptr(allocator_.MakeShared<Counter>(42u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_TRUE(ptr.is_owned());
  EXPECT_EQ(ptr->value(), 42u);

  ptr.operator= <Counter>(ptr);

  EXPECT_NE(ptr, nullptr);
  EXPECT_TRUE(ptr.is_owned());
  EXPECT_EQ(ptr->value(), 42u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, ConvertingSelfCopyAssignUnowned) {
  Counter counter(99u);
  pw::MaybeSharedPtr<Counter> ptr = pw::Unowned(counter);
  ASSERT_NE(ptr, nullptr);
  EXPECT_FALSE(ptr.is_owned());
  EXPECT_EQ(ptr.get(), &counter);
  EXPECT_EQ(ptr->value(), 99u);

  ptr.operator= <Counter>(ptr);

  EXPECT_NE(ptr, nullptr);
  EXPECT_FALSE(ptr.is_owned());
  EXPECT_EQ(ptr.get(), &counter);
  EXPECT_EQ(ptr->value(), 99u);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, MoveAssignOwned) {
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(10u));
  Counter* raw = ptr1.get();
  pw::MaybeSharedPtr<Counter> ptr2;

  ptr2 = std::move(ptr1);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_EQ(ptr2.get(), raw);
  EXPECT_TRUE(ptr2.is_owned());

  ptr2.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, MoveAssignDecreasesOldUseCount) {
  pw::MaybeSharedPtr<Counter> ptr1(allocator_.MakeShared<Counter>(10u));
  pw::MaybeSharedPtr<Counter> ptr2(allocator_.MakeShared<Counter>(20u));
  Counter* raw1 = ptr1.get();

  ptr2 = std::move(ptr1);
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_EQ(ptr2.get(), raw1);
}

TEST_F(MaybeSharedPtrTest, MoveAssignUnowned) {
  Counter counter(10u);
  pw::MaybeSharedPtr<Counter> ptr1 = pw::Unowned(counter);
  pw::MaybeSharedPtr<Counter> ptr2;

  ptr2 = std::move(ptr1);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_EQ(ptr2.get(), &counter);
  EXPECT_FALSE(ptr2.is_owned());
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, CopyAssignSharedPtr) {
  pw::SharedPtr<Counter> shared1 = allocator_.MakeShared<Counter>(42u);
  pw::MaybeSharedPtr<Counter> maybe;

  maybe = shared1;
  EXPECT_EQ(maybe.get(), shared1.get());
  EXPECT_TRUE(maybe.is_owned());

  pw::SharedPtr<Counter> shared2 = allocator_.MakeShared<Counter>(99u);
  maybe = shared2;
  EXPECT_EQ(maybe.get(), shared2.get());
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  shared1 = nullptr;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, MoveAssignSharedPtr) {
  pw::SharedPtr<Counter> shared = allocator_.MakeShared<Counter>(42u);
  Counter* raw = shared.get();
  pw::MaybeSharedPtr<Counter> maybe;

  maybe = std::move(shared);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(shared, nullptr);
  EXPECT_EQ(maybe.get(), raw);
  EXPECT_TRUE(maybe.is_owned());

  maybe.reset();
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, ConvertingMoveConstructorForOwned) {
  pw::MaybeSharedPtr<Baz> baz(allocator_.MakeShared<Baz>());
  Baz* raw = baz.get();
  pw::MaybeSharedPtr<Foo> foo(std::move(baz));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(baz, nullptr);
  EXPECT_EQ(foo.get(), raw);
  EXPECT_TRUE(foo.is_owned());
}

TEST_F(MaybeSharedPtrTest, ConvertingMoveConstructorForUnowned) {
  Baz static_baz;
  pw::MaybeSharedPtr<Baz> baz = pw::Unowned(static_baz);
  pw::MaybeSharedPtr<Foo> foo(std::move(baz));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(baz, nullptr);
  EXPECT_EQ(foo.get(), &static_baz);
  EXPECT_FALSE(foo.is_owned());
}

TEST_F(MaybeSharedPtrTest, ConvertingAssignments) {
  pw::MaybeSharedPtr<Baz> baz(allocator_.MakeShared<Baz>());
  pw::MaybeSharedPtr<Foo> foo;
  foo = baz;
  EXPECT_EQ(foo.get(), baz.get());
  EXPECT_TRUE(foo.is_owned());

  pw::MaybeSharedPtr<Baz> baz2(allocator_.MakeShared<Baz>());
  Baz* raw2 = baz2.get();
  foo = std::move(baz2);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(baz2, nullptr);
  EXPECT_EQ(foo.get(), raw2);

  pw::SharedPtr<Baz> shared_baz = allocator_.MakeShared<Baz>();
  foo = shared_baz;
  EXPECT_EQ(foo.get(), shared_baz.get());

  pw::SharedPtr<Baz> shared_baz2 = allocator_.MakeShared<Baz>();
  Baz* raw_shared2 = shared_baz2.get();
  foo = std::move(shared_baz2);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(shared_baz2, nullptr);
  EXPECT_EQ(foo.get(), raw_shared2);
}

TEST_F(MaybeSharedPtrTest, AssignNullptrToOwnedFreesObject) {
  pw::MaybeSharedPtr<Counter> maybe(allocator_.MakeShared<Counter>(42u));
  EXPECT_TRUE(maybe.is_owned());

  maybe = nullptr;
  EXPECT_EQ(maybe, nullptr);
  EXPECT_FALSE(maybe.is_owned());
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, AssignNullptrToUnownedDoesNotDestroyObject) {
  Counter counter(42u);
  pw::MaybeSharedPtr<Counter> maybe = pw::Unowned(counter);
  EXPECT_FALSE(maybe.is_owned());

  maybe = nullptr;
  EXPECT_EQ(maybe, nullptr);
  EXPECT_FALSE(maybe.is_owned());
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);
}

TEST_F(MaybeSharedPtrTest, CanExplicitlyDownCastOwned) {
  pw::MaybeSharedPtr<Foo> foo(allocator_.MakeShared<Baz>());
  pw::MaybeSharedPtr<Baz> baz = static_cast<pw::MaybeSharedPtr<Baz>>(foo);
  EXPECT_EQ(baz->foo(), 1);
  EXPECT_EQ(baz->bar(), 2);
  EXPECT_EQ(baz->baz(), 3);
  EXPECT_TRUE(baz.is_owned());
}

TEST_F(MaybeSharedPtrTest, CanExplicitlyDownCastUnowned) {
  Baz static_baz;
  pw::MaybeSharedPtr<Foo> foo = pw::Unowned<Foo>(static_baz);
  pw::MaybeSharedPtr<Baz> baz = static_cast<pw::MaybeSharedPtr<Baz>>(foo);
  EXPECT_EQ(baz->foo(), 1);
  EXPECT_EQ(baz->bar(), 2);
  EXPECT_EQ(baz->baz(), 3);
  EXPECT_FALSE(baz.is_owned());
}

TEST_F(MaybeSharedPtrTest, ConstPointerCastUnowned) {
  Baz static_baz;
  pw::MaybeSharedPtr<const Baz> const_baz = pw::Unowned(static_baz);
  pw::MaybeSharedPtr<Baz> non_const_baz =
      pw::const_pointer_cast<Baz>(const_baz);
  EXPECT_EQ(non_const_baz.get(), &static_baz);
  EXPECT_FALSE(non_const_baz.is_owned());
}

TEST_F(MaybeSharedPtrTest, PointerCastsOnNull) {
  pw::MaybeSharedPtr<Foo> empty_foo;
  pw::MaybeSharedPtr<Baz> cast_baz = pw::static_pointer_cast<Baz>(empty_foo);
  EXPECT_EQ(cast_baz, nullptr);
  EXPECT_FALSE(cast_baz.is_owned());

  pw::MaybeSharedPtr<const Foo> empty_const_foo;
  pw::MaybeSharedPtr<Foo> cast_non_const =
      pw::const_pointer_cast<Foo>(empty_const_foo);
  EXPECT_EQ(cast_non_const, nullptr);
  EXPECT_FALSE(cast_non_const.is_owned());
}

TEST_F(MaybeSharedPtrTest, SwapWhenOneIsEmpty) {
  Counter counter(111u);
  pw::MaybeSharedPtr<Counter> ptr1 = pw::Unowned(counter);
  pw::MaybeSharedPtr<Counter> ptr2;

  ptr1.swap(ptr2);
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_EQ(ptr2.get(), &counter);

  ptr1.swap(ptr2);
  EXPECT_EQ(ptr1.get(), &counter);
  EXPECT_EQ(ptr2, nullptr);
}

TEST_F(MaybeSharedPtrTest, SwapWhenBothAreEmpty) {
  pw::MaybeSharedPtr<Counter> ptr1;
  pw::MaybeSharedPtr<Counter> ptr2;
  ptr1.swap(ptr2);
  EXPECT_EQ(ptr1, nullptr);
  EXPECT_EQ(ptr2, nullptr);
}

TEST_F(MaybeSharedPtrTest, ResetEmptyPointerIsNoOp) {
  pw::MaybeSharedPtr<Counter> empty;
  empty.reset();
  EXPECT_EQ(empty, nullptr);
  EXPECT_FALSE(empty.is_owned());
}

TEST_F(MaybeSharedPtrTest, CompareEmptyWithEmpty) {
  pw::MaybeSharedPtr<Counter> empty1;
  pw::MaybeSharedPtr<Counter> empty2;
  EXPECT_EQ(empty1, empty2);
  EXPECT_FALSE(empty1 != empty2);

  pw::SharedPtr<Counter> empty_shared;
  EXPECT_EQ(empty1, empty_shared);
  EXPECT_EQ(empty_shared, empty1);
  EXPECT_FALSE(empty1 != empty_shared);
  EXPECT_FALSE(empty_shared != empty1);
}

TEST_F(MaybeSharedPtrTest, CompareOwnedWithNullptr) {
  pw::MaybeSharedPtr<Counter> owned(allocator_.MakeShared<Counter>(42u));
  EXPECT_NE(owned, nullptr);
  EXPECT_NE(nullptr, owned);
  EXPECT_FALSE(owned == nullptr);
  EXPECT_FALSE(nullptr == owned);
}

TEST_F(MaybeSharedPtrTest, CompareDifferentTypes) {
  pw::SharedPtr<Baz> baz = allocator_.MakeShared<Baz>();
  pw::MaybeSharedPtr<Foo> foo = baz;
  pw::MaybeSharedPtr<Bar> bar = baz;
  EXPECT_EQ(foo, bar);
  EXPECT_EQ(bar, foo);
  EXPECT_EQ(foo, baz);
  EXPECT_EQ(baz, foo);
}

TEST_F(MaybeSharedPtrTest, FreedExactlyOnceWithMultipleMaybeSharedPtr) {
  auto shared = allocator_.MakeShared<Counter>(42u);
  EXPECT_EQ(Counter::TakeNumCtorCalls(), 1u);

  pw::MaybeSharedPtr<Counter> maybe1 = shared;
  pw::MaybeSharedPtr<Counter> maybe2 = maybe1;

  shared = nullptr;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  maybe1 = nullptr;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 0u);

  maybe2 = nullptr;
  EXPECT_EQ(Counter::TakeNumDtorCalls(), 1u);
}

TEST_F(MaybeSharedPtrTest, AllocatorUpdatesOnResetAndMove) {
  pw::MaybeSharedPtr<Counter> owned(allocator_.MakeShared<Counter>(10u));
  EXPECT_EQ(owned.allocator(), &allocator_);

  pw::MaybeSharedPtr<Counter> moved = std::move(owned);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(owned.allocator(), nullptr);
  EXPECT_EQ(moved.allocator(), &allocator_);

  moved.reset();
  EXPECT_EQ(moved.allocator(), nullptr);
}

}  // namespace

#endif  // PW_ALLOCATOR_HAS_ATOMICS
