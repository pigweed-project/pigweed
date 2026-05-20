// Copyright 2020 The Pigweed Authors
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

// Macros for using C++ features in older standards.
#pragma once

/// @module{pw_polyfill}

/// Mark functions as `constexpr` if compiling for C++20 or newer. In C++17,
/// `PW_CONSTEXPR_CPP20` expands to nothing.
///
/// This is primarily used for functions that rely on standard library functions
/// that only became `constexpr` in C++20 (e.g. `std::copy`). Use with caution
/// in portable code; if `constexpr` is required in C++17, use `constexpr`.
#if __cplusplus >= 202002L
#define PW_CONSTEXPR_CPP20 constexpr
#else
#define PW_CONSTEXPR_CPP20
#endif  // __cpp_constexpr >= 201304L

/// Mark functions as `consteval` if supported (C++20), or `constexpr` if not
/// (C++17).
///
/// Use with caution in portable code. Calling a `consteval` function outside
/// of a constant expression is an error.
#if defined(__cpp_consteval) && __cpp_consteval >= 201811L
#define PW_CONSTEVAL consteval
#else
#define PW_CONSTEVAL constexpr
#endif  // __cpp_consteval >= 201811L

/// Declare a variable as `constinit`. Requires compiler-specific features if
/// `constinit` is not available.
#if defined(__cpp_constinit) && __cpp_constinit >= 201907L
#define PW_CONSTINIT constinit
#elif defined(__clang__)
#define PW_CONSTINIT [[clang::require_constant_initialization]]
#elif defined(__GNUC__) && __GNUC__ >= 10
#define PW_CONSTINIT __constinit
#else
#define PW_CONSTINIT                                                \
  static_assert(false,                                              \
                "PW_CONSTINIT does not yet support this compiler; " \
                "implement PW_CONSTINIT for this compiler to use it.");
#endif  // __cpp_constinit

/// Provides `[[nodiscard]]` with a string literal description, which is only
/// available starting in C++20.
#if __cplusplus >= 202002L
#define PW_NODISCARD_STR(str) [[nodiscard(str)]]
#else
#define PW_NODISCARD_STR(str) [[nodiscard]]
#endif  // __cplusplus >= 202002L

/// @endmodule
