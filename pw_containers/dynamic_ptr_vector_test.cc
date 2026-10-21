// Copyright 2025 The Pigweed Authors
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

#include "pw_containers/dynamic_ptr_vector.h"

#include <cstddef>
#include <type_traits>
#include <utility>

#include "pw_allocator/fault_injecting_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::containers::test::Counter;

class DynamicPtrVectorTest : public ::testing::Test {
 protected:
  DynamicPtrVectorTest() : allocator_(allocator_for_test_) {}

  pw::allocator::test::AllocatorForTest<2048> allocator_for_test_;
  pw::allocator::test::FaultInjectingAllocator allocator_;
};

TEST_F(DynamicPtrVectorTest, Constructor) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_GE(vec.ptr_capacity(), 0u);
}

TEST_F(DynamicPtrVectorTest, Data) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.push_back(Counter(10));
  vec.push_back(Counter(20));

  ASSERT_EQ(vec.size(), 2u);
  EXPECT_EQ((*vec.data())->value, 10);
  EXPECT_EQ(vec.data()[1]->value, 20);

  *vec.data()[1] = Counter(99);
  EXPECT_EQ(vec[1], 99);

  static_assert(
      std::is_const_v<std::remove_reference_t<decltype(vec.data()[0])>>);
  static_assert(
      !std::is_const_v<std::remove_reference_t<decltype(*vec.data()[0])>>);

  static_assert(
      std::is_const_v<
          std::remove_reference_t<decltype(std::as_const(vec).data()[0])>>);
  static_assert(
      std::is_const_v<
          std::remove_reference_t<decltype(*std::as_const(vec).data()[0])>>);
}

TEST_F(DynamicPtrVectorTest, PushBackAndAccess) {
  pw::DynamicPtrVector<Counter> vec(allocator_);

  vec.push_back(Counter(10));
  vec.push_back(Counter(20));
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0].value, 10);
  EXPECT_EQ(vec[1].value, 20);
  EXPECT_EQ(vec.front().value, 10);
  EXPECT_EQ(vec.back().value, 20);
}

TEST_F(DynamicPtrVectorTest, EmplaceBack) {
  pw::DynamicPtrVector<Counter> vec(allocator_);

  vec.emplace_back(100);
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0].value, 100);
}

TEST_F(DynamicPtrVectorTest, PopBack) {
  pw::DynamicPtrVector<Counter> vec(allocator_);

  vec.emplace_back(1);
  vec.emplace_back(2);
  EXPECT_EQ(vec.size(), 2u);

  vec.pop_back();
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec.back().value, 1);

  vec.pop_back();
  EXPECT_TRUE(vec.empty());
}

TEST_F(DynamicPtrVectorTest, Iterators) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);

  int expected = 1;
  for (auto& item : vec) {
    EXPECT_EQ(item.value, expected++);
  }
  EXPECT_EQ(expected, 4);

  auto it = vec.begin();
  EXPECT_EQ(it->value, 1);
  EXPECT_EQ((it + 1)->value, 2);
  EXPECT_EQ((it + 2)->value, 3);

  EXPECT_EQ(it[0].value, 1);
  EXPECT_EQ(it[1].value, 2);

  ++it;
  EXPECT_EQ(it->value, 2);
  it--;
  EXPECT_EQ(it->value, 1);
}

TEST_F(DynamicPtrVectorTest, Erase) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);
  vec.emplace_back(4);

  auto it = vec.erase(vec.begin() + 1);
  EXPECT_EQ(it->value, 3);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0].value, 1);
  EXPECT_EQ(vec[1].value, 3);
  EXPECT_EQ(vec[2].value, 4);

  it = vec.erase(vec.begin() + 1, vec.end());
  EXPECT_EQ(it, vec.end());
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0].value, 1);
}

TEST_F(DynamicPtrVectorTest, Clear) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  EXPECT_FALSE(vec.empty());

  size_t allocated_before = allocator_for_test_.GetAllocated();
  vec.clear();
  size_t allocated_after = allocator_for_test_.GetAllocated();
  EXPECT_LT(allocated_after, allocated_before);
}

TEST_F(DynamicPtrVectorTest, Reset) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  EXPECT_FALSE(vec.empty());
  Counter::Reset();

  vec.reset();

  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_EQ(vec.ptr_capacity(), 0u);
  EXPECT_EQ(vec.data(), nullptr);
  EXPECT_EQ(Counter::destroyed, 2);
}

TEST_F(DynamicPtrVectorTest, MoveAssign_DestroysOldElementsAndFreesBuffer) {
  pw::DynamicPtrVector<Counter> vec1(allocator_);
  pw::DynamicPtrVector<Counter> vec2(allocator_);

  vec1.emplace_back(1);
  vec1.emplace_back(2);

  vec2.emplace_back(3);
  vec2.emplace_back(4);
  vec2.emplace_back(5);

  ASSERT_EQ(vec1.size(), 2u);
  ASSERT_EQ(vec2.size(), 3u);

  Counter::Reset();
  const size_t initial_bytes = allocator_for_test_.GetAllocated();

  vec1 = std::move(vec2);

  EXPECT_EQ(vec1.size(), 3u);
  EXPECT_EQ(Counter::destroyed, 2);
  EXPECT_LT(allocator_for_test_.GetAllocated(), initial_bytes);
}

TEST_F(DynamicPtrVectorTest, TryEmplaceBack) {
  pw::DynamicPtrVector<Counter> vec(allocator_);

  EXPECT_TRUE(vec.try_emplace_back(1));
  EXPECT_EQ(vec.size(), 1u);

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_emplace_back(2));
  EXPECT_EQ(vec.size(), 1u);
}

TEST_F(DynamicPtrVectorTest, TryEmplaceBackFailsToReservePointer) {
  pw::DynamicPtrVector<Counter> vec(allocator_);

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_emplace_back(100));
}

TEST_F(DynamicPtrVectorTest, TryEmplaceBackFailsToAllocateObject) {
  struct BigObject {
    int big_array[65536];
  };
  pw::DynamicPtrVector<BigObject> vec(allocator_);

  EXPECT_FALSE(vec.try_emplace_back());
}

TEST_F(DynamicPtrVectorTest, Emplace) {
  pw::DynamicPtrVector<Counter> vec(allocator_);

  vec.emplace(vec.begin(), 100);
  EXPECT_TRUE(vec.try_emplace(vec.begin(), 200));

  EXPECT_EQ(vec[0].value, 200);
  EXPECT_EQ(vec[1].value, 100);
}

TEST_F(DynamicPtrVectorTest, TryInsert) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(3);

  auto it = vec.try_insert(vec.begin() + 1, Counter(2));
  ASSERT_TRUE(it.has_value());
  EXPECT_EQ((*it)->value, 2);
  EXPECT_EQ(vec[0].value, 1);
  EXPECT_EQ(vec[1].value, 2);
  EXPECT_EQ(vec[2].value, 3);

  allocator_.DisableAll();
  EXPECT_FALSE(vec.try_insert(vec.begin(), Counter(0)));
}

TEST_F(DynamicPtrVectorTest, ReservePtrAndPtrCapacity) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  EXPECT_EQ(vec.ptr_capacity(), 0u);

  vec.reserve_ptr(10);
  EXPECT_GE(vec.ptr_capacity(), 10u);

  vec.push_back(Counter(1));
  EXPECT_GE(vec.ptr_capacity(), 10u);

  vec.shrink_to_fit();
  EXPECT_EQ(vec.ptr_capacity(), vec.size());
}

TEST_F(DynamicPtrVectorTest, EmplaceDerived_TriviallyDestructible) {
  struct Base {
    int base_member = -1;
  };

  struct Derived : public Base {
    float value = 1.5f;
  };

  pw::DynamicPtrVector<Base> vec(allocator_);
  vec.emplace_back<Derived>();
  EXPECT_EQ(vec.back().base_member, -1);
  EXPECT_EQ(static_cast<Derived&>(vec.back()).value, 1.5f);
}

TEST_F(DynamicPtrVectorTest, EmplaceDerived_VirtualDestructor) {
  struct Base {
    Base() : base_member(1) {}

    virtual ~Base() = default;

    Counter base_member;
  };

  struct Derived1 : public Base {
    Derived1(int value) : member(value) {}

    Counter member;
  };

  struct Derived2 : public Base {
    int member = 3;
  };

  pw::DynamicPtrVector<Base> vec(allocator_);
  vec.emplace_back<Base>();
  vec.emplace_back<Derived1>(2);
  vec.emplace_back<Derived2>();

  EXPECT_EQ(vec[0].base_member.value, 1);
  EXPECT_EQ(vec[1].base_member.value, 1);
  EXPECT_EQ(static_cast<Derived1&>(vec[1]).member.value, 2);
  EXPECT_EQ(static_cast<Derived2&>(vec[2]).member, 3);
}

TEST_F(DynamicPtrVectorTest, EmplaceDerived_NonVirtualNonTrivialDestructor) {
  // Base is trivially destructible, but non-virtual.
  struct Base {
    int base_member = 123;
  };

  // Derived is not trivially destructible. It is virtually destructible, but
  // the Base is not.
  struct Derived : public Base {
    virtual ~Derived() = default;

    Counter counter;
  };

  pw::DynamicPtrVector<Base> vec(allocator_);
  vec.emplace_back<Base>();
  EXPECT_EQ(vec[0].base_member, 123);

#if PW_NC_TEST(EmplaceDerived_NonVirtualNonTrivial)
  PW_NC_EXPECT("virtually or trivially destructible");
  vec.emplace_back<Derived>(2);
#elif PW_NC_TEST(EmplaceDerived_NonDerived)
  PW_NC_EXPECT("must inherit from T");
  vec.emplace_back<int>(3);
#endif  // PW_NC_TEST
}

TEST_F(DynamicPtrVectorTest, Take) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);

  // Take the middle element (2)
  pw::UniquePtr<Counter> taken = vec.take(vec.begin() + 1);

  EXPECT_EQ(taken->value, 2);
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0].value, 1);
  EXPECT_EQ(vec[1].value, 3);

  // Verify ownership transfer
  size_t allocated_before_reset = allocator_for_test_.GetAllocated();
  taken.Reset();
  size_t allocated_after_reset = allocator_for_test_.GetAllocated();
  EXPECT_LT(allocated_after_reset, allocated_before_reset);
}

TEST_F(DynamicPtrVectorTest, PushBackUniquePtr_SameAllocator) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  auto ptr = allocator_.MakeUnique<Counter>(100);
  Counter* const raw_ptr = ptr.get();

  vec.push_back(std::move(ptr));
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0].value, 100);

  EXPECT_EQ(&vec[0], raw_ptr);
}

TEST_F(DynamicPtrVectorTest, PushBackUniquePtr_DifferentAllocator) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  pw::allocator::test::AllocatorForTest<256> other_allocator;
  auto ptr = other_allocator.MakeUnique<Counter>(200);
  Counter* const raw_ptr = ptr.get();

  vec.push_back(std::move(ptr));
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0].value, 200);

  EXPECT_NE(&vec[0], raw_ptr);
}

TEST_F(DynamicPtrVectorTest, InsertUniquePtr_SameAllocator) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(3);

  auto ptr = allocator_.MakeUnique<Counter>(2);
  Counter* const raw_ptr = ptr.get();

  auto it = vec.insert(vec.begin() + 1, std::move(ptr));
  EXPECT_EQ(it->value, 2);
  EXPECT_EQ(vec[0].value, 1);
  EXPECT_EQ(vec[1].value, 2);
  EXPECT_EQ(vec[2].value, 3);

  EXPECT_EQ(&vec[1], raw_ptr);
}

TEST_F(DynamicPtrVectorTest, InsertUniquePtr_DifferentAllocator) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(3);

  pw::allocator::test::AllocatorForTest<256> other_allocator;
  auto ptr = other_allocator.MakeUnique<Counter>(2);
  Counter* const raw_ptr = ptr.get();

  auto it = vec.insert(vec.begin() + 1, std::move(ptr));
  EXPECT_EQ(it->value, 2);
  EXPECT_EQ(vec[0].value, 1);
  EXPECT_EQ(vec[1].value, 2);
  EXPECT_EQ(vec[2].value, 3);

  EXPECT_NE(&vec[1], raw_ptr);
}

TEST_F(DynamicPtrVectorTest, PushBackUniquePtr_Derived) {
  struct Base {
    virtual ~Base() = default;
    int val = 0;
  };
  struct Derived : Base {
    Derived() { val = 1; }
  };

  pw::DynamicPtrVector<Base> vec(allocator_);
  auto ptr = allocator_.MakeUnique<Derived>();

  vec.push_back(std::move(ptr));
  EXPECT_EQ(vec[0].val, 1);
}

TEST_F(DynamicPtrVectorTest, SwapVectors) {
  pw::DynamicPtrVector<Counter> vec1(allocator_);
  vec1.emplace_back(1);
  vec1.emplace_back(2);

  pw::DynamicPtrVector<Counter> vec2(allocator_);
  vec2.emplace_back(3);
  vec2.emplace_back(4);
  vec2.emplace_back(5);

  vec1.swap(vec2);
  EXPECT_EQ(vec1.size(), 3u);
  EXPECT_EQ(vec1[0].value, 3);
  EXPECT_EQ(vec1[1].value, 4);
  EXPECT_EQ(vec1[2].value, 5);

  EXPECT_EQ(vec2.size(), 2u);
  EXPECT_EQ(vec2[0].value, 1);
  EXPECT_EQ(vec2[1].value, 2);
}

TEST_F(DynamicPtrVectorTest, SwapIterators) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);

  vec.swap(vec.begin(), vec.begin() + 2);
  EXPECT_EQ(vec[0].value, 3);
  EXPECT_EQ(vec[2].value, 1);

  vec.swap(vec.begin() + 1, vec.begin());
  EXPECT_EQ(vec[0].value, 2);
  EXPECT_EQ(vec[1].value, 3);
  EXPECT_EQ(vec[2].value, 1);
}

TEST_F(DynamicPtrVectorTest, SwapIndices) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);

  vec.swap(0, 2);
  EXPECT_EQ(vec[0].value, 3);
  EXPECT_EQ(vec[1].value, 2);
  EXPECT_EQ(vec[2].value, 1);

  vec.swap(1, 0);
  EXPECT_EQ(vec[0].value, 2);
  EXPECT_EQ(vec[1].value, 3);
  EXPECT_EQ(vec[2].value, 1);
}

TEST_F(DynamicPtrVectorTest, SwapWithBackIterator) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);

  vec.swap(vec.begin(), vec.end() - 1);
  EXPECT_EQ(vec[0].value, 3);
  EXPECT_EQ(vec[1].value, 2);
  EXPECT_EQ(vec[2].value, 1);
}

TEST_F(DynamicPtrVectorTest, SwapWithBackIndex) {
  pw::DynamicPtrVector<Counter> vec(allocator_);
  vec.emplace_back(1);
  vec.emplace_back(2);
  vec.emplace_back(3);

  vec.swap(1, vec.size() - 1);
  EXPECT_EQ(vec[0].value, 1);
  EXPECT_EQ(vec[1].value, 3);
  EXPECT_EQ(vec[2].value, 2);
}

}  // namespace
