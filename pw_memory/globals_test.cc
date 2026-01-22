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

#include "pw_memory/globals.h"

#include <cstdint>

#include "pw_assert/check.h"
#include "pw_polyfill/standard.h"
#include "pw_unit_test/framework.h"

namespace {

#if PW_CXX_STANDARD_IS_SUPPORTED(20)

// Constant initialization example for the docs.
// DOCSTAG[pw_memory-globals-init]
// This function initializes an array to non-zero values.
constexpr std::array<uint8_t, 4096> InitializedArray() {
  std::array<uint8_t, 4096> data{};
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i);
  }
  return data;
}

// This array constant initialized, which increases the binary size by 4KB.
constinit std::array<uint8_t, 4096> constant_initialized = InitializedArray();

// This array is statically initialized and takes no space in the binary, but
// the InitializedArray() function is included in the binary.
pw::RuntimeInitGlobal<std::array<uint8_t, 4096>> runtime_initialized(
    InitializedArray());

// This array is zero-initialized and takes no space in the binary. It must be
// manually initialized.
std::array<uint8_t, 4096> zero_initialized;
// DOCSTAG[pw_memory-globals-init]

TEST(RuntimeInitGlobal, BigArrayExample) {
  EXPECT_EQ(constant_initialized[255], 255u);
  EXPECT_EQ((*runtime_initialized)[255], 255u);
  EXPECT_EQ(zero_initialized[255], 0u);
}

#endif  // PW_CXX_STANDARD_IS_SUPPORTED(20)

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

  ~ConstexprConstructible() { PW_CHECK(!crash); }

  bool crash;
};

TEST(RuntimeInitGlobal, ShouldNotCallDestructor) {
  bool destructor_called = false;

  {
    HasADestructor should_be_destroyed(destructor_called);
  }

  EXPECT_TRUE(destructor_called);

  {
    pw::RuntimeInitGlobal<HasADestructor> should_not_be_destroyed(
        destructor_called);
  }

  EXPECT_FALSE(destructor_called);
}

TEST(RuntimeInitGlobal, MemberAccess) {
  pw::RuntimeInitGlobal<CrashInDestructor> no_destructor;

  no_destructor->some_value = 123;
  EXPECT_EQ(123, (*no_destructor).some_value);
  EXPECT_EQ(no_destructor.operator->(), no_destructor->MyAddress());
}

TEST(RuntimeInitGlobal, TrivialDestructor) {
  pw::RuntimeInitGlobal<TrivialDestructor> no_destructor(555);

  EXPECT_EQ(no_destructor->value, 555);
  no_destructor->value = 123;
  EXPECT_EQ(no_destructor->value, 123);
}

TEST(RuntimeInitGlobal, TrivialType) {
  pw::RuntimeInitGlobal<int> no_destructor;

  EXPECT_EQ(*no_destructor, 0);
  *no_destructor = 123;
  EXPECT_EQ(*no_destructor, 123);
}

TEST(RuntimeInitGlobal, FunctionStatic) {
  static pw::RuntimeInitGlobal<CrashInDestructor> function_static_no_destructor;
}

pw::RuntimeInitGlobal<CrashInDestructor> global_no_destructor;

static_assert(!std::is_trivially_destructible<CrashInDestructor>::value,
              "Type should not be trivially destructible");
static_assert(std::is_trivially_destructible<
                  pw::RuntimeInitGlobal<CrashInDestructor>>::value,
              "Wrapper should be trivially destructible");

}  // namespace
