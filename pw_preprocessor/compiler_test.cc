// Copyright 2021 The Pigweed Authors
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

#include "pw_preprocessor/compiler.h"

#include <cstdint>

#include "pw_unit_test/framework.h"

namespace pw::preprocessor {
namespace {

PW_MODIFY_DIAGNOSTICS_PUSH();
PW_MODIFY_DIAGNOSTIC(ignored, "-Wunused-variable");

int this_variable_is_unused;

PW_MODIFY_DIAGNOSTICS_POP();

class Foo {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wunused");

  int this_field_is_unused;

  PW_MODIFY_DIAGNOSTICS_POP();
};

TEST(CompilerMacros, ModifyDiagnostics) {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wunused-variable");

  int this_variable_also_is_unused;

  PW_MODIFY_DIAGNOSTICS_POP();
}

TEST(CompilerMacros, ModifyDiagnosticsClangGcc) {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC_GCC(ignored, "-Wunused-variable");
  PW_MODIFY_DIAGNOSTIC_CLANG(ignored, "-Wunused-variable");

  int this_variable_also_is_unused;

  PW_MODIFY_DIAGNOSTICS_POP();
}

template <typename T, typename U = int>
class GenericTemplate;

template <typename T>
using IntTemplate = GenericTemplate<T, int>;

template <typename T, typename U>
class PW_ATTRIBUTE_PREFERRED_NAME(IntTemplate<T>) GenericTemplate {
 public:
  constexpr explicit GenericTemplate(T value) : value_(value) {}
  constexpr T value() const { return value_; }

 private:
  T value_;
};

TEST(CompilerMacros, PreferredName) {
  IntTemplate<int> instance(42);
  EXPECT_EQ(instance.value(), 42);
}

// Exercise PW_ALWAYS_INLINE macro expansion on constexpr and inline functions.
PW_ALWAYS_INLINE constexpr int AlwaysInlineAdd(int a, int b) { return a + b; }

PW_ALWAYS_INLINE inline int AlwaysInlineMul(int a, int b) { return a * b; }

TEST(CompilerMacros, AlwaysInline) {
  static_assert(AlwaysInlineAdd(2, 3) == 5);
  EXPECT_EQ(AlwaysInlineMul(3, 4), 12);
}

class PW_ATTRIBUTE_CORO_RETURN_TYPE PW_ATTRIBUTE_CORO_LIFETIME_BOUND
    FakeCoroReturnType {
 public:
  constexpr explicit FakeCoroReturnType(int value) : value_(value) {}
  constexpr int value() const { return value_; }

 private:
  int value_;
};

PW_ATTRIBUTE_CORO_WRAPPER constexpr FakeCoroReturnType MakeFakeCoro(
    const int& value) {
  return FakeCoroReturnType(value);
}

PW_ATTRIBUTE_CORO_WRAPPER
PW_ATTRIBUTE_CORO_DISABLE_LIFETIME_BOUND constexpr FakeCoroReturnType
MakeFakeCoroCopied(const int& value) {
  return FakeCoroReturnType(value);
}

TEST(CompilerMacros, CoroutineLifetimeAttributes) {
  constexpr int kLvalue = 7;
  FakeCoroReturnType wrapped = MakeFakeCoro(kLvalue);
  EXPECT_EQ(wrapped.value(), kLvalue);

  FakeCoroReturnType disabled = MakeFakeCoroCopied(/*value=*/11);
  EXPECT_EQ(disabled.value(), 11);
}
}  // namespace
}  // namespace pw::preprocessor
