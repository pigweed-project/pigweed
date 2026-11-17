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
#pragma once

#include <cstddef>
#include <type_traits>

#include "pw_preprocessor/apply.h"
#include "pw_preprocessor/arguments.h"
#include "pw_preprocessor/compiler.h"

namespace pw::containers::internal {

// Sentinel type returned by `SafeNthType<I, Ts...>` when `I >= sizeof...(Ts)`.
struct InvalidTypeIndex {};

// Sentinel index returned when a type lookup fails or is ambiguous.
inline constexpr size_t kInvalidTypeIndex = static_cast<size_t>(-1);

// Fast O(1) pack indexing using compiler intrinsic on Clang.
// GCC's `__type_pack_element` has substitution bugs with dependent non-type
// template parameters and out-of-bounds SFINAE probes, so GCC uses the 8-way
// chunked `detail::NthTypeImpl` specialization below.
#if defined(__clang__) && PW_HAS_BUILTIN(__type_pack_element)
template <size_t I, typename... Ts>
using NthType = __type_pack_element<I, Ts...>;
#else
namespace detail {

#define _PW_CONTAINERS_TPARAM(i, unused, arg) typename arg
#define _PW_CONTAINERS_COMMA_SEP(i, unused) ,
#define _PW_CONTAINERS_TPARAMS(...) \
  PW_APPLY(_PW_CONTAINERS_TPARAM, _PW_CONTAINERS_COMMA_SEP, , __VA_ARGS__)

#define _PW_CONTAINERS_APPLY_8(macro) \
  macro(T0);                          \
  macro(T0, T1);                      \
  macro(T0, T1, T2);                  \
  macro(T0, T1, T2, T3);              \
  macro(T0, T1, T2, T3, T4);          \
  macro(T0, T1, T2, T3, T4, T5);      \
  macro(T0, T1, T2, T3, T4, T5, T6);  \
  macro(T0, T1, T2, T3, T4, T5, T6, T7)

template <size_t I, bool kLessThan8, typename... Ts>
struct NthTypeImpl {
  using type = InvalidTypeIndex;
};

#define _PW_CONTAINERS_NTH_TYPE(...)                               \
  template <_PW_CONTAINERS_TPARAMS(__VA_ARGS__), typename... Rest> \
  struct NthTypeImpl<PW_MACRO_ARG_COUNT(__VA_ARGS__) - 1,          \
                     true,                                         \
                     __VA_ARGS__,                                  \
                     Rest...> {                                    \
    using type = PW_LAST_ARG(__VA_ARGS__);                         \
  }

_PW_CONTAINERS_APPLY_8(_PW_CONTAINERS_NTH_TYPE);
#undef _PW_CONTAINERS_NTH_TYPE
#undef _PW_CONTAINERS_APPLY_8
#undef _PW_CONTAINERS_TPARAMS
#undef _PW_CONTAINERS_COMMA_SEP
#undef _PW_CONTAINERS_TPARAM

template <size_t I,
          typename T0,
          typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7,
          typename... Rest>
struct NthTypeImpl<I, false, T0, T1, T2, T3, T4, T5, T6, T7, Rest...> {
  using type = typename NthTypeImpl<I - 8, (I - 8 < 8), Rest...>::type;
};

}  // namespace detail

template <size_t I, typename... Ts>
using NthType = typename detail::NthTypeImpl<I, (I < 8), Ts...>::type;
#endif  // defined(__clang__) && PW_HAS_BUILTIN(__type_pack_element)

namespace detail {

template <size_t I, bool kInBounds, typename... Ts>
struct SafeNthTypeImpl {
  using type = InvalidTypeIndex;
};

template <size_t I, typename... Ts>
struct SafeNthTypeImpl<I, true, Ts...> {
  using type = NthType<I, Ts...>;
};

template <typename T, typename... Ts>
constexpr size_t FindUniqueTypeIndex() noexcept {
  if constexpr (sizeof...(Ts) > 0) {
    constexpr bool kMatches[] = {std::is_same_v<T, Ts>...};
    size_t found = kInvalidTypeIndex;
    for (size_t i = 0; i < sizeof...(Ts); ++i) {
      if (kMatches[i]) {
        if (found != kInvalidTypeIndex) {
          return kInvalidTypeIndex;
        }
        found = i;
      }
    }
    return found;
  }
  return kInvalidTypeIndex;
}

}  // namespace detail

// Returns `InvalidTypeIndex` when `I >= sizeof...(Ts)` so SFINAE never
// indexes `__type_pack_element` out of bounds.
template <size_t I, typename... Ts>
using SafeNthType =
    typename detail::SafeNthTypeImpl<I, (I < sizeof...(Ts)), Ts...>::type;

// Non-recursive unique type index lookup. Returns the zero-based index of `T`
// in `Ts...` if `T` occurs exactly once, or `kInvalidTypeIndex` if `T` is
// absent or duplicated.
template <typename T, typename... Ts>
inline constexpr size_t kUniqueTypeIndex =
    detail::FindUniqueTypeIndex<T, Ts...>();

}  // namespace pw::containers::internal
