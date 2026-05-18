// Copyright 2022 The Pigweed Authors
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

// pw::span is an implementation of std::span for C++17 or newer. The
// implementation is shared with the std::span polyfill class.
#pragma once

#include "pw_span/internal/config.h"

#if __has_include(<version>)
#include <version>
#endif  // __has_include(<version>)

// If the C++ library fully supports <span>, pw::span is an alias of std::span,
// but only if PW_SPAN_ENABLE_ASSERTS is not enabled.
//
// Avoid std::span for libc++ versions older than 16 (e.g. Clang 15) because of
// incomplete C++20 ranges support. Older libc++ versions fall back to C++17
// container deduction guides which incorrectly deduce span<char> instead of
// span<const char> when passed a non-const std::string_view lvalue. pw::span
// has a more robust deduction guide that correctly deduces const char.
#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L && \
    !PW_SPAN_ENABLE_ASSERTS &&                              \
    (!defined(_LIBCPP_VERSION) || _LIBCPP_VERSION >= 16000)

#include <span>

namespace pw {

using std::as_bytes;
using std::as_writable_bytes;
using std::dynamic_extent;
using std::span;

}  // namespace pw

#else

// If std::span is not available, use Pigweed's span implementation.
#include "pw_span/internal/span_impl.h"  // IWYU pragma: export

#endif  // defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
