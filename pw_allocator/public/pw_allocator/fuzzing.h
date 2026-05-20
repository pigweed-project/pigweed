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

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_allocator/test_harness.h"
#include "pw_fuzzer/fuzztest.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test_fuzz}

/// Returns a FuzzTest domain for producing arbitrary allocator requests.
///
/// This method integrates with FuzzTest to use code coverage to produce guided
/// mutations.
///
/// See https://github.com/google/fuzztest/blob/main/doc/domains-reference.md
///
/// @param  max_size  Size of the largest allocation that can be requested.
fuzzer::Domain<Request> ArbitraryRequest(size_t max_size);

/// Returns a FuzzTest domain for producing sequences of arbitrary allocator
/// requests.
///
/// This method can be used to drive an `AllocatorTestHarness` as part of a
/// fuzz test.
///
/// See https://github.com/google/fuzztest/blob/main/doc/domains-reference.md
///
/// @tparam kMaxRequests  Maximum number of requests in the sequence.
/// @tparam kMaxSize      Size of the largest allocation that can be requested.
template <size_t kMaxRequests, size_t kMaxSize>
auto ArbitraryRequests() {
  return fuzzer::VectorOf<kMaxRequests>(ArbitraryRequest(kMaxSize));
}

/// Builds an Request from an index and values.
///
/// Unfortunately, the reproducer emitted by FuzzTest for vectors of
/// Requests cannot simply be copied and pasted. To create a
/// reproducer, create a `pw::Vector` of the appropriate size, and populate it
/// using this method with the correct index.
///
/// For example, consider the following sample output:
/// @code
/// The test fails with input:
/// argument 0: {(index=0, value={0, 1}), (index=0, value={1209, 8}),
/// (index=2, value={9223372036854775807, 2048})}
///
/// =================================================================
/// === Reproducer test
///
/// TEST(MyAllocatorFuzzTest, NeverCrashesU16Regression) {
///   NeverCrashes(
///     {{0, 1}, {1209, 8}, {9223372036854775807, 2048},
///   );
/// }
/// @endcode
///
/// A valid reproducer might be:
/// @code{.cpp}
/// TEST(MyAllocatorFuzzTest, NeverCrashesU16Regression) {
///   Vector<test::Request, 3> input({
///     test::MakeRequest<0>(0u, 1u),
///     test::MakeRequest<0>(1209u, 8u),
///     test::MakeRequest<2>(9223372036854775807u, 2048u),
///   });
///   NeverCrashes(input);
// }
/// @endcode
template <size_t kIndex, typename... Args>
Request MakeRequest(Args... args) {
  if constexpr (kIndex == 0) {
    return AllocationRequest{static_cast<size_t>(args)...};
  }
  if constexpr (kIndex == 1) {
    return DeallocationRequest{static_cast<size_t>(args)...};
  }
  if constexpr (kIndex == 2) {
    return ReallocationRequest{static_cast<size_t>(args)...};
  }
  return Request();
}

inline constexpr size_t kMaxRequests = 256;
inline constexpr size_t kMaxSize = 2048;

inline auto DefaultArbitraryRequests() {
  return pw::allocator::test::ArbitraryRequests<kMaxRequests, kMaxSize>();
}

/// @endsubmodule

}  // namespace pw::allocator::test
