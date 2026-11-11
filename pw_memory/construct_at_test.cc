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

#include "pw_memory/construct_at.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "pw_polyfill/language_feature_macros.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

template <typename, typename T, typename... Args>
struct CanConstructAtImpl : std::false_type {};

template <typename T, typename... Args>
struct CanConstructAtImpl<std::void_t<decltype(pw::construct_at(
                              std::declval<T*>(), std::declval<Args>()...))>,
                          T,
                          Args...> : std::true_type {};

template <typename T, typename... Args>
constexpr bool kCanConstructAt = CanConstructAtImpl<void, T, Args...>::value;

struct MultiArg {
  constexpr MultiArg(int first, int second) : a(first), b(second) {}

  int a;
  int b;
};

struct WeirdOperators {
  constexpr explicit WeirdOperators(int v) : value(v) {}

  void operator&() const = delete;  // NOLINT(google-runtime-operator)
  static void* operator new(size_t, void*) = delete;

  int value;
};

struct CopyAndMoveTracker {
  constexpr explicit CopyAndMoveTracker(int v) : value(v) {}
  constexpr CopyAndMoveTracker(const CopyAndMoveTracker& other)
      : value(other.value), copied(true) {}
  PW_CONSTEXPR_CPP20 CopyAndMoveTracker(CopyAndMoveTracker&& other) noexcept
      : value(std::exchange(other.value, -1)), moved(true) {}

  int value;
  bool copied = false;
  bool moved = false;
};

struct NonTrivialConstexpr {
  constexpr NonTrivialConstexpr(int v, int* dtor_count)
      : value(v), dtor_count_(dtor_count) {}

  PW_CONSTEXPR_CPP20 NonTrivialConstexpr(NonTrivialConstexpr&& other) noexcept
      : value(other.value),
        dtor_count_(std::exchange(other.dtor_count_, nullptr)) {}

  PW_CONSTEXPR_CPP20 ~NonTrivialConstexpr() {
    if (dtor_count_ != nullptr) {
      *dtor_count_ += 1;
    }
  }

  int value;
  int* dtor_count_;
};

template <typename T>
union Storage {
  constexpr Storage() : empty{} {}
  PW_CONSTEXPR_CPP20 ~Storage() {}

  char empty;
  T value;
};

static_assert(kCanConstructAt<int>);
static_assert(kCanConstructAt<int, int>);
static_assert(!kCanConstructAt<int, int, int>);
static_assert(!kCanConstructAt<void>);
static_assert(!kCanConstructAt<const void>);
static_assert(!kCanConstructAt<void()>);
static_assert(!kCanConstructAt<int[]>);
static_assert(!kCanConstructAt<int[], int>);
static_assert(!kCanConstructAt<MultiArg>);
static_assert(!kCanConstructAt<MultiArg, int, void*>);
static_assert(kCanConstructAt<MultiArg, int, int>);

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, ValueInitializesWhenGivenNoArgs, {
  int value = 99;
  int* ptr = pw::construct_at(&value);
  PW_TEST_EXPECT_EQ(ptr, &value);
  PW_TEST_EXPECT_EQ(value, 0);
});

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, ForwardsMultipleArguments, {
  Storage<MultiArg> storage;

  MultiArg* ptr = pw::construct_at(&storage.value, 10, 20);
  PW_TEST_EXPECT_EQ(ptr, &storage.value);
  PW_TEST_EXPECT_EQ(storage.value.a, 10);
  PW_TEST_EXPECT_EQ(storage.value.b, 20);
});

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, ExplicitTemplateArgument, {
  Storage<MultiArg> storage;

  MultiArg* ptr = pw::construct_at<MultiArg>(&storage.value, 3, 4);
  PW_TEST_EXPECT_EQ(ptr, &storage.value);
  PW_TEST_EXPECT_EQ(storage.value.a, 3);
  PW_TEST_EXPECT_EQ(storage.value.b, 4);
});

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, CopiesLvalueArgument, {
  CopyAndMoveTracker source(21);
  Storage<CopyAndMoveTracker> storage;

  pw::construct_at(&storage.value, source);
  PW_TEST_EXPECT_EQ(storage.value.value, 21);
  PW_TEST_EXPECT_TRUE(storage.value.copied);
  PW_TEST_EXPECT_FALSE(storage.value.moved);
  PW_TEST_EXPECT_EQ(source.value, 21);
});

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, IgnoresDeletedAddressAndNewOperators, {
  Storage<WeirdOperators> storage;

  WeirdOperators* ptr = pw::construct_at(std::addressof(storage.value), 42);
  PW_TEST_EXPECT_EQ(ptr, std::addressof(storage.value));
  PW_TEST_EXPECT_EQ(storage.value.value, 42);
});

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, NonTrivialUnionLifecycle, {
  int destroyed = 0;
  Storage<NonTrivialConstexpr> storage;

  pw::construct_at(&storage.value, 7, &destroyed);
  PW_TEST_EXPECT_EQ(storage.value.value, 7);
  PW_TEST_EXPECT_EQ(destroyed, 0);

  std::destroy_at(&storage.value);
  PW_TEST_EXPECT_EQ(destroyed, 1);

  pw::construct_at(&storage.value, 8, &destroyed);
  PW_TEST_EXPECT_EQ(storage.value.value, 8);
  std::destroy_at(&storage.value);
  PW_TEST_EXPECT_EQ(destroyed, 2);
});

PW_CONSTEXPR_TEST_IF_CPP20(ConstructAt, MovesMoveOnlyArgument, {
  int destroyed = 0;
  Storage<NonTrivialConstexpr> storage;
  {
    NonTrivialConstexpr source(55, &destroyed);
    pw::construct_at(&storage.value, std::move(source));
    PW_TEST_EXPECT_EQ(storage.value.value, 55);
  }
  PW_TEST_EXPECT_EQ(destroyed, 0);

  std::destroy_at(&storage.value);
  PW_TEST_EXPECT_EQ(destroyed, 1);
});

}  // namespace
