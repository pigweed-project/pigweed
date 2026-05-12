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

#include "pw_function/function_ref.h"

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace pw {
namespace {

// We rely on a clang-specific attribute to catch this lifetime error
#ifdef __clang__
#if PW_NC_TEST(DanglingLambdaDoesNotLiveLongEnough)
PW_NC_EXPECT_CLANG("temporary.*destroyed.*Wdangling");
[[maybe_unused]] void TriggerDanglingLambda() {
  [[maybe_unused]] FunctionRef<void()> ref = [] {};
}
#endif  // PW_NC_TEST
#endif  // __clang__

int Multiply(int a, int b) { return a * b; }

TEST(FunctionRefTest, BasicLambda) {
  auto lambda = [](int a, int b) { return a + b; };
  FunctionRef<int(int, int)> ref(lambda);
  EXPECT_EQ(ref(2, 3), 5);
}

TEST(FunctionRefTest, LambdaWithCapture) {
  int factor = 2;
  auto lambda = [&factor](int a) { return a * factor; };
  FunctionRef<int(int)> ref(lambda);
  EXPECT_EQ(ref(3), 6);
  factor = 3;
  EXPECT_EQ(ref(3), 9);
}

int SomeFunction(FunctionRef<int()> ref) { return ref() + ref(); }

// It is okay to create a FunctionRef from a temporary lambda as long as the
// FunctionRef does not outlive the lambda
TEST(FunctionRefTest, TemporaryLambda) {
  int a = 0;
  EXPECT_EQ(SomeFunction([&a] {
              a += 128;
              return 100;
            }),
            200);
  EXPECT_EQ(a, 256);
}

TEST(FunctionRefTest, FunctionPointer) {
  FunctionRef<int(int, int)> ref(Multiply);
  EXPECT_EQ(ref(2, 3), 6);
}

TEST(FunctionRefTest, ConstSignature) {
  auto lambda = [](int a) { return a * 2; };
  FunctionRef<int(int) const> ref(lambda);
  EXPECT_EQ(ref(3), 6);
}

TEST(FunctionRefTest, StatelessRvalue) {
  auto call_ref = [](FunctionRef<int(int, int)> ref) { return ref(2, 3); };
  EXPECT_EQ(call_ref([](int a, int b) { return a + b; }), 5);
}

TEST(FunctionRefTest, SmallRvalue) {
  int factor = 2;
  auto call_ref = [](FunctionRef<int(int)> ref) { return ref(3); };
  EXPECT_EQ(call_ref([factor](int a) { return a * factor; }), 6);
}

TEST(FunctionRefTest, Properties) {
  static_assert(std::is_trivially_copyable_v<FunctionRef<void()>>);
  static_assert(std::is_copy_constructible_v<FunctionRef<void()>> &&
                std::is_copy_assignable_v<FunctionRef<void()>>);
  static_assert(!std::is_default_constructible_v<FunctionRef<void()>>);
}

TEST(FunctionRefTest, Size) {
  static_assert(sizeof(FunctionRef<void()>) == 2 * sizeof(void*));
  static_assert(sizeof(FunctionRef<void() const>) == 2 * sizeof(void*));
  static_assert(sizeof(FunctionRef<void() noexcept>) == 2 * sizeof(void*));
  static_assert(sizeof(FunctionRef<void() const noexcept>) ==
                2 * sizeof(void*));
}

struct Foo {
  int Bar(int a) const { return a + 1; }
};

TEST(FunctionRefTest, MemberFunction) {
  Foo foo;
  auto lambda = [&foo](int a) { return foo.Bar(a); };
  FunctionRef<int(int)> ref(lambda);
  EXPECT_EQ(ref(5), 6);
}

TEST(FunctionRefTest, NoexceptSignature) {
  auto lambda = [](int a) noexcept { return a * 2; };
  FunctionRef<int(int) noexcept> ref(lambda);
  EXPECT_EQ(ref(3), 6);
}

TEST(FunctionRefTest, ConstNoexceptSignature) {
  auto lambda = [](int a) noexcept { return a * 2; };
  FunctionRef<int(int) const noexcept> ref(lambda);
  EXPECT_EQ(ref(3), 6);
}

TEST(FunctionRefTest, CopySupport) {
  auto lambda = [](int a) { return a * 2; };
  FunctionRef<int(int)> ref1(lambda);
  FunctionRef<int(int)> ref2 = ref1;  // Copy construct
  EXPECT_EQ(ref2(3), 6);

  auto lambda2 = [](int a) { return a * 3; };
  FunctionRef<int(int)> ref3(lambda2);
  ref2 = ref3;  // Copy assign
  EXPECT_EQ(ref2(3), 9);
}

TEST(FunctionRefTest, MoveSupport) {
  auto lambda = [](int a) { return a * 2; };
  FunctionRef<int(int)> ref1(lambda);
  FunctionRef<int(int)> ref2 = std::move(ref1);  // Move construct
  EXPECT_EQ(ref2(3), 6);

  auto lambda2 = [](int a) { return a * 3; };
  FunctionRef<int(int)> ref3(lambda2);
  ref2 = std::move(ref3);  // Move assign
  EXPECT_EQ(ref2(3), 9);
}

}  // namespace
}  // namespace pw
