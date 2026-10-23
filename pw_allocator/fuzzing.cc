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

#include "pw_allocator/fuzzing.h"

#include <climits>
#include <cstddef>

#include "pw_allocator/test_harness.h"
#include "pw_fuzzer/fuzztest.h"

namespace pw::allocator::test {
namespace {

auto ArbitrarySize(size_t max_size) {
  return fuzzer::InRange<size_t>(0, max_size);
}

auto ArbitraryAlignment(size_t size) {
  return fuzzer::Map(
      [size](size_t lshift) { return AlignmentFromLShift(lshift, size); },
      fuzzer::Arbitrary<size_t>());
}

auto ArbitraryIndex() { return fuzzer::Arbitrary<size_t>(); }

fuzzer::Domain<AllocationRequest> ArbitraryAllocationRequest(size_t max_size) {
  auto from_size = [](size_t size) {
    return fuzzer::StructOf<AllocationRequest>(fuzzer::Just(size),
                                               ArbitraryAlignment(size));
  };
  return fuzzer::FlatMap(from_size, ArbitrarySize(max_size));
}

fuzzer::Domain<DeallocationRequest> ArbitraryDeallocationRequest() {
  return fuzzer::StructOf<DeallocationRequest>(ArbitraryIndex());
}

fuzzer::Domain<ReallocationRequest> ArbitraryReallocationRequest(
    size_t max_size) {
  return fuzzer::StructOf<ReallocationRequest>(ArbitraryIndex(),
                                               ArbitrarySize(max_size));
}

}  // namespace

fuzzer::Domain<Request> ArbitraryRequest(size_t max_size) {
  return fuzzer::VariantOf(ArbitraryAllocationRequest(max_size),
                           ArbitraryDeallocationRequest(),
                           ArbitraryReallocationRequest(max_size));
}

}  // namespace pw::allocator::test
