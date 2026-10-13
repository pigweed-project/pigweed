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
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_span/span.h"
#include "pw_status/status_with_size.h"

namespace pw::random {

/// @module{pw_random}

/// A random generator uses injected entropy to generate random values. Many of
/// the guarantees for this interface are provided at the level of the
/// implementations. In general:
///  * DO assume a generator will always succeed.
///  * DO NOT assume a generator is cryptographically secure.
///  * DO NOT assume uniformity of generated data.
class RandomGenerator {
 public:
  virtual ~RandomGenerator() = default;

  template <class T>
  void GetInt(T& dest) {
    static_assert(std::is_integral<T>::value,
                  "Use Get() for non-integral types");
    Get({reinterpret_cast<std::byte*>(&dest), sizeof(T)});
  }

  /// Calculates a uniformly distributed random number in the range
  /// `[0, exclusive_upper_bound)`.
  ///
  /// This avoids modulo biasing. Uniformity is only guaranteed if the
  /// underlying generator generates uniform data. Uniformity is achieved by
  /// generating new random numbers until one is generated in the desired
  /// range (with optimizations).
  ///
  /// @param[out] dest The destination to populate the random number into.
  ///
  /// @param[in] exclusive_upper_bound The largest number that can be
  /// populated into `dest`, exclusive.
  template <class T>
  void GetInt(T& dest, const T& exclusive_upper_bound) {
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer");
    PW_ASSERT(exclusive_upper_bound != 0);

    if (exclusive_upper_bound < 2) {
      dest = 0;
      return;
    }

    const uint8_t leading_zeros_in_upper_bound =
        CountLeadingZeros(exclusive_upper_bound);

    // Create a mask that discards the higher order bits of the random number.
    const T mask =
        std::numeric_limits<T>::max() >> leading_zeros_in_upper_bound;

    // This loop should end fairly soon. It discards all the values that aren't
    // below exclusive_upper_bound. The probability of values being greater or
    // equal than exclusive_upper_bound is less than 1/2, which means that the
    // expected amount of iterations is less than 2.
    while (true) {
      GetInt(dest);
      dest &= mask;
      if (dest < exclusive_upper_bound) {
        return;
      }
    }
  }

  /// Populates the destination buffer with a randomly generated value.
  ///
  /// @param[out] dest The destination buffer.
  virtual void Get(ByteSpan dest) = 0;

  /// Injects entropy into the pool.
  ///
  /// @param[in] data Up to 32 bits of random entropy data.
  ///
  /// @param[in] num_bits The number of bits of entropy. If less than `32`,
  /// entropy is assumed to be stored in the least significant bits of `data`.
  virtual void InjectEntropyBits(uint32_t data, uint_fast8_t num_bits) = 0;

  /// Injects entropy into the pool byte-by-byte.
  void InjectEntropy(ConstByteSpan data) {
    for (std::byte b : data) {
      InjectEntropyBits(std::to_integer<uint32_t>(b), /*num_bits=*/8);
    }
  }

 private:
  template <class T>
  uint8_t CountLeadingZeros(T value) {
    if constexpr (std::is_same_v<T, unsigned>) {
      return static_cast<uint8_t>(__builtin_clz(value));
    } else if constexpr (std::is_same_v<T, unsigned long>) {
      return static_cast<uint8_t>(__builtin_clzl(value));
    } else if constexpr (std::is_same_v<T, unsigned long long>) {
      return static_cast<uint8_t>(__builtin_clzll(value));
    } else {
      static_assert(sizeof(T) < sizeof(unsigned));
      // __builtin_clz returns the count of leading zeros in an unsigned , so we
      // need to subtract the size difference of T in bits.
      return static_cast<uint8_t>(
          __builtin_clz(value) -
          static_cast<int>((sizeof(unsigned) - sizeof(T)) * CHAR_BIT));
    }
  }
};

}  // namespace pw::random
