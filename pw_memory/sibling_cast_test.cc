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

#include "pw_memory/internal/sibling_cast.h"

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace pw::internal {
namespace {

class Base {
 public:
  constexpr Base(char id) : lowercase_id_{id} {}

 protected:
  char lowercase_id_;
};

class DerivedA : public Base {
 public:
  constexpr DerivedA() : Base('a') {}

  // Lowercase version of the ID.
  char Id() const { return lowercase_id_; }
};

class DerivedB : public Base {
 public:
  constexpr DerivedB() : Base('b') {}

  // Capitalized version of the ID.
  char Id() const { return lowercase_id_ - ('a' - 'A'); }
};

constexpr DerivedA kInstanceA;
constexpr DerivedB kInstanceB;

TEST(SiblingCast, Reference) {
  DerivedA instance_a;
  DerivedB instance_b;

  EXPECT_EQ(instance_a.Id(), 'a');
  DerivedB& b_ref = SiblingCast<DerivedB&, Base>(instance_a);
  EXPECT_EQ(b_ref.Id(), 'A');

  EXPECT_EQ(instance_b.Id(), 'B');
  DerivedA& a_ref = SiblingCast<DerivedA&, Base>(instance_b);
  EXPECT_EQ(a_ref.Id(), 'b');
}

TEST(SiblingCast, RvalueReference) {
  DerivedA instance_a;
  DerivedB instance_b;

  EXPECT_EQ(instance_a.Id(), 'a');
  DerivedB&& b_ref = SiblingCast<DerivedB&&, Base>(std::move(instance_a));
  EXPECT_EQ(b_ref.Id(), 'A');

  EXPECT_EQ(instance_b.Id(), 'B');
  DerivedA&& a_ref = SiblingCast<DerivedA&&, Base>(std::move(instance_b));
  EXPECT_EQ(a_ref.Id(), 'b');
}

TEST(SiblingCast, ConstReference) {
  EXPECT_EQ(kInstanceA.Id(), 'a');
  const DerivedB& b_ref = SiblingCast<const DerivedB&, Base>(kInstanceA);
  EXPECT_EQ(b_ref.Id(), 'A');

  EXPECT_EQ(kInstanceB.Id(), 'B');
  const DerivedA& a_ref = SiblingCast<const DerivedA&, Base>(kInstanceB);
  EXPECT_EQ(a_ref.Id(), 'b');
}

TEST(SiblingCast, ConstRvalueReference) {
  EXPECT_EQ(kInstanceA.Id(), 'a');
  const DerivedB&& b_ref =
      SiblingCast<const DerivedB&&, Base>(std::move(kInstanceA));
  EXPECT_EQ(b_ref.Id(), 'A');

  EXPECT_EQ(kInstanceB.Id(), 'B');
  const DerivedA&& a_ref =
      SiblingCast<const DerivedA&&, Base>(std::move(kInstanceB));
  EXPECT_EQ(a_ref.Id(), 'b');
}

TEST(SiblingCast, NonConstToConstReference) {
  DerivedA instance_a;
  DerivedB instance_b;

  EXPECT_EQ(instance_a.Id(), 'a');
  const DerivedB& b_ref = SiblingCast<const DerivedB&, Base>(instance_a);
  EXPECT_EQ(b_ref.Id(), 'A');

  EXPECT_EQ(instance_b.Id(), 'B');
  const DerivedA& a_ref = SiblingCast<const DerivedA&, Base>(instance_b);
  EXPECT_EQ(a_ref.Id(), 'b');
}

TEST(SiblingCast, Pointer) {
  DerivedA instance_a;
  DerivedB instance_b;

  EXPECT_EQ(instance_a.Id(), 'a');
  DerivedB* b_ptr = SiblingCast<DerivedB*, Base>(&instance_a);
  EXPECT_EQ(b_ptr->Id(), 'A');

  EXPECT_EQ(instance_b.Id(), 'B');
  DerivedA* a_ptr = SiblingCast<DerivedA*, Base>(&instance_b);
  EXPECT_EQ(a_ptr->Id(), 'b');
}

TEST(SiblingCast, ConstPointer) {
  EXPECT_EQ(kInstanceA.Id(), 'a');
  const DerivedB* b_ptr = SiblingCast<const DerivedB*, Base>(&kInstanceA);
  EXPECT_EQ(b_ptr->Id(), 'A');

  EXPECT_EQ(kInstanceB.Id(), 'B');
  const DerivedA* a_ptr = SiblingCast<const DerivedA*, Base>(&kInstanceB);
  EXPECT_EQ(a_ptr->Id(), 'b');
}

TEST(SiblingCast, NonConstToConstPointer) {
  DerivedA instance_a;
  DerivedB instance_b;

  EXPECT_EQ(instance_a.Id(), 'a');
  const DerivedB* b_ptr = SiblingCast<const DerivedB*, Base>(&instance_a);
  EXPECT_EQ(b_ptr->Id(), 'A');

  EXPECT_EQ(instance_b.Id(), 'B');
  const DerivedA* a_ptr = SiblingCast<const DerivedA*, Base>(&instance_b);
  EXPECT_EQ(a_ptr->Id(), 'b');
}

class DerivedExtra : public Base {
 public:
  DerivedExtra() : Base('e') {}

  int member() const { return member_; }

 private:
  int member_ = 0;
};

class DerivedMultiple : public DerivedA, DerivedB {
 public:
  DerivedMultiple() {}
};

TEST(SiblingCast, NegativeCompilationTests) {
  DerivedMultiple multiple;
  DerivedExtra extra;
#if PW_NC_TEST(AmbiguousBase)
  PW_NC_EXPECT("unambiguously derive from the base");
  [[maybe_unused]] DerivedB& b_ref = SiblingCast<DerivedB&, Base>(multiple);
#elif PW_NC_TEST(SourceTypeCannotAddMembers)
  PW_NC_EXPECT("source type cannot add any members");
  [[maybe_unused]] DerivedB& b_ref = SiblingCast<DerivedB&, Base>(extra);
#elif PW_NC_TEST(DestinationTypeCannotAddMembers)
  PW_NC_EXPECT("destination type cannot add any members");
  [[maybe_unused]] auto& e = SiblingCast<const DerivedExtra&, Base>(kInstanceA);
#endif  // PW_NC_TEST
}

}  // namespace
}  // namespace pw::internal
