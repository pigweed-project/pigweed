// Copyright 2022 The Pigweed Authors
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

#include "pw_memory/no_destructor.h"

#include "pw_assert/check.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_polyfill/standard.h"
#include "pw_unit_test/framework.h"

namespace pw {
namespace {

class HasADestructor {
 public:
  HasADestructor(bool& destructor_called_flag)
      : destructor_called_(destructor_called_flag) {
    destructor_called_ = false;
  }

  ~HasADestructor() { destructor_called_ = true; }

 private:
  bool& destructor_called_;
};

class CrashInDestructor {
 public:
  const CrashInDestructor* MyAddress() const { return this; }

  int some_value = 0;

 private:
  // Even though the destructor is never called, classes with private
  // destructors must friend `pw::NoDestructor`.
  friend class pw::NoDestructor<CrashInDestructor>;

  ~CrashInDestructor() { PW_CRASH("This destructor should never execute!"); }
};

class TrivialDestructor {
 public:
  TrivialDestructor(int initial_value) : value(initial_value) {}

  int value;
};

class ConstexprConstructible {
 public:
  constexpr ConstexprConstructible() : crash(true) {}

  PW_CONSTEXPR_CPP20 ~ConstexprConstructible() { PW_CHECK(!crash); }

  bool crash;
};

TEST(NoDestructor, ShouldNotCallDestructor) {
  bool destructor_called = false;

  {
    HasADestructor should_be_destroyed(destructor_called);
  }

  EXPECT_TRUE(destructor_called);

  {
    NoDestructor<HasADestructor> should_not_be_destroyed(destructor_called);
  }

  EXPECT_FALSE(destructor_called);
}

TEST(NoDestructor, MemberAccess) {
  NoDestructor<CrashInDestructor> no_destructor;

  no_destructor->some_value = 123;
  EXPECT_EQ(123, (*no_destructor).some_value);
  EXPECT_EQ(no_destructor.operator->(), no_destructor->MyAddress());
}

TEST(NoDestructor, TrivialDestructor) {
  NoDestructor<TrivialDestructor> no_destructor(555);

  EXPECT_EQ(no_destructor->value, 555);
  no_destructor->value = 123;
  EXPECT_EQ(no_destructor->value, 123);
}

TEST(NoDestructor, TrivialType) {
  NoDestructor<int> no_destructor;

  EXPECT_EQ(*no_destructor, 0);
  *no_destructor = 123;
  EXPECT_EQ(*no_destructor, 123);
}

TEST(NoDestructor, FunctionStatic) {
  static NoDestructor<CrashInDestructor> function_static_no_destructor;
}

TEST(NoDestructor, Constinit) {
  PW_CONSTINIT static NoDestructor<ConstexprConstructible> should_crash;
  EXPECT_TRUE(should_crash->crash);
}

NoDestructor<CrashInDestructor> global_no_destructor;
PW_CONSTINIT NoDestructor<ConstexprConstructible> global_constinit;

TEST(NoDestructor, Globals) {
  EXPECT_EQ(global_no_destructor->some_value, 0);
  EXPECT_TRUE(global_constinit->crash);
}

static_assert(std::is_trivially_destructible<pw::NoDestructor<int>>::value,
              "Wrapper should be trivially destructible");

constexpr NoDestructor<int> kConstexprTrivialNoDestructor(1138);
static_assert(*kConstexprTrivialNoDestructor == 1138);

#if PW_CXX_STANDARD_IS_SUPPORTED(20)

constexpr NoDestructor<ConstexprConstructible> kConstexprNoDestructor;
static_assert(kConstexprNoDestructor->crash);

#endif  // PW_CXX_STANDARD_IS_SUPPORTED(20)

}  // namespace
}  // namespace pw
