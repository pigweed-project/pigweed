// Copyright 2024 The Pigweed Authors
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
#include <cstring>
#include <optional>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_multibuf/v1/header_chunk_region_tracker.h"
#include "pw_multibuf/v1/multibuf.h"

namespace pw::multibuf::v1::test_utils {

// Arbitrary size intended to be large enough to store the Chunk and data
// slices. This may be increased if `MakeChunk` or a Chunk-splitting operation
// fails.
inline constexpr size_t kArbitraryAllocatorSize = 2048;
inline constexpr size_t kArbitraryChunkSize = 32;

inline constexpr std::byte kPoisonByte{0x9d};

using ::pw::allocator::test::AllocatorForTest;

inline OwnedChunk MakeChunk(pw::allocator::Allocator& allocator,
                            size_t size,
                            std::byte initializer = std::byte{0}) {
  std::optional<OwnedChunk> chunk =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator, size);
  // If this check fails, `kArbitraryAllocatorSize` above may need increasing.
  PW_CHECK(chunk.has_value());
  std::memset(chunk->data(), static_cast<uint8_t>(initializer), size);
  return std::move(*chunk);
}

inline OwnedChunk MakeChunk(pw::allocator::Allocator& allocator,
                            std::initializer_list<std::byte> data) {
  std::optional<OwnedChunk> chunk =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator, data.size());
  // If this check fails, `kArbitraryAllocatorSize` above may need increasing.
  PW_CHECK(chunk.has_value());
  std::copy(data.begin(), data.end(), (*chunk)->begin());
  return std::move(*chunk);
}

inline OwnedChunk MakeChunk(pw::allocator::Allocator& allocator,
                            pw::span<const std::byte> data) {
  std::optional<OwnedChunk> chunk =
      HeaderChunkRegionTracker::AllocateRegionAsChunk(allocator, data.size());
  // If this check fails, `kArbitraryAllocatorSize` above may need increasing.
  PW_CHECK(chunk.has_value());
  std::copy(data.begin(), data.end(), (*chunk)->begin());
  return std::move(*chunk);
}

template <typename ActualIterable, typename ExpectedIterable>
void ExpectElementsEqual(const ActualIterable& actual,
                         const ExpectedIterable& expected) {
  auto actual_iter = actual.begin();
  auto expected_iter = expected.begin();
  for (; expected_iter != expected.end(); ++actual_iter, ++expected_iter) {
    ASSERT_NE(actual_iter, actual.end());
    EXPECT_EQ(*actual_iter, *expected_iter);
  }
}

template <typename ActualIterable, typename T>
void ExpectElementsEqual(const ActualIterable& actual,
                         std::initializer_list<T> expected) {
  ExpectElementsEqual<ActualIterable, std::initializer_list<T>>(actual,
                                                                expected);
}

template <typename ActualIterable,
          typename T = typename ActualIterable::iterator::value_type>
void ExpectElementsAre(const ActualIterable& actual, T value) {
  auto actual_iter = actual.begin();
  for (; actual_iter != actual.end(); ++actual_iter) {
    ASSERT_NE(actual_iter, actual.end());
    EXPECT_EQ(*actual_iter, value);
  }
}

}  // namespace pw::multibuf::v1::test_utils
