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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace pw {

class HashState;

/// The "Extension Point" for custom types.
///
/// Users should define a function named `PwHashValue` in the same namespace as
/// their custom type. The `pw::Hash` system will find it via Argument Dependent
/// Lookup (ADL).
///
/// We delete the primary template to ensure that only valid overloads (found
/// via ADL) are ever called.
template <typename H, typename T>
H PwHashValue(H h, const T& val) = delete;

namespace hash_impl {

template <typename H, typename T, typename = void>
struct HasPwHashValue : std::false_type {};

template <typename H, typename T>
struct HasPwHashValue<H,
                      T,
                      std::void_t<decltype(PwHashValue(
                          std::declval<H>(), std::declval<const T&>()))>>
    : std::is_same<decltype(PwHashValue(std::declval<H>(),
                                        std::declval<const T&>())),
                   H> {};

template <typename H, typename T>
inline constexpr bool kHasPwHashValue = HasPwHashValue<H, T>::value;

template <typename T, typename = void>
struct HasStdHash : std::false_type {};

template <typename T>
struct HasStdHash<T,
                  std::void_t<decltype(std::declval<std::hash<T>>()(
                      std::declval<const T&>()))>>
    : std::is_same<decltype(std::declval<std::hash<T>>()(
                       std::declval<const T&>())),
                   size_t> {};

template <typename T>
inline constexpr bool kHasStdHash = HasStdHash<T>::value;

// Dispatches the hashing logic based on the type traits detected above.
// Priority:
// 1. Custom `PwHashValue` (User-defined extension)
// 2. Pointers (Hashed by address)
// 3. Enums (Hashed by underlying integer value)
// 4. Standard `std::hash` (Fallback for int, float, etc.)
template <typename H, typename T>
H hash_value_dispatcher(H h, const T& val) {
  if constexpr (kHasPwHashValue<H, T>) {
    return PwHashValue(std::move(h), val);
  } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
    return H::mix(std::move(h), reinterpret_cast<uintptr_t>(val));
  } else if constexpr (std::is_enum_v<T>) {
    return H::mix(
        std::move(h),
        static_cast<size_t>(static_cast<std::underlying_type_t<T>>(val)));
  } else {
    static_assert(kHasStdHash<T>,
                  "The type must be hashable by either PwHashValue, std::hash, "
                  "or a pointer/enum.");
    return H::mix(std::move(h), std::hash<T>()(val));
  }
}

}  // namespace hash_impl

/// A stateful helper class for accumulating and mixing hash values.
///
/// `HashState` encapsulates the bitwise operations required to combine multiple
/// values into a single hash. It uses move semantics to enforce a linear chain
/// of operations, ensuring that intermediate states are not accidentally
/// reused.
class HashState {
 public:
  /// Initializes the hash state with a seed.
  /// @param seed An initial value to randomize the hash. Defaults to 17.
  explicit HashState(size_t seed = 17) : state_(seed) {}

  HashState(const HashState&) = delete;
  HashState& operator=(const HashState&) = delete;
  HashState(HashState&&) = default;
  HashState& operator=(HashState&&) = default;

  /// Mixes a raw integer value into the current hash state.
  ///
  /// This implementation is based on `boost::hash_combine`.
  ///
  /// @param h The current state (passed by move).
  /// @param val The raw integer value to mix in.
  /// @return The new updated HashState.
  static HashState mix(HashState&& h, size_t val) {
    h.state_ ^= val + 0x9e3779b9 + (h.state_ << 6) + (h.state_ >> 2);
    return std::move(h);
  }

  /// Combines a single value into the hash state.
  ///
  /// This function acts as the gateway to the `hash_value_dispatcher`. It
  /// automatically detects if `val` should be hashed via a custom `PwHashValue`
  /// function, as a pointer, or via `std::hash`.
  template <typename T>
  static HashState combine(HashState&& h, const T& val) {
    h = hash_impl::hash_value_dispatcher(std::move(h), val);
    return std::move(h);
  }

  /// Recursively combines multiple values into the hash state.
  ///
  /// This is the primary utility for users implementing `PwHashValue`.
  /// It allows you to hash all members of a struct in a single call.
  ///
  /// Example:
  /// @code
  ///   class MyType {
  ///    public:
  ///     // Defined as an inline friend (Hidden Friend idiom).
  ///     friend HashState PwHashValue(HashState&& h, const MyType& v) {
  ///       return HashState::combine(std::move(h), v.x_, v.y_, v.z_);
  ///     }
  ///
  ///    private:
  ///     int x_, y_, z_;
  ///   };
  /// @endcode
  template <typename T, typename... Ts>
  static HashState combine(HashState&& h, const T& first, const Ts&... rest) {
    h = combine(std::move(h), first);
    if constexpr (sizeof...(rest) > 0) {
      return combine(std::move(h), rest...);
    }
    return std::move(h);
  }

  /// Combines a contiguous range of elements into the hash state.
  ///
  /// This is intended for arrays or buffers where the order of elements
  /// matters.
  ///
  /// @note This implementation currently iterates through the range and
  /// combines elements one-by-one. It is NOT yet optimized for bulk
  /// hashing (e.g., using CRC32 or SIMD) even for trivially copyable
  /// types like `uint8_t`.
  ///
  /// @param h The current state (passed by move).
  /// @param data A pointer to the first element in the range.
  /// @param size The number of elements to hash.
  /// @return The updated HashState.
  template <typename T>
  static HashState combine_contiguous(HashState&& h,
                                      const T* data,
                                      size_t size) {
    for (size_t i = 0; i < size; i++) {
      h = combine(std::move(h), data[i]);
    }
    return std::move(h);
  }

  /// Combines a range of elements where the order does not affect the output.
  ///
  /// This is intended for unordered containers like sets or maps. It works
  /// by hashing each element independently and combining them using
  /// commutative operations (addition), ensuring that `{A, B}` results
  /// in the same hash as `{B, A}`.
  ///
  /// @param h The current state (passed by move).
  /// @param first The starting iterator of the range.
  /// @param last The ending iterator of the range.
  /// @return The updated HashState including the combined range and its size.
  template <typename InputIt>
  static HashState combine_unordered(HashState&& h,
                                     InputIt first,
                                     InputIt last) {
    size_t total = 0;
    size_t count = 0;
    for (auto it = first; it != last; ++it) {
      // High-entropy seed (Golden Ratio) prevents zero-swallowing
      total += combine(HashState(0x9E3779B9), *it).finalize();
      ++count;
    }
    return HashState::combine(std::move(h), total, count);
  }

  /// Finalizes the hash calculation and returns the result.
  size_t finalize() { return state_; }

 private:
  size_t state_;
};

struct Hash {
  size_t seed_;
  constexpr explicit Hash(size_t seed = 17) : seed_(seed) {}

  template <typename T>
  constexpr size_t operator()(const T& value) const {
    return hash_impl::hash_value_dispatcher(HashState(seed_), value).finalize();
  }
};

// Performs comparisons with operator==, similar to C++14's `std::equal_to<>`.
struct EqualTo {
  template <typename T, typename U>
  constexpr bool operator()(const T& lhs, const U& rhs) const {
    return lhs == rhs;
  }
};

}  // namespace pw
