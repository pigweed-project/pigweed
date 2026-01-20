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

#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace pw::bluetooth::proxy::hci {

template <typename Int, Int ReservedInvalidId = 0>
class IdentifierMint;

/// Type safe, move-only unforgeable identifier.
template <typename Int, Int ReservedInvalidId = 0>
class Identifier final {
 public:
  using ValueType = Int;
  using Mint = IdentifierMint<Int, ReservedInvalidId>;

  constexpr Identifier() : id_(kReservedInvalidId) {}
  constexpr Identifier(const Identifier&) = delete;
  constexpr Identifier(Identifier&& other) : Identifier() {
    std::swap(id_, other.id_);
  }
  constexpr Identifier& operator=(const Identifier&) = delete;
  constexpr Identifier& operator=(Identifier&& other) {
    std::swap(id_, other.id_);
    return *this;
  }

  // Support equality between `Identifier`s and the value contained within them.
  // We don't support equality for `Identifier`s directly because there should
  // only ever be one identifier in existence with this (mint_, id_) pair.
  constexpr bool operator==(Int value) const { return id_ == value; }
  constexpr bool operator!=(Int value) const { return id_ != value; }

  constexpr bool is_valid() const { return id_ != kReservedInvalidId; }
  constexpr Int value() const { return id_; }

 private:
  // Only `IdentifierMint`s can mint new IDs.
  friend Mint;

  static constexpr Int kReservedInvalidId = ReservedInvalidId;
  constexpr explicit Identifier(Int id) : id_(id) {}

  // Unique value of the identifier.
  Int id_;
};

// A type to produce new unique `Identifier`s.
template <typename Int, Int ReservedInvalidId>
class IdentifierMint final {
 public:
  using Identifier =
      ::pw::bluetooth::proxy::hci::Identifier<Int, ReservedInvalidId>;

  // Generate a new (valid) unique Identifier, or std::nullopt if not possible.
  // Callers must supply a `bool(Int) is_used` function, which will check if an
  // identifier is already in use, so an external std::(unordered_)map-like or
  // std::(unordered_)set-like data structure should be used.
  template <typename Func>
  std::optional<Identifier> MintId(Func&& is_used) {
    Int next_generated = last_generated_;
    while (++next_generated != last_generated_) {
      if (next_generated == Identifier::kReservedInvalidId) {
        // Skip the reserved IDs.
        continue;
      }
      if (!is_used(next_generated)) {
        // Allocation found.
        last_generated_ = next_generated;
        return Identifier(next_generated);
      }
    }
    // Identifier space is exhausted, all possible values are in use.
    return std::nullopt;
  }

 private:
  // Begin generating immediately after the reserved ID, this is arbitrary and
  // should not be relied upon externally.
  Int last_generated_ = Identifier::kReservedInvalidId;
};

}  // namespace pw::bluetooth::proxy::hci
