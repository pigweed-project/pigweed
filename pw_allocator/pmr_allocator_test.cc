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

#include "pw_allocator/pmr_allocator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <forward_list>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::PmrAllocator;
using ::pw::allocator::test::AllocatorForTest;

struct Foo {
  uintptr_t value;

  Foo(uintptr_t value_) : value(value_) {}
};

bool operator==(const Foo& lhs, const Foo& rhs) {
  return lhs.value == rhs.value;
}

bool operator<(const Foo& lhs, const Foo& rhs) { return lhs.value < rhs.value; }

}  // namespace

template <>
struct std::hash<Foo> {
  size_t operator()(const Foo& foo) const {
    return std::hash<uintptr_t>()(foo.value);
  }
};

namespace {

struct Bar {
  std::array<std::byte, 16> buffer;

  Bar(int value) { std::memset(buffer.data(), value, buffer.size()); }
};

template <typename T, typename = void>
struct has_emplace_front : std::false_type {};

template <typename T>
struct has_emplace_front<
    T,
    std::void_t<decltype(std::declval<T>().emplace_front())> >
    : std::true_type {};

template <typename T, typename = void>
struct has_emplace_back : std::false_type {};

template <typename T>
struct has_emplace_back<
    T,
    std::void_t<decltype(std::declval<T>().emplace_back())> > : std::true_type {
};

template <typename T, typename = void>
struct has_key_type : std::false_type {};

template <typename T>
struct has_key_type<T, std::void_t<typename T::key_type> > : std::true_type {};

template <typename T, typename = void>
struct has_mapped_type : std::false_type {};

template <typename T>
struct has_mapped_type<T, std::void_t<typename T::mapped_type> >
    : std::true_type {};

template <typename Container, size_t kCapacity = 256>
void TestPmrAllocator() {
  AllocatorForTest<kCapacity> underlying;
  static_assert(sizeof(Foo) >= AllocatorForTest<kCapacity>::kMinSize);
  auto& requested_bytes = underlying.metrics().requested_bytes;
  EXPECT_EQ(requested_bytes.value(), 0U);

  PmrAllocator allocator(underlying);
  {
    Container container(allocator);
    size_t size = 0;
    // Sequence containers.
    if constexpr (has_emplace_front<Container>::value) {
      container.emplace_front(1);
      container.emplace_front(2);
      size += sizeof(typename Container::value_type) * 2;
    }
    if constexpr (has_emplace_back<Container>::value) {
      container.emplace_back(3);
      container.emplace_back(4);
      size += sizeof(typename Container::value_type) * 2;
    }
    // Associative containers.
    if constexpr (has_mapped_type<Container>::value) {
      container.insert({1, 10});
      container.insert({2, 20});
      size += sizeof(typename Container::key_type) * 2;
      size += sizeof(typename Container::mapped_type) * 2;
    } else if constexpr (has_key_type<Container>::value) {
      container.insert(3);
      container.insert(4);
      size += sizeof(typename Container::key_type) * 2;
    }
    EXPECT_GE(requested_bytes.value(), size);
  }
  EXPECT_EQ(requested_bytes.value(), 0U);
}

// Unit tests.

TEST(PmrAllocatorTest, Vector) { TestPmrAllocator<pw::pmr::vector<Foo> >(); }

TEST(PmrAllocatorTest, Deque) {
  // Some implementations preallocate a lot of memory.
  TestPmrAllocator<pw::pmr::deque<Foo>, 8192>();
}

TEST(PmrAllocatorTest, ForwardList) {
  TestPmrAllocator<pw::pmr::forward_list<Foo> >();
}

TEST(PmrAllocatorTest, List) { TestPmrAllocator<pw::pmr::list<Foo> >(); }

TEST(PmrAllocatorTest, Set) { TestPmrAllocator<pw::pmr::set<Foo> >(); }

TEST(PmrAllocatorTest, Map) { TestPmrAllocator<pw::pmr::map<Foo, Bar> >(); }

TEST(PmrAllocatorTest, MultiSet) {
  TestPmrAllocator<pw::pmr::multiset<Foo> >();
}

TEST(PmrAllocatorTest, MultiMap) {
  TestPmrAllocator<pw::pmr::multimap<Foo, Bar> >();
}

TEST(PmrAllocatorTest, UnorderedSet) {
  // Some implementations preallocate a lot of memory.
  TestPmrAllocator<pw::pmr::unordered_set<Foo>, 1024>();
}

TEST(PmrAllocatorTest, UnorderedMap) {
  // Some implementations preallocate a lot of memory.
  TestPmrAllocator<pw::pmr::unordered_map<Foo, Bar>, 1024>();
}

TEST(PmrAllocatorTest, UnorderedMultiSet) {
  // Some implementations preallocate a lot of memory.
  TestPmrAllocator<pw::pmr::unordered_multiset<Foo>, 1024>();
}

TEST(PmrAllocatorTest, UnorderedMultiMap) {
  // Some implementations preallocate a lot of memory.
  TestPmrAllocator<pw::pmr::unordered_multimap<Foo, Bar>, 1024>();
}

}  // namespace
