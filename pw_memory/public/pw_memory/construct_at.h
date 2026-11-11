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

#if __has_include(<version>)
#include <version>
#endif  // __has_include(<version>)

// `<version>` advertises `std::construct_at` via
// `__cpp_lib_constexpr_dynamic_alloc`. Under `-ffreestanding`, libstdc++ 13+
// omits `__cpp_lib_constexpr_dynamic_alloc` (because `std::allocator` is
// hosted-only) while still providing `std::construct_at` gated on the
// compiler's `__cpp_constexpr_dynamic_alloc`.
#if (defined(__cpp_lib_constexpr_dynamic_alloc) &&          \
     __cpp_lib_constexpr_dynamic_alloc >= 201907L) ||       \
    (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 13 && \
     defined(__cpp_constexpr_dynamic_alloc) &&              \
     __cpp_constexpr_dynamic_alloc >= 201907L)

#include <memory>

namespace pw {

using std::construct_at;

}  // namespace pw

#else

#include <new>
#include <type_traits>
#include <utility>

namespace pw {

/// @module{pw_memory}

/// Constructs a `T` object initialized with `args` at `location` without
/// allocating memory.
///
/// Backports C++20's `std::construct_at` so C++17-compatible code can perform
/// `constexpr` in-place construction when compiled as C++20 or newer.
///
/// @tparam T The non-cv-qualified, non-array object type to construct.
/// @tparam Args Argument types forwarded to `T`'s constructor.
/// @param[out] location Pointer to uninitialized storage suitable for `T`.
/// @param[in] args Arguments forwarded to `T`'s constructor.
/// @returns `location`.
template <
    typename T,
    typename... Args,
    typename = std::enable_if_t<!std::is_array_v<T> || std::extent_v<T> != 0>,
    typename = decltype(::new (std::declval<void*>())
                            T(std::declval<Args>()...))>
T* construct_at(T* location, Args&&... args) {
  return ::new (static_cast<void*>(location)) T(std::forward<Args>(args)...);
}

/// @endmodule

}  // namespace pw

#endif  // __cpp_lib_constexpr_dynamic_alloc
