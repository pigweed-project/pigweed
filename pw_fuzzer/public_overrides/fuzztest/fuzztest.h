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

/// @file fuzztest.h
/// Stubs for the FuzzTest interface
///
/// @rst
/// .. warning::
///    This header depends on portions of the standard library that may not
///    supported on your device!
///
/// This header provides stubs for the full FuzzTest interface, including
/// `macros`_ and `domains`_ that include C++ standard library `headers`_ that
/// are not permitted in Pigweed. It should only be used in downstream projects
/// that support the full standard library on both host and device. It should
/// never be used in core Pigweed.
///
/// If possible, consider including ``pw_fuzzer/fuzztest.h`` instead.
///
/// This header is included when FuzzTest is disabled, e.g. for GN, when
/// ``dir_pw_third_party_fuzztest`` or ``pw_toolchain_FUZZING_ENABLED`` are not
/// set. Otherwise, ``$dir_pw_third_party_fuzztest/fuzztest.h`` is used instead.
///
/// .. _domains:
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md
/// .. _headers: https://pigweed.dev/docs/style_guide.html#permitted-headers
/// .. _macros:
/// https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md
/// @endrst

#include <deque>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pw_fuzzer/internal/fuzztest.h"  // IWYU pragma: export

namespace fuzztest {

// This namespace is here only as a way to disable ADL (argument-dependent
// lookup). Names should be used from the fuzztest:: namespace.
namespace internal_no_adl {

template <typename T>
auto ElementOf(std::vector<T>) {
  return internal::Domain<T>{};
}

inline auto String() { return Arbitrary<std::string>(); }

template <int&... ExplicitArgumentBarrier, typename T>
inline auto StringOf(internal::Domain<T> inner) {
  return ContainerOf<std::string>(std::move(inner));
}

inline auto AsciiString() { return StringOf(AsciiChar()); }

inline auto PrintableAsciiString() { return StringOf(PrintableAsciiChar()); }

template <template <typename> class Ptr,
          int&... ExplicitArgumentBarrier,
          typename T>
auto SmartPointerOf(internal::Domain<T>) {
  return internal::Domain<Ptr<T>>{};
}

template <int&... ExplicitArgumentBarrier, typename T>
auto UniquePtrOf(internal::Domain<T>) {
  return internal::Domain<std::unique_ptr<T>>{};
}

template <int&... ExplicitArgumentBarrier, typename T>
auto SharedPtrOf(internal::Domain<T>) {
  return internal::Domain<std::shared_ptr<T>>{};
}

template <int&... ExplicitArgumentBarrier, typename T>
auto VectorOf(internal::Domain<T> inner) {
  return ContainerOf<std::vector<T>>(std::move(inner));
}

template <int&... ExplicitArgumentBarrier, typename T>
auto DequeOf(internal::Domain<T> inner) {
  return ContainerOf<std::deque<T>>(std::move(inner));
}

template <int&... ExplicitArgumentBarrier, typename T>
auto ListOf(internal::Domain<T> inner) {
  return ContainerOf<std::list<T>>(std::move(inner));
}

template <int&... ExplicitArgumentBarrier, typename T>
auto SetOf(internal::Domain<T> inner) {
  return ContainerOf<std::set<T>>(std::move(inner));
}

template <int&... ExplicitArgumentBarrier, typename K, typename V>
auto MapOf(internal::Domain<K> keys, internal::Domain<V> values) {
  return ContainerOf<std::map<K, V>>(
      PairOf(std::move(keys), std::move(values)));
}

template <int&... ExplicitArgumentBarrier, typename T>
auto UnorderedSetOf(internal::Domain<T> inner) {
  return ContainerOf<std::unordered_set<T>>(std::move(inner));
}

template <int&... ExplicitArgumentBarrier, typename K, typename V>
auto UnorderedMapOf(internal::Domain<K> keys, internal::Domain<V> values) {
  return ContainerOf<std::unordered_map<K, V>>(
      PairOf(std::move(keys), std::move(values)));
}

template <typename T>
auto UniqueElementsVectorOf(internal::Domain<T> inner) {
  return VectorOf(std::move(inner));
}

template <typename P,
          typename T = std::remove_cv_t<
              std::remove_pointer_t<decltype(std::declval<P>()())>>>
auto ProtobufOf(P) {
  return internal::Domain<std::unique_ptr<T>>{};
}

}  // namespace internal_no_adl

// Inject the names from internal_no_adl into fuzztest, without allowing for
// ADL. Note that an `inline` namespace would not have this effect (ie it would
// still allow ADL to trigger).
using namespace internal_no_adl;  // NOLINT

}  // namespace fuzztest
