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

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/type_traits.h"
#include "pw_containers/internal/tag.h"
#include "pw_polyfill/language_feature_macros.h"

// C++20 `std::construct_at` enables `constexpr` in-place construction (unlike
// placement `new`). On freestanding libstdc++ 13+, `std::construct_at` is
// available via `__cpp_constexpr_dynamic_alloc` even though
// `__cpp_lib_constexpr_dynamic_alloc` is omitted.
#if __cplusplus >= 202002L &&                                \
    (defined(__cpp_lib_constexpr_dynamic_alloc) ||           \
     (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 13 && \
      defined(__cpp_constexpr_dynamic_alloc)))
#define _PW_CONTAINERS_CONSTEXPR_CONSTRUCT_AT 1
#else
#define _PW_CONTAINERS_CONSTEXPR_CONSTRUCT_AT 0
#endif

namespace pw::containers::internal {

// Constructs an object of type `T` in-place at `location`. Uses
// `std::construct_at` in C++20+ (enabling `constexpr` evaluation) and global
// placement `::new` in C++17.
template <typename T, typename... Args>
inline PW_CONSTEXPR_CPP20 T& ConstructAt(T& location, Args&&... args) noexcept {
#if _PW_CONTAINERS_CONSTEXPR_CONSTRUCT_AT
  return *std::construct_at(std::addressof(location),
                            std::forward<Args>(args)...);
#else
  return *::new (const_cast<void*>(static_cast<const volatile void*>(
      std::addressof(location)))) T(std::forward<Args>(args)...);
#endif  // _PW_CONTAINERS_CONSTEXPR_CONSTRUCT_AT
}

// Untagged union storage for an optional element of type `T`, preserving a
// trivial destructor when `T` is trivially destructible.
template <typename T, bool kTrivialDtor = std::is_trivially_destructible_v<T>>
union MaybeImpl;

#define _PW_CONTAINERS_MAYBE_BODY()                                            \
  /* For trivially default-constructible types, prefer default-constructing */ \
  /* the value over the empty state to avoid -Wmaybe-uninitialized warnings */ \
  /* when building with GCC.                                                */ \
  template <                                                                   \
      typename U = T,                                                          \
      std::enable_if_t<std::is_trivially_default_constructible_v<U>, int> = 0> \
  explicit constexpr MaybeImpl(NullPlaceholder) noexcept : value{} {}          \
                                                                               \
  template <typename U = T,                                                    \
            std::enable_if_t<!std::is_trivially_default_constructible_v<U>,    \
                             int> = 0>                                         \
  explicit constexpr MaybeImpl(NullPlaceholder) noexcept : none{} {}           \
                                                                               \
  template <                                                                   \
      typename Arg,                                                            \
      typename... Rest,                                                        \
      std::enable_if_t<                                                        \
          !(sizeof...(Rest) == 0 &&                                            \
            (std::is_same_v<cpp20::remove_cvref_t<Arg>, MaybeImpl> ||          \
             std::is_same_v<cpp20::remove_cvref_t<Arg>, NullPlaceholder>)) &&  \
              std::is_constructible_v<T, Arg, Rest...>,                        \
          int> = 0>                                                            \
  constexpr MaybeImpl(Arg&& arg, Rest&&... rest) noexcept                      \
      : value(std::forward<Arg>(arg), std::forward<Rest>(rest)...) {}          \
                                                                               \
  constexpr MaybeImpl(const MaybeImpl&) = default;                             \
  constexpr MaybeImpl(MaybeImpl&&) = default;                                  \
  constexpr MaybeImpl& operator=(const MaybeImpl&) = default;                  \
  constexpr MaybeImpl& operator=(MaybeImpl&&) = default;                       \
                                                                               \
  template <typename... Args>                                                  \
  PW_CONSTEXPR_CPP20 T& Construct(Args&&... args) noexcept {                   \
    return ConstructAt(value, std::forward<Args>(args)...);                    \
  }                                                                            \
                                                                               \
  Empty none;                                                                  \
  T value

template <typename T>
union MaybeImpl<T, true> {
  _PW_CONTAINERS_MAYBE_BODY();
  ~MaybeImpl() = default;
};

template <typename T>
union MaybeImpl<T, false> {
  _PW_CONTAINERS_MAYBE_BODY();
  PW_CONSTEXPR_CPP20 ~MaybeImpl() noexcept {}
};

#undef _PW_CONTAINERS_MAYBE_BODY
#undef _PW_CONTAINERS_CONSTEXPR_CONSTRUCT_AT

template <typename T>
using Maybe = MaybeImpl<T>;

}  // namespace pw::containers::internal
