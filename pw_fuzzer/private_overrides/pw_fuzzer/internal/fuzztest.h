// Copyright 2023 The Pigweed Authors
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
#pragma once

// IWYU pragma: private, include "pw_fuzzer/fuzztest.h"

/// @file
/// Stubs for the Pigweed-compatible subset of the FuzzTest interface
///
/// @rst
/// This header provides stubs for the portion of the FuzzTest interface that
/// only depends on permitted C++ standard library `headers`_, including
/// `macros`_ and `domains`_.
///
/// This header is included when FuzzTest is disabled, e.g. for GN, when
/// ``dir_pw_third_party_fuzztest`` or ``pw_toolchain_FUZZING_ENABLED`` are not
/// set. Otherwise, ``//pw_fuzzer/public/pw_fuzzer/internal/fuzztest.h`` is used
/// instead.
///
/// .. _domains:
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md
/// .. _headers: https://pigweed.dev/docs/style_guide.html#permitted-headers
/// .. _macros:
/// https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md
/// @endrst

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#define FUZZ_TEST(test_suite_name, test_name)                              \
  TEST(test_suite_name, DISABLED_##test_name) {}                           \
  auto _pw_fuzzer_##test_suite_name##_##test_name##_FUZZTEST_NOT_PRESENT = \
      fuzztest::internal::TypeCheckFuzzTest(test_name).IgnoreFunction()

#define FUZZ_TEST_F(test_fixture, test_name)                            \
  TEST_F(test_fixture, DISABLED_##test_name) {}                         \
  auto _pw_fuzzer_##test_fixture##_##test_name##_FUZZTEST_NOT_PRESENT = \
      fuzztest::internal::TypeCheckFuzzTest(test_name).IgnoreFunction()

namespace fuzztest {

/// Stub for a FuzzTest domain that produces values.
///
/// In FuzzTest, domains are used to provide values of specific types when
/// fuzzing. However, FuzzTest is only optionally supported on host with Clang.
/// For other build configurations, this struct provides a FuzzTest-compatible
/// stub that can be used to perform limited type-checking at build time.
///
/// Fuzzer authors must not invoke this type directly. Instead, use the factory
/// methods for domains such as `Arbitrary`, `VectorOf`, `Map`, etc.
template <typename T>
struct Domain {
  using value_type = T;
};

namespace internal {

/// Stub for a FuzzTest domain that produces containers of values.
///
/// This struct is an extension of `Domain` that add stubs for the methods that
/// control container size.
template <typename T>
struct ContainerDomain : public Domain<T> {
  template <typename U, typename = std::enable_if_t<std::is_integral_v<U>>>
  ContainerDomain<T>& WithSize(U) {
    return *this;
  }

  template <typename U, typename = std::enable_if_t<std::is_integral_v<U>>>
  ContainerDomain<T>& WithMinSize(U) {
    return *this;
  }

  template <typename U, typename = std::enable_if_t<std::is_integral_v<U>>>
  ContainerDomain<T>& WithMaxSize(U) {
    return *this;
  }
};

/// Stub for a FuzzTest domain that produces optional values.
///
/// This struct is an extension of `Domain` that add stubs for the methods that
/// control nullability.
template <typename T>
struct OptionalDomain : public Domain<T> {
  OptionalDomain<T>& SetAlwaysNull() { return *this; }
  OptionalDomain<T>& SetWithoutNull() { return *this; }
};

/// Register a FuzzTest stub.
///
/// FuzzTest is only optionally supported on host with Clang. For other build
/// configurations, this struct provides a FuzzTest-compatible stub of a test
/// registration that only performs limited type-checking at build time.
///
/// Fuzzer authors must not invoke this type directly. Instead, use the
/// `FUZZ_TEST` and/or `FUZZ_TEST_F` macros.
template <typename TargetFunction>
struct TypeCheckFuzzTest {
  TypeCheckFuzzTest(TargetFunction) {}

  TypeCheckFuzzTest<TargetFunction>& IgnoreFunction() { return *this; }

  template <int&... ExplicitArgumentBarrier,
            typename... Domains,
            typename T = std::decay_t<
                std::invoke_result_t<TargetFunction,
                                     const typename Domains::value_type&...>>>
  TypeCheckFuzzTest<TargetFunction>& WithDomains(Domains...) {
    return *this;
  }

  template <int&... ExplicitArgumentBarrier,
            typename... Seeds,
            typename T = std::decay_t<
                std::invoke_result_t<TargetFunction, const Seeds&...>>>
  TypeCheckFuzzTest<TargetFunction>& WithSeeds(Seeds...) {
    return *this;
  }
};

template <typename TargetFunction>
TypeCheckFuzzTest(TargetFunction) -> TypeCheckFuzzTest<TargetFunction>;

}  // namespace internal

// The remaining functions match those defined by fuzztest/fuzztest.h.
//
// This namespace is here only as a way to disable ADL (argument-dependent
// lookup). Names should be used from the fuzztest:: namespace.
namespace internal_no_adl {

////////////////////////////////////////////////////////////////
// Arbitrary domains
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#arbitrary-domains

template <typename T>
auto Arbitrary() {
  return Domain<T>{};
}

////////////////////////////////////////////////////////////////
// Other miscellaneous domains
// These typically appear later in docs and tests. They are placed early in this
// file to allow other domains to be defined using them.

// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#oneof
template <int&... ExplicitArgumentBarrier, typename T, typename... Domains>
auto OneOf(Domain<T>, Domains...) {
  return Domain<T>{};
}

// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#oneof
template <typename T>
auto Just(T) {
  return Domain<T>{};
}

// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#map
template <int&... ExplicitArgumentBarrier, typename Mapper, typename... Inner>
auto Map(Mapper, Inner...) {
  return Domain<std::decay_t<
      std::invoke_result_t<Mapper, typename Inner::value_type&...>>>{};
}

// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#flatmap
template <typename FlatMapper, typename... Inner>
using FlatMapOutputDomain = std::decay_t<
    std::invoke_result_t<FlatMapper, typename Inner::value_type&...>>;
template <int&... ExplicitArgumentBarrier,
          typename FlatMapper,
          typename... Inner>
auto FlatMap(FlatMapper, Inner...) {
  return Domain<
      typename FlatMapOutputDomain<FlatMapper, Inner...>::value_type>{};
}

// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#filter
template <int&... ExplicitArgumentBarrier, typename T, typename Pred>
auto Filter(Pred, Domain<T>) {
  return Domain<T>{};
}

////////////////////////////////////////////////////////////////
// Numerical domains
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains

template <typename T>
auto InRange(T min, T max) {
  return Filter([min, max](T t) { return min <= t && t <= max; },
                Arbitrary<T>());
}

template <typename T>
auto NonZero() {
  return Filter([](T t) { return t != 0; }, Arbitrary<T>());
}

template <typename T>
auto Positive() {
  if constexpr (std::is_floating_point_v<T>) {
    return InRange<T>(std::numeric_limits<T>::denorm_min(),
                      std::numeric_limits<T>::max());
  } else {
    return InRange<T>(T{1}, std::numeric_limits<T>::max());
  }
}

template <typename T>
auto NonNegative() {
  return InRange<T>(T{}, std::numeric_limits<T>::max());
}

template <typename T>
auto Negative() {
  static_assert(!std::is_unsigned_v<T>,
                "Negative<T>() can only be used with with signed T-s! "
                "For char, consider using signed char.");
  if constexpr (std::is_floating_point_v<T>) {
    return InRange<T>(std::numeric_limits<T>::lowest(),
                      -std::numeric_limits<T>::denorm_min());
  } else {
    return InRange<T>(std::numeric_limits<T>::min(), T{-1});
  }
}

template <typename T>
auto NonPositive() {
  static_assert(!std::is_unsigned_v<T>,
                "NonPositive<T>() can only be used with with signed T-s! "
                "For char, consider using signed char.");
  return InRange<T>(std::numeric_limits<T>::lowest(), T{});
}

template <typename T>
auto Finite() {
  static_assert(std::is_floating_point_v<T>,
                "Finite<T>() can only be used with floating point types!");
  return Filter([](T f) { return std::isfinite(f); }, Arbitrary<T>());
}

////////////////////////////////////////////////////////////////
// Character domains
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains

inline auto NonZeroChar() { return Positive<char>(); }
inline auto NumericChar() { return InRange<char>('0', '9'); }
inline auto LowerChar() { return InRange<char>('a', 'z'); }
inline auto UpperChar() { return InRange<char>('A', 'Z'); }
inline auto AlphaChar() { return OneOf(LowerChar(), UpperChar()); }
inline auto AlphaNumericChar() { return OneOf(AlphaChar(), NumericChar()); }
inline auto AsciiChar() { return InRange<char>(0, 127); }
inline auto PrintableAsciiChar() { return InRange<char>(32, 126); }

////////////////////////////////////////////////////////////////
// Regular expression domains

// TODO: b/285775246 - Add support for `fuzztest::InRegexp`.
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#inregexp-domains
// inline auto InRegexp(std::string_view) {
//   return Domain<std::string_view>{};
// }

////////////////////////////////////////////////////////////////
// Enumerated domains
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#elementof-domains

template <typename T>
auto ElementOf(std::initializer_list<T>) {
  return Domain<T>{};
}

template <typename T>
auto BitFlagCombinationOf(std::initializer_list<T>) {
  return Domain<T>{};
}

////////////////////////////////////////////////////////////////
// Container domains
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#container-combinators

template <typename T, int&... ExplicitArgumentBarrier, typename U>
auto ContainerOf(Domain<U>) {
  return internal::ContainerDomain<T>{};
}

template <template <typename, typename...> class T,
          int&... ExplicitArgumentBarrier,
          typename U>
auto ContainerOf(Domain<U>) {
  return internal::ContainerDomain<T<U>>{};
}

template <typename T, int&... ExplicitArgumentBarrier, typename U>
auto UniqueElementsContainerOf(Domain<U>) {
  return internal::ContainerDomain<T>{};
}

template <int&... ExplicitArgumentBarrier, typename T>
auto NonEmpty(internal::ContainerDomain<T> inner) {
  return inner.WithMinSize(1);
}

////////////////////////////////////////////////////////////////
// Aggregate domains
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#container-combinators

template <int&... ExplicitArgumentBarrier, typename T, typename... Domains>
auto ArrayOf(Domain<T>, Domains... others) {
  return Domain<std::array<T, 1 + sizeof...(others)>>{};
}

template <int N, int&... ExplicitArgumentBarrier, typename T>
auto ArrayOf(const Domain<T>&) {
  return Domain<std::array<T, N>>{};
}

template <typename T, int&... ExplicitArgumentBarrier, typename... Inner>
auto StructOf(Inner...) {
  return Domain<T>{};
}

template <typename T, int&... ExplicitArgumentBarrier>
auto ConstructorOf() {
  return Domain<T>{};
}

template <typename T,
          int&... ExplicitArgumentBarrier,
          typename U,
          typename... Inner>
auto ConstructorOf(Domain<U>, Inner... inner) {
  return ConstructorOf<T>(inner...);
}

template <int&... ExplicitArgumentBarrier, typename T1, typename T2>
auto PairOf(Domain<T1>, Domain<T2>) {
  return Domain<std::pair<T1, T2>>{};
}

template <int&... ExplicitArgumentBarrier, typename... Inner>
auto TupleOf(Inner...) {
  return Domain<std::tuple<typename Inner::value_type...>>{};
}

template <typename T, int&... ExplicitArgumentBarrier, typename... Inner>
auto VariantOf(Inner...) {
  return Domain<T>{};
}

template <int&... ExplicitArgumentBarrier, typename... Inner>
auto VariantOf(Inner...) {
  return Domain<std::variant<typename Inner::value_type...>>{};
}

template <template <typename> class Optional,
          int&... ExplicitArgumentBarrier,
          typename T>
auto OptionalOf(Domain<T>) {
  return internal::OptionalDomain<Optional<T>>{};
}

template <int&... ExplicitArgumentBarrier, typename T>
auto OptionalOf(Domain<T> inner) {
  return OptionalOf<std::optional>(inner);
}

template <typename T>
auto NullOpt() {
  return internal::OptionalDomain<std::optional<T>>{}.SetAlwaysNull();
}

template <int&... ExplicitArgumentBarrier, typename T>
auto NonNull(internal::OptionalDomain<T> inner) {
  return inner.SetWithoutNull();
}

}  // namespace internal_no_adl

// Inject the names from internal_no_adl into fuzztest, without allowing for
// ADL. Note that an `inline` namespace would not have this effect (ie it would
// still allow ADL to trigger).
using namespace internal_no_adl;  // NOLINT

}  // namespace fuzztest
