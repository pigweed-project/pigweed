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

#include "pw_allocator/fragmentation.h"

#include <cmath>
#include <cstddef>
#include <tuple>

#include "pw_assert/check.h"
#include "pw_numeric/checked_arithmetic.h"

namespace pw::allocator {
namespace {

/// Adds the second number to the first, and returns 1 if it overflowed, else 0.
size_t AddTo(size_t& accumulate, size_t value) {
  accumulate += value;
  return accumulate < value ? 1 : 0;
}

/// Subtracts the second number from the first, and returns 1 if it underflowed,
/// otherwise returns 0.
size_t SubFrom(size_t& accumulate, size_t value) {
  size_t borrow = accumulate < value ? 1 : 0;
  accumulate -= value;
  return borrow;
}

/// Returns `value * value` as two words that represent the more and less
/// significant bits of the product.
std::tuple<size_t, size_t> Square(size_t value) {
  constexpr size_t kShift = sizeof(size_t) * 4;
  constexpr size_t kMask = (size_t(1) << kShift) - 1;
  size_t hi = value >> kShift;
  size_t lo = value & kMask;
  size_t crossterm = hi * lo;
  hi = hi * hi;
  lo = lo * lo;
  hi += (AddTo(crossterm, crossterm)) << kShift;
  hi += (crossterm >> kShift) + AddTo(lo, (crossterm & kMask) << kShift);
  return std::tuple(hi, lo);
}

}  // namespace

void Fragmentation::AddFragment(size_t size) {
  auto [hi, lo] = Square(size);
  sum_of_squares.hi += (hi + AddTo(sum_of_squares.lo, lo));
  sum += size;
}

void Fragmentation::SubtractFragment(size_t size) {
  auto [hi, lo] = Square(size);
  sum_of_squares.hi -= (hi + SubFrom(sum_of_squares.lo, lo));
  sum -= size;
}

Fragmentation& Fragmentation::operator+=(const Fragmentation& other) {
  sum_of_squares.hi += AddTo(sum_of_squares.lo, other.sum_of_squares.lo);
  PW_CHECK(CheckedIncrement(sum_of_squares.hi, other.sum_of_squares.hi));
  PW_CHECK(CheckedIncrement(sum, other.sum));
  return *this;
}

Fragmentation& Fragmentation::operator-=(const Fragmentation& other) {
  sum_of_squares.hi -= (other.sum_of_squares.hi +
                        SubFrom(sum_of_squares.lo, other.sum_of_squares.lo));
  sum -= other.sum;
  return *this;
}

float CalculateFragmentation(const Fragmentation& fragmentation) {
  auto sum_of_squares = static_cast<float>(fragmentation.sum_of_squares.hi);
  if (sum_of_squares != 0) {
    sum_of_squares *= std::pow(2.f, sizeof(size_t) * 8.f);
  }
  sum_of_squares += static_cast<float>(fragmentation.sum_of_squares.lo);
  return 1.f -
         (std::sqrt(sum_of_squares) / static_cast<float>(fragmentation.sum));
}

}  // namespace pw::allocator
