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

#include "pw_containers/functional.h"

#include <string>

#include "pw_unit_test/framework.h"

namespace {

TEST(Hash, BasicTypes) {
  pw::Hash hasher;

  EXPECT_NE(hasher(1), hasher(2));
  EXPECT_NE(hasher(1.0), hasher(2.0));
  EXPECT_NE(hasher('a'), hasher('b'));
  EXPECT_NE(hasher(true), hasher(false));

  EXPECT_EQ(hasher(1), hasher(1));
  EXPECT_EQ(hasher(1.0), hasher(1.0));
  EXPECT_EQ(hasher('a'), hasher('a'));
  EXPECT_EQ(hasher(true), hasher(true));
}

TEST(Hash, String) {
  pw::Hash hasher;
  const char* str1 = "hello";
  const char* str2 = "hello";
  const char* str3 = "world";

  EXPECT_EQ(hasher(std::string(str1)), hasher(std::string(str2)));
  EXPECT_NE(hasher(std::string(str1)), hasher(std::string(str3)));
}

TEST(Hash, Pointers) {
  pw::Hash hasher;
  int x = 5;
  int y = 5;
  int* p1 = &x;
  int* p2 = &x;
  int* p3 = &y;
  int* null_ptr = nullptr;

  EXPECT_EQ(hasher(p1), hasher(p2));
  EXPECT_NE(hasher(p1), hasher(p3));
  EXPECT_EQ(hasher(null_ptr), hasher(nullptr));
  EXPECT_NE(hasher(p1), hasher(null_ptr));
}

TEST(Hash, DifferentSeeds) {
  pw::Hash hasher1(17);
  pw::Hash hasher2(42);

  EXPECT_NE(hasher1(123), hasher2(123));
  EXPECT_NE(hasher1(std::string("hello")), hasher2(std::string("hello")));
}

struct CustomTypeA {
  int x;
  double y;

  template <typename H>
  friend H PwHashValue(H h, const CustomTypeA& t) {
    return H::combine(std::move(h), t.x, t.y);
  }
};

TEST(Hash, CustomTypePwHashValue) {
  pw::Hash hasher;
  CustomTypeA p1{1, 2.0};
  CustomTypeA p2{1, 2.0};
  CustomTypeA p3{2, 1.0};

  EXPECT_EQ(hasher(p1), hasher(p2));
  EXPECT_NE(hasher(p1), hasher(p3));
}

struct CustomTypeB {
  int x;
  double y;
};

}  // namespace

namespace std {
template <>
struct hash<CustomTypeB> {
  size_t operator()(const CustomTypeB& t) const {
    pw::Hash hasher;
    return hasher(t.x) ^ hasher(t.y);
  }
};
}  // namespace std

namespace {

TEST(Hash, CustomTypeStdHash) {
  pw::Hash hasher;
  CustomTypeB p1{1, 2};
  CustomTypeB p2{1, 2};
  CustomTypeB p3{2, 1};

  EXPECT_EQ(hasher(p1), hasher(p2));
  EXPECT_NE(hasher(p1), hasher(p3));
}

TEST(Hash, Combine) {
  pw::HashState h1 = pw::HashState();
  h1 = pw::HashState::combine(std::move(h1), 1);
  h1 = pw::HashState::combine(std::move(h1), 2);
  h1 = pw::HashState::combine(std::move(h1), 3);
  size_t hash1 = h1.finalize();

  pw::HashState h2 = pw::HashState();
  h2 = pw::HashState::combine(std::move(h2), 1);
  h2 = pw::HashState::combine(std::move(h2), 2);
  h2 = pw::HashState::combine(std::move(h2), 3);
  size_t hash2 = h2.finalize();

  pw::HashState h3 = pw::HashState();
  h3 = pw::HashState::combine(std::move(h3), 3);
  h3 = pw::HashState::combine(std::move(h3), 2);
  h3 = pw::HashState::combine(std::move(h3), 1);
  size_t hash3 = h3.finalize();

  EXPECT_EQ(hash1, hash2);
  EXPECT_NE(hash1, hash3);

  std::string s1 = "hello";
  std::string s2 = "world";

  pw::HashState h4 = pw::HashState();
  h4 = pw::HashState::combine(std::move(h4), s1);
  h4 = pw::HashState::combine(std::move(h4), 5);
  size_t hash4 = h4.finalize();

  pw::HashState h5 = pw::HashState();
  h5 = pw::HashState::combine(std::move(h5), s1);
  h5 = pw::HashState::combine(std::move(h5), 5);
  size_t hash5 = h5.finalize();

  pw::HashState h6 = pw::HashState();
  h6 = pw::HashState::combine(std::move(h6), s2);
  h6 = pw::HashState::combine(std::move(h6), 5);
  size_t hash6 = h6.finalize();

  pw::HashState h7 = pw::HashState();
  h7 = pw::HashState::combine(std::move(h7), s1);
  h7 = pw::HashState::combine(std::move(h7), 6);
  size_t hash7 = h7.finalize();

  EXPECT_EQ(hash4, hash5);
  EXPECT_NE(hash4, hash6);
  EXPECT_NE(hash4, hash7);
}

enum class MyEnum { kValue1, kValue2 };

TEST(Hash, Enum) {
  pw::Hash hasher;

  EXPECT_EQ(hasher(MyEnum::kValue1), hasher(MyEnum::kValue1));
  EXPECT_NE(hasher(MyEnum::kValue1), hasher(MyEnum::kValue2));
}

TEST(EqualTo, BasicTypes) {
  pw::EqualTo equal_to;

  EXPECT_TRUE(equal_to(1, 1));
  EXPECT_FALSE(equal_to(1, 2));
  EXPECT_TRUE(equal_to(1.0, 1.0));
  EXPECT_FALSE(equal_to(1.0, 2.0));
  EXPECT_TRUE(equal_to('a', 'a'));
  EXPECT_FALSE(equal_to('a', 'b'));
  EXPECT_TRUE(equal_to(true, true));
  EXPECT_FALSE(equal_to(true, false));
  const char* str1 = "hello";
  const char* str2 = "hello";
  const char* str3 = "world";
  EXPECT_TRUE(equal_to(std::string(str1), std::string(str2)));
  EXPECT_FALSE(equal_to(std::string(str1), std::string(str3)));
}

TEST(EqualTo, MixedTypes) {
  pw::EqualTo equal_to;

  EXPECT_TRUE(equal_to(1, 1L));
  EXPECT_TRUE(equal_to(1, 1.0));
  EXPECT_FALSE(equal_to(1, 2.0));
  EXPECT_TRUE(equal_to(0, false));
  EXPECT_TRUE(equal_to(1, true));
  EXPECT_FALSE(equal_to(2, true));
}

TEST(EqualTo, Pointers) {
  pw::EqualTo equal_to;
  int x = 5;
  int y = 5;
  int* p1 = &x;
  int* p2 = &x;
  int* p3 = &y;
  int* null_ptr = nullptr;

  EXPECT_TRUE(equal_to(p1, p2));
  EXPECT_FALSE(equal_to(p1, p3));
  EXPECT_TRUE(equal_to(null_ptr, nullptr));
  EXPECT_FALSE(equal_to(p1, null_ptr));
}

template <typename T>
struct Comparable {
  T value;
  bool operator==(const Comparable<T>& other) const {
    return value == other.value;
  }
};

template <typename T>
bool operator==(const Comparable<T>& lhs, const T& rhs) {
  return lhs.value == rhs;
}

template <typename T>
bool operator==(const T& lhs, const Comparable<T>& rhs) {
  return lhs == rhs.value;
}

TEST(EqualTo, UserDefinedTypes) {
  pw::EqualTo equal_to;

  Comparable<int> c1{1};
  Comparable<int> c2{1};
  Comparable<int> c3{2};

  EXPECT_TRUE(equal_to(c1, c2));
  EXPECT_FALSE(equal_to(c1, c3));
  EXPECT_TRUE(equal_to(c1, 1));
  EXPECT_FALSE(equal_to(c1, 2));
  EXPECT_TRUE(equal_to(1, c1));
  EXPECT_FALSE(equal_to(2, c1));

  Comparable<double> cd1{1.0};
  Comparable<double> cd2{1.0};
  Comparable<double> cd3{2.0};

  EXPECT_TRUE(equal_to(cd1, cd2));
  EXPECT_FALSE(equal_to(cd1, cd3));
  EXPECT_TRUE(equal_to(cd1, 1.0));
  EXPECT_FALSE(equal_to(cd1, 2.0));
  EXPECT_TRUE(equal_to(1.0, cd1));
  EXPECT_FALSE(equal_to(2.0, cd1));

  Comparable<std::string> cs1{"hello"};
  Comparable<std::string> cs2{"hello"};
  Comparable<std::string> cs3{"world"};

  EXPECT_TRUE(equal_to(cs1, cs2));
  EXPECT_FALSE(equal_to(cs1, cs3));
  EXPECT_TRUE(equal_to(cs1, std::string("hello")));
  EXPECT_FALSE(equal_to(cs1, std::string("world")));
  EXPECT_TRUE(equal_to(std::string("hello"), cs1));
  EXPECT_FALSE(equal_to(std::string("world"), cs1));
}

}  // namespace
