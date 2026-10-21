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

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "lib/stdcompat/type_traits.h"
#include "pw_assert/assert.h"
#include "pw_numeric/integer_division.h"
#include "pw_polyfill/language_feature_macros.h"

namespace pw::thread::internal {

/// @module{pw_thread}

// Number of named priorities (lowest, low, very low, ..., very high, highest).
inline constexpr size_t kNamedPriorities = 9;

// Produces a table that distributes priorities between 0 and the highest value.
// These values are used as offsets when mapping from the native priority type.
template <typename T>
PW_CONSTEVAL std::array<T, kNamedPriorities> PriorityOffsets(T highest) {
  std::array<T, kNamedPriorities> offsets{};

  for (unsigned i = 0; i < kNamedPriorities; ++i) {
    // Divide the offsets into 8 tiers. The highest value is its own tier.
    uint64_t priority_value = IntegerDivisionRoundNearest<uint64_t>(
        static_cast<uint64_t>(highest) * i, kNamedPriorities - 1);
    // The calculated value never exceeds the max.
    offsets[i] = static_cast<T>(priority_value);
  }
  return offsets;
}

// Abstract priority level. Specialized for cases when lower numbers represent
// higher priorities, so that addition and comparisons work in terms of the
// abtract priority rather than the numeric value.
template <typename T, T kLowest, T kHighest, bool = kLowest <= kHighest>
class AbstractLevel {
 public:
  using U = std::make_unsigned_t<T>;

  static constexpr U Range() { return kHighest - kLowest; }

  constexpr AbstractLevel() : n_{} {}
  explicit constexpr AbstractLevel(T value) : n_{value} {}
  constexpr T value() const { return n_; }

  constexpr bool operator==(AbstractLevel rhs) const { return n_ == rhs.n_; }
  constexpr bool operator!=(AbstractLevel rhs) const { return n_ != rhs.n_; }
  constexpr bool operator<(AbstractLevel rhs) const { return n_ < rhs.n_; }
  constexpr bool operator<=(AbstractLevel rhs) const { return n_ <= rhs.n_; }
  constexpr bool operator>(AbstractLevel rhs) const { return n_ > rhs.n_; }
  constexpr bool operator>=(AbstractLevel rhs) const { return n_ >= rhs.n_; }

  constexpr AbstractLevel operator+(U amount) const {
    return AbstractLevel(static_cast<T>(static_cast<U>(n_) + amount));
  }
  constexpr AbstractLevel operator-(U amount) const {
    return AbstractLevel(static_cast<T>(static_cast<U>(n_) - amount));
  }

 private:
  static_assert(kLowest <= kHighest);
  T n_;
};

template <typename T, T kLowest, T kHighest>
struct AbstractLevel<T, kLowest, kHighest, false> {
 public:
  using U = std::make_unsigned_t<T>;

  static constexpr U Range() { return kLowest - kHighest; }

  constexpr AbstractLevel() : n_{} {}
  explicit constexpr AbstractLevel(T value) : n_{value} {}
  constexpr T value() const { return n_; }

  constexpr bool operator==(AbstractLevel rhs) const { return n_ == rhs.n_; }
  constexpr bool operator!=(AbstractLevel rhs) const { return n_ != rhs.n_; }
  constexpr bool operator<(AbstractLevel rhs) const { return n_ > rhs.n_; }
  constexpr bool operator<=(AbstractLevel rhs) const { return n_ >= rhs.n_; }
  constexpr bool operator>(AbstractLevel rhs) const { return n_ < rhs.n_; }
  constexpr bool operator>=(AbstractLevel rhs) const { return n_ <= rhs.n_; }

  constexpr AbstractLevel operator+(U amount) const {
    return AbstractLevel(static_cast<T>(static_cast<U>(n_) - amount));
  }
  constexpr AbstractLevel operator-(U amount) const {
    return AbstractLevel(static_cast<T>(static_cast<U>(n_) + amount));
  }

 private:
  static_assert(kLowest > kHighest);
  T n_;
};

// Produces a table that maps named priority levels to their corresponding
// native priority values.
template <typename T, bool = std::is_enum_v<T>>
struct UnderlyingInteger : public std::underlying_type<T> {};

template <typename T>
struct UnderlyingInteger<T, false> : public cpp20::type_identity<T> {};

/// Generic priority class. `pw::ThreadPriority` instantiates `Priority` with
/// the priority range specified by the backend.
template <typename T, T kLowestPriority, T kHighestPriority, T kDefaultPriority>
class Priority {
 public:
  /// True if the `pw_thread` backend supports more than one priority level.
  static constexpr bool IsSupported() { return Level::Range() != 0u; }

  /// Constructs a priority at the backend-specified default level.
  constexpr Priority() : Priority(kDefault) {}

  constexpr Priority(const Priority&) = default;
  constexpr Priority& operator=(const Priority&) = default;

  /// Returns the lowest priority supported by the backend. The underlying OS
  /// may support lower priorities; backends may only expose a subset of
  /// priorities.
  static constexpr Priority Lowest() { return Priority(kLevels[kLowest]); }

  /// Priority higher than `Lowest`, but lower than `Low`, if possible.
  static constexpr Priority VeryLow() { return Priority(kLevels[kVeryLow]); }

  /// Priority higher than `VeryLow`, but lower than `MediumLow`, if possible.
  static constexpr Priority Low() { return Priority(kLevels[kLow]); }

  /// Priority higher than `Low`, but lower than `Medium`, if possible.
  static constexpr Priority MediumLow() { return Priority(kLevels[kMedLow]); }

  /// Priority higher than `MediumLow`, but lower than `MediumHigh`, if
  /// possible.
  static constexpr Priority Medium() { return Priority(kLevels[kMed]); }

  /// Priority higher than `Medium`, but lower than `High`, if possible.
  static constexpr Priority MediumHigh() { return Priority(kLevels[kMedHigh]); }

  /// Priority higher than `MediumHigh`, but lower than `VeryHigh`, if possible.
  static constexpr Priority High() { return Priority(kLevels[kHigh]); }

  /// Priority higher than `High`, but lower than `Highest`, if possible.
  static constexpr Priority VeryHigh() { return Priority(kLevels[kVeryHigh]); }

  /// Returns the highest priority supported by the backend. The underlying OS
  /// may support higher priorities; backends may only expose a subset of
  /// priorities.
  static constexpr Priority Highest() { return Priority(kLevels[kHighest]); }

  /// Returns a priority at the backend-specified default level.
  static constexpr Priority Default() { return Priority(); }

  /// Returns the next higher priority. Fails if this priority is the maximum
  /// priority.
  ///
  /// @warning This function is not portable, since it fails on platforms that
  /// cannot represent the requested priority.
  constexpr Priority NextHigher(Priority maximum = Highest()) const {
    PW_ASSERT(*this != maximum);  // Priority cannot exceed the maximum value.
    return Priority(level_ + 1);
  }

  /// Returns the next lower priority. Fails if this priority is the minimum
  /// priority.
  ///
  /// @warning This function is not portable, since it fails on platforms that
  /// cannot represent the requested priority.
  constexpr Priority NextLower(Priority minimum = Lowest()) const {
    PW_ASSERT(*this != minimum);  // Priority cannot subceed the minimum value.
    return Priority(level_ - 1);
  }

  /// Returns the next lower priority, down to the provided maximum.
  constexpr Priority NextLowerClamped(Priority minimum = Lowest()) const {
    return *this > minimum ? Priority(level_ - 1) : *this;
  }

  /// Returns the next higher priority, up to the provided minimum.
  constexpr Priority NextHigherClamped(Priority maximum = Highest()) const {
    return *this < maximum ? Priority(level_ + 1) : *this;
  }

  constexpr bool operator==(Priority rhs) const { return level_ == rhs.level_; }
  constexpr bool operator!=(Priority rhs) const { return level_ != rhs.level_; }
  constexpr bool operator<(Priority rhs) const { return level_ < rhs.level_; }
  constexpr bool operator<=(Priority rhs) const { return level_ <= rhs.level_; }
  constexpr bool operator>(Priority rhs) const { return level_ > rhs.level_; }
  constexpr bool operator>=(Priority rhs) const { return level_ >= rhs.level_; }

  /// Alias for the native priority type used by the backend.
  ///
  /// @warning Using `native_type` is not portable!
  using native_type = T;

  /// Returns the native value used to represent this priority.
  ///
  /// @warning This function is not portable!
  constexpr native_type native() const {
    return static_cast<native_type>(level_.value());
  }

  /// Returns the priority value used to represent this native priority.
  ///
  /// @warning This function is not portable!
  static constexpr Priority FromNative(native_type priority) {
    const Level level(static_cast<NativeInt>(priority));
    PW_ASSERT(kLevels[kLowest] <= level);   // Priority cannot subceed the min
    PW_ASSERT(level <= kLevels[kHighest]);  // Priority cannot exceed the max
    return Priority(level);
  }

 private:
  static_assert(std::is_integral_v<native_type> || std::is_enum_v<native_type>,
                "The native priority type must be an integer or enum");
  static_assert(sizeof(native_type) <= sizeof(uint64_t),
                "The native priority type cannot be larger than 64 bits");

  using NativeInt = typename UnderlyingInteger<native_type>::type;
  using Level = AbstractLevel<NativeInt,
                              static_cast<NativeInt>(kLowestPriority),
                              static_cast<NativeInt>(kHighestPriority)>;

  enum : size_t {
    kLowest,
    kVeryLow,
    kLow,
    kMedLow,
    kMed,
    kMedHigh,
    kHigh,
    kVeryHigh,
    kHighest,
  };

  static_assert(kNamedPriorities == static_cast<size_t>(kHighest) + 1);

  // Produces a table that maps named priority levels to their corresponding
  // native priority values.
  static constexpr std::array<Level, kNamedPriorities> kLevels = [] {
    const std::array offsets = PriorityOffsets(Level::Range());

    std::array<Level, kNamedPriorities> levels{};
    for (size_t i = 0; i < offsets.size(); ++i) {
      levels[i] = Level(static_cast<NativeInt>(kLowestPriority)) + offsets[i];
    }
    return levels;
  }();

  static constexpr Level kDefault{static_cast<NativeInt>(kDefaultPriority)};
  static_assert(kLevels[kLowest] <= kDefault && kDefault <= kLevels[kHighest],
                "pw::thread::backend::kDefaultPriority must be between "
                "kLowestPriority and kHighestPriority");

  explicit constexpr Priority(Level level) : level_{level} {}

  Level level_;
};

}  // namespace pw::thread::internal
