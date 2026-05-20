// Copyright 2025 The Pigweed Authors
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

#include "pw_bytes/alignment.h"
#include "pw_span/internal/config.h"
#include "pw_span/span.h"

namespace pw {
namespace internal {

template <class ResultT, size_t kSourceExtentBytes>
using SpanFromBytes = span<ResultT,
                           (kSourceExtentBytes == dynamic_extent
                                ? dynamic_extent
                                : kSourceExtentBytes / sizeof(ResultT))>;

}  // namespace internal

/// @module{pw_span}

/// @defgroup pw_span_cast Cast
/// @{

/// Casts a `pw::span<std::byte>` (`ByteSpan`) to a span of a different type.
///
/// This function is only safe to use if the underlying data is actually of the
/// specified type. You cannot safely use this function to reinterpret e.g. a
/// raw byte array from `malloc()` as a span of integers.
///
/// This function is essentially the inverse of `pw::as_writable_bytes`.
///
/// If `kSourceExtentBytes` is `dynamic_extent`, the returned span also has a
/// dynamic extent.  Otherwise, the returned span has a static extent of
/// `kSourceExtentBytes / sizeof(ResultT)`.
///
/// @tparam  ResultT             The type of the returned span.
///                              Must be one byte to avoid misuse and violation
///                              of the strict aliasing rule. This restriction
///                              might be lifted in the future.
/// @tparam  kSourceExtentBytes  The extent of the source byte span. This is
///                              normally inferred and need not be explicitly
///                              provided.
template <class ResultT, size_t kSourceExtentBytes>
internal::SpanFromBytes<ResultT, kSourceExtentBytes> span_cast(
    span<std::byte, kSourceExtentBytes> bytes) {
  static_assert(sizeof(ResultT) == 1);

  ResultT* const ptr = reinterpret_cast<ResultT*>(bytes.data());
  const size_t count = bytes.size() / sizeof(ResultT);

  auto result =
      internal::SpanFromBytes<ResultT, kSourceExtentBytes>{ptr, count};

  _PW_SPAN_ASSERT(IsAlignedAs<ResultT>(result.data()));
  _PW_SPAN_ASSERT(result.size_bytes() == bytes.size_bytes());

  return result;
}

// TODO: https://pwbug.dev/396493663 - Doxygen thinks this is the same function
// as the non-const version above, and merges the docs together.

/// Casts a `pw::span<const std::byte>` (`ConstByteSpan`) to a span of a
/// different const type.
///
/// This function is only safe to use if the underlying data is actually of the
/// specified type. You cannot safely use this function to reinterpret e.g. a
/// raw byte array from `malloc()` as a span of integers.
///
/// This function is essentially the inverse of `pw::as_bytes`.
///
/// If `kSourceExtentBytes` is `dynamic_extent`, the returned span also has a
/// dynamic extent.  Otherwise, the returned span has a static extent of
/// `kSourceExtentBytes / sizeof(ResultT)`.
///
/// @tparam  ResultT             The type of the returned span.
///                              Must be one byte to avoid misuse and violation
///                              of the strict aliasing rule. This restriction
///                              might be lifted in the future.
/// @tparam  kSourceExtentBytes  The extent of the source byte span. This is
///                              normally inferred and need not be explicitly
///                              provided.
template <class ResultT, size_t kSourceExtentBytes>
internal::SpanFromBytes<const ResultT, kSourceExtentBytes> span_cast(
    span<const std::byte, kSourceExtentBytes> bytes) {
  static_assert(sizeof(ResultT) == 1);

  const ResultT* const ptr = reinterpret_cast<const ResultT*>(bytes.data());
  const size_t count = bytes.size() / sizeof(ResultT);

  auto result =
      internal::SpanFromBytes<const ResultT, kSourceExtentBytes>{ptr, count};

  _PW_SPAN_ASSERT(IsAlignedAs<const ResultT>(result.data()));
  _PW_SPAN_ASSERT(result.size_bytes() == bytes.size_bytes());

  return result;
}

/// @}
/// @endmodule

}  // namespace pw
