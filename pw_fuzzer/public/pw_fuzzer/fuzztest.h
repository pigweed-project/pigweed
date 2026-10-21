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
/// Pigweed interface to FuzzTest
///
/// @rst
/// This header exposes the portion of the FuzzTest interface that only depends
/// on permitted C++ standard library `headers`_, including `macros`_ and
/// `domains`_.
///
/// It also extends the interface to provide domains for common Pigweed types,
/// such as those from the following modules:
///
/// * :ref:`module-pw_containers`
/// * :ref:`module-pw_result`
/// * :ref:`module-pw_status`
/// * :ref:`module-pw_string`
///
/// .. _domains:
///    https://github.com/google/fuzztest/blob/main/doc/domains-reference.md
/// .. _headers: https://pigweed.dev/docs/style_guide.html#permitted-headers
/// .. _macros:
///    https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md
/// @endrst

#include "pw_containers/flat_map.h"
#include "pw_containers/inline_deque.h"
#include "pw_containers/inline_queue.h"
#include "pw_containers/intrusive_list.h"
#include "pw_containers/vector.h"
#include "pw_fuzzer/internal/fuzztest.h"  // IWYU pragma: export
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_string/string.h"

/// Fuzz testing library
namespace pw::fuzzer {

template <typename T>
using Domain = fuzztest::Domain<T>;

////////////////////////////////////////////////////////////////
// Arbitrary domains
// Additional specializations are provided with the Pigweed domains.

/// @struct ArbitraryImpl
/// @fn Arbitrary
///
/// Produces values for fuzz target function parameters.
///
/// This defines a new template rather than using the `fuzztest` one directly in
/// order to facilitate specializations for Pigweed types.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#arbitrary-domains
template <typename T, typename = void>
struct ArbitraryImpl {
  auto operator()() { return fuzztest::Arbitrary<T>(); }
};
template <typename T>
auto Arbitrary() {
  return ArbitraryImpl<T>()();
}

////////////////////////////////////////////////////////////////
// Numerical domains

/// Produces values from a closed interval.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::InRange;

/// Like Arbitrary<T>(), but does not produce the zero value.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::NonZero;

/// Produces numbers greater than zero.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::Positive;

/// Produces the zero value and numbers greater than zero.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::NonNegative;

/// Produces numbers less than zero.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::Negative;

/// Produces the zero value and numbers less than zero.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::NonPositive;

/// Produces floating points numbers that are neither infinity nor NaN.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#numerical-domains
using fuzztest::Finite;

////////////////////////////////////////////////////////////////
// Character domains

/// Produces any char except '\0'.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::NonZeroChar;

/// Alias for `InRange('0', '9')`.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::NumericChar;

/// Alias for `InRange('a', 'z')`.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::LowerChar;

/// Alias for `InRange('A', 'Z')`.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::UpperChar;

/// Alias for `OneOf(LowerChar(), UpperChar())`.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::AlphaChar;

/// Alias for `OneOf(AlphaChar(), NumericChar())`.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::AlphaNumericChar;

/// Produces printable characters (`InRange<char>(32, 126)`).
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::PrintableAsciiChar;

/// Produces ASCII characters (`InRange<char>(0, 127)`).
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#character-domains
using fuzztest::AsciiChar;

////////////////////////////////////////////////////////////////
// Regular expression domains

// TODO: b/285775246 - Add support for `fuzztest::InRegexp`.
// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#inregexp-domains
// using fuzztest::InRegexp;

////////////////////////////////////////////////////////////////
// Enumerated domains

/// Produces values from an enumerated set.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#elementof-domains
using fuzztest::ElementOf;

/// Produces combinations of binary flags from a provided list.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#bitflagcombinationof-domains
using fuzztest::BitFlagCombinationOf;

////////////////////////////////////////////////////////////////
// Container domains

/// @struct ContainerOfImpl
/// @fn ContainerOf
///
/// Produces containers of elements provided by inner domains.
///
/// This defines a new template rather than using the `fuzztest` one directly in
/// order to specify the static container capacity as part of the container
/// type.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#container-combinators
template <typename Container, typename = void>
struct ContainerOfImpl {
  template <int&... ExplicitArgumentBarrier, typename Inner>
  auto operator()(Inner inner) {
    return fuzztest::ContainerOf<Container>(std::move(inner))
        .WithMaxSize(Container{}.max_size());
  }
};
template <typename Container, int&... ExplicitArgumentBarrier, typename Inner>
auto ContainerOf(Inner inner) {
  return ContainerOfImpl<Container>()(std::move(inner));
}

namespace internal {

template <typename T, typename = void>
struct IsContainer : std::false_type {};

template <typename T>
struct IsContainer<T, std::void_t<decltype(T().begin(), T().end())>>
    : std::true_type {};

}  // namespace internal

/// Produces containers of at least one element provided by inner domains.
///
/// The container type is given by a template parameter.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#container-combinators
using fuzztest::NonEmpty;

/// @struct UniqueElementsContainerOfImpl
/// @fn UniqueElementsContainerOf
///
/// Produces containers of unique elements provided by inner domains.
///
/// This defines a new template rather than using the `fuzztest` one directly in
/// order to specify the static container capacity as part of the container
/// type.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#container-combinators
template <typename Container, typename = void>
struct UniqueElementsContainerOfImpl {
  template <int&... ExplicitArgumentBarrier, typename Inner>
  auto operator()(Inner inner) {
    return fuzztest::UniqueElementsContainerOf<Container>(std::move(inner))
        .WithMaxSize(Container{}.max_size());
  }
};
template <typename Container, int&... ExplicitArgumentBarrier, typename Inner>
auto UniqueElementsContainerOf(Inner inner) {
  return UniqueElementsContainerOfImpl<Container>()(std::move(inner));
}

////////////////////////////////////////////////////////////////
// Aggregate domains

/// Produces std::array<T>s of elements provided by inner domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#container-combinators
using fuzztest::ArrayOf;

/// Specializes @cpp_class{pw::fuzzer::ContainerOfImpl} for arrays, which do not
/// need a maximum size applied.
///
/// @param[in]  inner   Domain the produces values of type `T`.
///
/// @retval     Domain that produces `std::array<T, kArraySize>`s.
template <typename T, size_t N>
struct ContainerOfImpl<std::array<T, N>> {
  template <int&... ExplicitArgumentBarrier, typename Inner>
  auto operator()(Inner inner) {
    return fuzztest::ContainerOf<std::array<T, N>>(std::move(inner));
  }
};

/// Produces user-defined structs.
///
/// The struct type is given by a template parameter.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#structof
using fuzztest::StructOf;

/// Produces user-defined objects.
///
/// The class type is given by a template parameter.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#constructorof
using fuzztest::ConstructorOf;

/// Produces `std::pair<T1, T2>`s from provided inner domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#pairof
using fuzztest::PairOf;

/// Produces `std::tuple<T...>`s from provided inner domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#tupleof
using fuzztest::TupleOf;

/// Produces `std::variant<T...>` from provided inner domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#variantof
using fuzztest::VariantOf;

/// Produces `std::optional<T>` from provided inner domain.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#optionalof
using fuzztest::OptionalOf;

/// Produces `std::optional<T>` from provided inner domain that is null.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#optionalof
using fuzztest::NullOpt;

/// Produces `std::optional<T>` from provided inner domain that is not null.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#optionalof
using fuzztest::NonNull;

////////////////////////////////////////////////////////////////
// Other miscellaneous domains

/// Produces values by choosing between provided inner domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#oneof
using fuzztest::OneOf;

/// Produces values equal to the provided value.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#oneof
using fuzztest::Just;

/// Produces values by applying a function to values from provided domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#map
using fuzztest::Map;

/// Creates a domain by applying a function to values from provided domains.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#flatmap
using fuzztest::FlatMap;

/// Produces values from a provided domain that will cause a provided predicate
/// to return true.
///
/// See
/// https://github.com/google/fuzztest/blob/main/doc/domains-reference.md#filter
using fuzztest::Filter;

////////////////////////////////////////////////////////////////
// pw_status-related types

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::Status}.
template <>
struct ArbitraryImpl<Status> {
  auto operator()() {
    return ConstructorOf<Status>(
        Map([](int code) { return static_cast<pw_Status>(code); },
            InRange<int>(PW_STATUS_OK, PW_STATUS_LAST)));
  }
};

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::StatusWithSize}.
template <>
struct ArbitraryImpl<StatusWithSize> {
  auto operator()() {
    return ConstructorOf<StatusWithSize>(Arbitrary<Status>(),
                                         Arbitrary<size_t>());
  }
};

/// Like @cpp_func{pw::fuzzer::Arbitrary<Status>}, except that
/// @cpp_func{pw::OkStatus} is filtered out.
inline auto NonOkStatus() {
  return ConstructorOf<pw::Status>(
      Map([](int code) { return static_cast<pw_Status>(code); },
          InRange<int>(PW_STATUS_CANCELLED, PW_STATUS_UNAUTHENTICATED)));
}

////////////////////////////////////////////////////////////////
// pw_result-related types

/// Returns a FuzzTest domain that produces @cpp_class{pw::Result}s.
///
/// The value produced may either be value produced by the given domain, or a
/// @cpp_class{pw::Status} indicating an error.
///
/// Alternatively, you can use `Arbitrary<Result<T>>`.
///
/// @param[in] inner  Domain that produces values of type `T`.
///
/// @retval    Domain that produces `Result<T>`s.
template <int&... ExplicitArgumentBarrier, typename Inner>
auto ResultOf(Inner inner) {
  return Map(
      [](std::optional<typename Inner::value_type> value, Status status) {
        if (value) {
          return Result<typename Inner::value_type>(*value);
        }
        return Result<typename Inner::value_type>(status);
      },
      OptionalOf(std::move(inner)),
      NonOkStatus());
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::Result}.
template <typename T>
struct ArbitraryImpl<Result<T>> {
  auto operator()() { return ResultOf(Arbitrary<T>()); }
};

////////////////////////////////////////////////////////////////
// pw_containers-related types

/// Returns a FuzzTest domain that produces @cpp_class{pw::Vector}s.
///
/// Use this in place of `fuzztest::VectorOf`. The vector's maximum size is set
/// by the template parameter.
///
/// Alternatively, you can use `Arbitrary<Vector<T, kMaxSize>>`.
///
/// @param[in] inner  Domain that produces values of type `T`.
///
/// @retval    Domain that produces `Vector<T>`s.
template <size_t kMaxSize, int&... ExplicitArgumentBarrier, typename Inner>
auto VectorOf(Inner inner) {
  return fuzztest::ContainerOf<Vector<typename Inner::value_type, kMaxSize>>(
             inner)
      .WithMaxSize(kMaxSize);
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::Vector}.
template <typename T, size_t kMaxSize>
struct ArbitraryImpl<Vector<T, kMaxSize>> {
  auto operator()() { return VectorOf<kMaxSize>(Arbitrary<T>()); }
};

/// Like `fuzztest::UniqueElementsVectorOf`, but uses a @cpp_class{pw::Vector}
/// in place of a `std::vector`.
///
/// @param[in]  inner   Domain the produces values for the vector.
///
/// @retval     Domain that produces `@cpp_class{pw::Vector}`s.
template <size_t kMaxSize, int&... ExplicitArgumentBarrier, typename Inner>
auto UniqueElementsVectorOf(Inner inner) {
  return UniqueElementsContainerOf<
      Vector<typename Inner::value_type, kMaxSize>>(std::move(inner));
}

/// Returns a FuzzTest domain that produces @cpp_class{pw::containers::Pair}s.
///
/// Use this in place of `fuzztest::PairOf` when working with
/// @cpp_class{pw::containers::FlatMap}s.
///
/// Alternatively, you can use `Arbitrary<pw::containers::Pair<K, V>>`.
///
/// @param[in] keys     Domain that produces values of type `K`.
/// @param[in] values   Domain that produces values of type `V`.
///
/// @retval    Domain that produces `containers::Pair<T>`s.
template <int&... ExplicitArgumentBarrier,
          typename KeyDomain,
          typename ValueDomain>
auto FlatMapPairOf(KeyDomain keys, ValueDomain values) {
  return StructOf<containers::Pair<typename KeyDomain::value_type,
                                   typename ValueDomain::value_type>>(
      std::move(keys), std::move(values));
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::containers::Pair}.
template <typename K, typename V>
struct ArbitraryImpl<containers::Pair<K, V>> {
  auto operator()() { return FlatMapPairOf(Arbitrary<K>(), Arbitrary<V>()); }
};

/// Transforms a domain that produces containers of keys and values into a
/// domain that produces @cpp_class{pw::containers::FlatMap}s
///
/// This method can be used to apply additional constraints to the set of keys
/// and/or values overall, e.g. by requiring all keys to be unique.
///
/// @param[in] keys     Domain that produces containers of keys.
/// @param[in] values   Domain that produces containers of values.
///
/// @retval    Domain that produces `containers::Pair<T>`s.
template <size_t kArraySize,
          int&... ExplicitArgumentBarrier,
          typename KeyDomain,
          typename ValueDomain>
auto MapToFlatMap(KeyDomain keys, ValueDomain values) {
  using ContainerK = typename KeyDomain::value_type;
  using ContainerV = typename ValueDomain::value_type;
  static_assert(internal::IsContainer<ContainerK>::value);
  static_assert(internal::IsContainer<ContainerV>::value);
  using K = typename ContainerK::value_type;
  using V = typename ContainerV::value_type;
  return Map(
      [](const ContainerK& keys_c, const ContainerV& vals_c) {
        auto key = keys_c.begin();
        auto val = vals_c.begin();
        std::array<containers::Pair<K, V>, kArraySize> pairs;
        for (auto& item : pairs) {
          PW_ASSERT(key != keys_c.end());
          PW_ASSERT(val != vals_c.end());
          item.first = *key++;
          item.second = *val++;
        }
        return pairs;
      },
      std::move(keys),
      std::move(values));
}

/// Returns a FuzzTest domain that produces
/// @cpp_class{pw::containers::FlatMap}s.
///
/// Use this in place of `fuzztest::MapOf` and/or `fuzztest::UnorderedMapOf`.
/// The map's size is set by the template parameter. The map is populated by
/// pairs of values from the given `KeyDomain` and `ValueDomain`.
///
/// Alternatively, you can use `Arbitrary<FlatMap<K, V, kArraySize>>`.
///
/// Note that neither approach returns a domain that produces `FlatMap<K< V>`s.
/// Such a domain is infeasible, since `FlatMap<K, V>`s are not movable or
/// copyable. Instead, these functions return domains that produce arrays of
/// `Pair<K, V>`s that can be implicitly converted to `FlatMap<K, V>`s.
///
/// @param[in] keys    Domain that produces map keys.
/// @param[in] values  Domain that produces map values.
///
/// @retval    Domain that produces `std::array<T, kArraySize>`s.
template <size_t kArraySize,
          int&... ExplicitArgumentBarrier,
          typename KeyDomain,
          typename ValueDomain>
auto FlatMapOf(KeyDomain keys, ValueDomain values) {
  return ArrayOf<kArraySize>(FlatMapPairOf(std::move(keys), std::move(values)));
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::containers::FlatMap}.
template <typename K, typename V, size_t kArraySize>
struct ArbitraryImpl<containers::FlatMap<K, V, kArraySize>> {
  auto operator()() {
    return FlatMapOf<kArraySize>(Arbitrary<K>(), Arbitrary<V>());
  }
};

/// Implementation of @cpp_func{pw::fuzzer::ContainerOf} for
/// @cpp_class{pw::containers::FlatMap}.
///
/// Since flat maps have a static capacity, the returned domains do not produce
/// FuzzTest containers, but aggregates. As a result, container methods such as
/// `WithMaxSize` cannot be applied. Use @cpp_func{pw::fuzzer::MapToFlatMap}
/// instead to apply constraints to the set of keys and values.
///
/// @param[in]  inner   Domain the produces @cpp_class{pw::containers::Pair}s.
///
/// @retval     Domain that produces `@cpp_class{pw::containers::FlatMap}`s.
template <typename K, typename V, size_t kArraySize>
struct ContainerOfImpl<containers::FlatMap<K, V, kArraySize>> {
  template <int&... ExplicitArgumentBarrier, typename Inner>
  auto operator()(Inner inner) {
    static_assert(
        std::is_same_v<typename Inner::value_type, containers::Pair<K, V>>,
        "The domain passed to `pw::fuzzer::ContainerOf<FlatMap<K, V>>` must "
        "produce `pw::containers::Pair<K, V>`s. An example of a valid domain is"
        "`pw::fuzzer::FlatMapPairOf(FooDomain<K>(), BarDomain<V>())`");
    return ArrayOf<kArraySize>(std::move(inner));
  }
};

/// Transforms a domain that produces containers into a domain that produces
/// @cpp_class{pw::InlineDeque}s.
///
/// The domains returned by @cpp_func{pw::fuzzer::DequeOf} and
/// `Arbitrary<InlineDeque>` do not create FuzzTest containers. This method
/// can be used to apply container methods such as `WithMinSize` or
/// `UniqueElementsContainerOf` before building a deque from that container.
///
/// @param[in] inner  Domain that produces containers.
///
/// @retval    Domain that produces `@cpp_class{pw::InlineDeque}`s.
template <size_t kCapacity, int&... ExplicitArgumentBarrier, typename Inner>
auto MapToDeque(Inner inner) {
  using Container = typename Inner::value_type;
  static_assert(internal::IsContainer<Container>::value);
  using T = typename Container::value_type;
  return Map(
      [](const Container& items) {
        return InlineDeque<T, kCapacity>(items.begin(), items.end());
      },
      std::move(inner));
}

/// Returns a FuzzTest domain that produces @cpp_class{pw::InlineDeque}s.
///
/// Use this or @cpp_func{pw::fuzzer::DequeOf} in place of `fuzztest::DequeOf`.
/// The deque's maximum size is set by the template parameter.
///
/// Alternatively, you can use `Arbitrary<InlineDeque<T, kCapacity>>`.
///
/// @param[in] inner  Domain that produces values of type `T`.
///
/// @retval    Domain that produces values of type @cpp_class{pw::InlineDeque}
template <size_t kCapacity, int&... ExplicitArgumentBarrier, typename Inner>
auto DequeOf(Inner inner) {
  return MapToDeque<kCapacity>(VectorOf<kCapacity>(std::move(inner)));
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::InlineDeque}.
template <typename T, size_t kCapacity>
struct ArbitraryImpl<InlineDeque<T, kCapacity>> {
  auto operator()() { return DequeOf<kCapacity>(Arbitrary<T>()); }
};

/// Implementation of @cpp_func{pw::fuzzer::ContainerOf} for
/// @cpp_class{pw::containers::InlineDeque}.
///
/// Since inline deques have a static capacity, the returned domains do not
/// produce FuzzTest containers, but aggregates. As a result, container methods
/// such as `WithMaxSize` cannot be applied. Instead, use
/// @cpp_func{pw::fuzzer::MapToDeque} or @cpp_func{pw::fuzzer::MapToDeque}
/// to apply constraints to the set of keys and values.
///
/// @param[in]  inner   Domain the produces values of type `T`.
///
/// @retval     Domain that produces `@cpp_class{pw::InlineDeque}`s.
template <typename T, size_t kCapacity>
struct ContainerOfImpl<InlineDeque<T, kCapacity>> {
  template <int&... ExplicitArgumentBarrier, typename Inner>
  auto operator()(Inner inner) {
    return DequeOf<kCapacity>(std::move(inner));
  }
};

/// Transforms a domain that produces containers into a domain that produces
/// @cpp_class{pw::InlineQueue}s.
///
/// The domains returned by @cpp_func{pw::fuzzer::QueueOf} and
/// `Arbitrary<InlineQueue>` do not create FuzzTest containers. This method
/// can be used to apply container methods such as `WithMinSize` or
/// `UniqueElementsContainerOf` before building a queue from that container.
///
/// @param[in] inner  Domain that produces containers.
///
/// @retval    Domain that produces `@cpp_class{pw::InlineQueue}`s.
template <size_t kCapacity, int&... ExplicitArgumentBarrier, typename Inner>
auto MapToQueue(Inner inner) {
  using Container = typename Inner::value_type;
  static_assert(internal::IsContainer<Container>::value);
  using T = typename Container::value_type;
  return Map(
      [](const Container& items) {
        return InlineQueue<T, kCapacity>(items.begin(), items.end());
      },
      std::move(inner));
}

/// Returns a FuzzTest domain that produces @cpp_class{pw::InlineQueue}s.
///
/// Use this, @cpp_func{pw::fuzzer::QueueOf}, or
/// @cpp_func{pw::fuzzer::ScopedListOf} in place of `fuzztest::ListOf`. The
/// queue's maximum size is set by the template parameter.
///
/// Alternatively, you can use `Arbitrary<InlineQueue<T, kCapacity>>`.
///
/// @param[in] inner  Domain that produces values of type `T`.
///
/// @retval    Domain that produces values of type
///            `InlineQueue<T, kCapacity>`.
template <size_t kCapacity, int&... ExplicitArgumentBarrier, typename Inner>
auto QueueOf(Inner inner) {
  return MapToQueue<kCapacity>(VectorOf<kCapacity>(std::move(inner)));
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::InlineQueue}.
template <typename T, size_t kCapacity>
struct ArbitraryImpl<InlineQueue<T, kCapacity>> {
  auto operator()() { return QueueOf<kCapacity>(Arbitrary<T>()); }
};

/// Implementation of @cpp_func{pw::fuzzer::ContainerOf} for
/// @cpp_class{pw::containers::InlineQueue}.
///
/// Since inline queues have a static capacity, the returned domains do not
/// produce FuzzTest containers, but aggregates. As a result, container methods
/// such as `WithMaxSize` cannot be applied. Instead, use
/// @cpp_func{pw::fuzzer::MapToQueue} or @cpp_func{pw::fuzzer::MapToQueue}
/// to apply constraints to the set of keys and values.
///
/// @param[in]  inner   Domain the produces values of type `T`.
///
/// @retval     Domain that produces `@cpp_class{pw::InlineQueue}`s.
template <typename T, size_t kCapacity>
struct ContainerOfImpl<InlineQueue<T, kCapacity>> {
  template <int&... ExplicitArgumentBarrier, typename Inner>
  auto operator()(Inner inner) {
    return QueueOf<kCapacity>(std::move(inner));
  }
};

/// Associates an `IntrusiveList<T>` with a `Vector<T>` that stores its `Item`s.
///
/// The `Item`s are constructed from a sequence of argument tuples passed to
// constructor.
template <typename T, size_t kMaxSize>
class ScopedList {
 public:
  ~ScopedList() { list_.clear(); }

  template <int&... ExplicitArgumentBarrier, typename Tuple>
  explicit ScopedList(const Vector<Tuple>& arg_tuples) {
    for (const auto& arg_tuple : arg_tuples) {
      items_.emplace_back(std::make_from_tuple<T>(arg_tuple));
      list_.push_back(items_.back());
    }
  }

  ScopedList(const ScopedList& other) = delete;
  ScopedList& operator=(const ScopedList& other) = delete;

  ScopedList(ScopedList&& other) { *this = std::move(other); }
  ScopedList& operator=(ScopedList&& other) {
    list_.clear();
    other.list_.clear();
    items_ = std::move(other.items_);
    list_.assign(items_.begin(), items_.end());
    return *this;
  }

  const IntrusiveList<T>& list() const { return list_; }

 private:
  IntrusiveList<T> list_;
  Vector<T, kMaxSize> items_;
};

/// Transforms a domain that produces containers into a domain that produces
/// @cpp_class{pw::fuzzer::ScopedList}s.
///
/// The domains returned by @cpp_func{pw::fuzzer::ScopedListOf} do not create
/// FuzzTest containers. This method can be used to apply container methods such
/// as `WithMinSize` or `UniqueElementsContainerOf` before building an intrusive
/// list from that container.
///
/// @param[in] inner  Domain that produces containers.
///
/// @retval    Domain that produces `@cpp_class{pw::fuzzer::ScopedList}`s.
template <typename T,
          size_t kMaxSize,
          int&... ExplicitArgumentBarrier,
          typename Inner>
auto MapToScopedList(Inner inner) {
  using Container = typename Inner::value_type;
  static_assert(internal::IsContainer<Container>::value);
  using Tuple = typename Container::value_type;
  static_assert(
      std::is_same_v<T, decltype(std::make_from_tuple<T>(Tuple()))>,
      "The domain passed to `pw::fuzzer::MapToScopedList<T, kMaxSize>>` must "
      "produce `std::tuple`s of constructor arguments for `T`, e.g. using "
      "`pw::fuzzer::TupleOf`.");
  return ConstructorOf<ScopedList<T, kMaxSize>>(std::move(inner));
}

/// Returns a FuzzTest domain that produces @cpp_class{pw::fuzzer::ScopedList}s.
///
/// Use this, @cpp_func{pw::fuzzer::QueueOf}, or
/// @cpp_func{pw::fuzzer::QueueOf} in place of `fuzztest::ListOf`. The list's
/// maximum size is set by the template parameter.
///
/// @param[in] inner...  Domains that produces `IntrusiveList<T>::Item`s.
///
/// @retval    Domain that produces `ScopedList<T, kMaxSize>`s.
template <typename T,
          size_t kMaxSize,
          int&... ExplicitArgumentBarrier,
          typename... Inner>
auto ScopedListOf(Inner... inner) {
  return MapToScopedList<T, kMaxSize>(
      VectorOf<kMaxSize>(TupleOf(std::move(inner)...)));
}

////////////////////////////////////////////////////////////////
// pw_string-related types

/// Returns a FuzzTest domain that produces @cpp_class{pw::InlineBasicString}s.
///
/// Use this in place of `fuzztest::StringOf`. The characters of the string
/// are drawn from the given domain. The string capacity is given by the
/// template parameter.
///
/// Alternatively, you can use `Arbitrary<InlineString<kCapacity>>`.
///
/// @param[in] inner  Domain that produces values of a character type.
///
/// @retval    Domain that produces `InlineBasicString<kCapacity>`s.
template <size_t kCapacity, int&... ExplicitArgumentBarrier, typename Inner>
auto StringOf(Inner inner) {
  return ContainerOf<InlineBasicString<typename Inner::value_type, kCapacity>>(
             inner)
      .WithMaxSize(kCapacity);
}

/// Implementation of @cpp_func{pw::fuzzer::Arbitrary} for
/// @cpp_class{pw::InlineBasicString}.
template <typename T, size_t kCapacity>
struct ArbitraryImpl<InlineBasicString<T, kCapacity>> {
  auto operator()() { return StringOf<kCapacity>(Arbitrary<T>()); }
};

/// Returns a FuzzTest domain that produces @cpp_class{pw::InlineString}s.
///
/// Use this in place of `fuzztest::String`. The string capacity is given by the
/// template parameter.
///
/// @retval    Domain that produces `InlineString<kCapacity>`s.
template <size_t kCapacity>
auto String() {
  return StringOf<kCapacity>(Arbitrary<char>());
}

/// Returns a FuzzTest domain that produces @cpp_class{pw::InlineString}s
/// containing only ASCII characters.
///
/// Use this in place of `fuzztest::AsciiString`. The string capacity is given
/// by the template parameter.
///
/// @retval    Domain that produces `InlineString<kCapacity>`s.
template <size_t kCapacity>
auto AsciiString() {
  return StringOf<kCapacity>(AsciiChar());
}

/// Returns a FuzzTest domain that produces @cpp_class{pw::InlineString}s
/// containing only printable ASCII characters.
///
/// Use this in place of `fuzztest::PrintableAsciiString`. The string capacity
/// is given by the template parameter.
///
/// @retval    Domain that produces printable `InlineString<kCapacity>`s.
template <size_t kCapacity>
auto PrintableAsciiString() {
  return StringOf<kCapacity>(PrintableAsciiChar());
}

}  // namespace pw::fuzzer
