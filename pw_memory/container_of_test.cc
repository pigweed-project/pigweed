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

#include "pw_memory/container_of.h"

#include <cstdint>

#include "pw_unit_test/framework.h"

namespace {

struct TestStruct {
  uint32_t a;
  uint8_t b;
};

TEST(ContainerOf, OffsetOf) {
  EXPECT_EQ(pw::OffsetOf(&TestStruct::a), 0u);
  EXPECT_EQ(pw::OffsetOf(&TestStruct::b), sizeof(uint32_t));
}

TEST(ContainerOf, ContainerOf) {
  TestStruct s;
  EXPECT_EQ(pw::ContainerOf(&s.a, &TestStruct::a), &s);
  EXPECT_EQ(pw::ContainerOf(&s.b, &TestStruct::b), &s);
}

TEST(ContainerOf, ContainerOfConst) {
  const TestStruct s{};
  EXPECT_EQ(pw::ContainerOf(&s.a, &TestStruct::a), &s);
  EXPECT_EQ(pw::ContainerOf(&s.b, &TestStruct::b), &s);
}

TEST(ContainerOf, ContainerOfTemplateAuto) {
  TestStruct s;
  EXPECT_EQ(pw::ContainerOf<&TestStruct::a>(&s.a), &s);
  EXPECT_EQ(pw::ContainerOf<&TestStruct::b>(&s.b), &s);
}

TEST(ContainerOf, Inheritance) {
  struct Base {
    int x;
  };

  struct Derived : Base {
    int y;
    int z;
  };

  struct Other {
    int a;
    Derived b;
  };

  Other other;
  EXPECT_EQ(pw::ContainerOf<&Other::a>(&other.a), &other);

  EXPECT_EQ(pw::ContainerOf<&Other::b>(&other.b), &other);
  EXPECT_EQ((pw::ContainerOf<&Other::b, Base>(&other.b)), &other);
  EXPECT_EQ((pw::ContainerOf<&Other::b, Derived>(&other.b)), &other);
}

struct VirtualStruct {
  virtual ~VirtualStruct() = default;

  uint32_t a;
  uint32_t b;

  virtual void Foo() {}
};

TEST(ContainerOf, VirtualStruct) {
  VirtualStruct s;
  EXPECT_EQ(pw::ContainerOf(&s.a, &VirtualStruct::a), &s);
  EXPECT_EQ(pw::ContainerOf(&s.b, &VirtualStruct::b), &s);
}

TEST(ContainerOf, Reference) {
  TestStruct s;
  EXPECT_EQ(&pw::ContainerOf(s.a, &TestStruct::a), &s);
  EXPECT_EQ(&pw::ContainerOf(s.b, &TestStruct::b), &s);
}

TEST(ContainerOf, ReferenceConst) {
  const TestStruct s{};
  EXPECT_EQ(&pw::ContainerOf(s.a, &TestStruct::a), &s);
  EXPECT_EQ(&pw::ContainerOf(s.b, &TestStruct::b), &s);
}

TEST(ContainerOf, ReferenceTemplateAuto) {
  TestStruct s;
  EXPECT_EQ(&pw::ContainerOf<&TestStruct::a>(s.a), &s);
  EXPECT_EQ(&pw::ContainerOf<&TestStruct::b>(s.b), &s);

  const TestStruct const_s{};
  EXPECT_EQ(&pw::ContainerOf<&TestStruct::a>(const_s.a), &const_s);
  EXPECT_EQ(&pw::ContainerOf<&TestStruct::b>(const_s.b), &const_s);
}

TEST(ContainerOf, NullptrAsserts) {
  uint8_t* null_b = nullptr;
  ASSERT_DEATH_IF_SUPPORTED((void)pw::ContainerOf(null_b, &TestStruct::b), "");
  ASSERT_DEATH_IF_SUPPORTED((void)pw::ContainerOf<&TestStruct::b>(null_b), "");
}

}  // namespace
