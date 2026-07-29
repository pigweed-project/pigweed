// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_STDCOMPAT_MEMORY_H_
#define LIB_STDCOMPAT_MEMORY_H_

#include <memory>
#include <type_traits>

#include "version.h"

namespace cpp20 {

#if defined(__cpp_lib_to_address) && __cpp_lib_to_address >= 201711L && \
    !defined(LIB_STDCOMPAT_USE_POLYFILLS)

using std::to_address;

#else  // Provide to_address polyfill.

namespace internal {

// C++17 compatible trait to detect if T has operator->().
template <typename T, typename = void>
struct has_operator_arrow : std::false_type {};

template <typename T>
struct has_operator_arrow<T, std::void_t<decltype(std::declval<const T&>().operator->())>>
    : std::true_type {};

}  // namespace internal

template <typename T>
constexpr T* to_address(T* pointer) noexcept {
  static_assert(!std::is_function<T>::value, "Cannot pass function pointers to std::to_address()");
  return pointer;
}

template <typename T, typename = std::enable_if_t<!std::is_pointer_v<T>>>
constexpr auto to_address(const T& pointer) noexcept {
  if constexpr (internal::has_operator_arrow<T>::value) {
    // The strict check to prevent operator chaining (https://fxbug.dev/42149777).
    // Using std::is_pointer_v makes the polyfill resilient to the metadata
    // changes in the new LLVM rollout (where pointer_traits::element_type
    // no longer matches the return type of __wrap_iter::operator->).
    static_assert(std::is_pointer_v<decltype(pointer.operator->())>,
                  "For compatibility with libc++ and libstdc++, operator->() must return "
                  "a raw pointer. 'Chaining' operator->() in cpp20::to_address() will "
                  "not be permitted until https://fxbug.dev/42149777 is resolved.");

    return pointer.operator->();
  } else {
    return std::addressof(*pointer);
  }
}

#endif  // __cpp_lib_to_address >= 201711L && !defined(LIB_STDCOMPAT_USE_POLYFILLS)

}  // namespace cpp20

#endif  // LIB_STDCOMPAT_MEMORY_H_
