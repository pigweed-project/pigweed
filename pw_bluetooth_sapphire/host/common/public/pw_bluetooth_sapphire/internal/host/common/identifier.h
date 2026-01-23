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
#include <cpp-string/string_printf.h>

#include <cinttypes>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

namespace bt {

template <typename T>
struct IdentifierTraits {
  // Returns a string representation of |value|.
  static std::string ToString(T value) { return std::to_string(value); }
};

// Specializations for integer types return a fixed-length string.
template <>
struct IdentifierTraits<uint64_t> {
  static std::string ToString(uint64_t value) {
    return bt_lib_cpp_string::StringPrintf("%.16" PRIx64, value);
  }
};

// Opaque identifier type for host library layers.
template <typename T>
class Identifier {
  static_assert(std::is_trivial_v<T>);
  static_assert(!std::is_pointer_v<std::decay<T>>);

 public:
  using value_t = T;
  using Traits = IdentifierTraits<T>;

  constexpr explicit Identifier(const T& value) : value_(value) {}
  Identifier() = default;

  T value() const { return value_; }

  // Comparison.
  bool operator==(const Identifier& other) const {
    return value_ == other.value_;
  }
  bool operator!=(const Identifier& other) const {
    return value_ != other.value_;
  }

  // Postfix increment operator
  Identifier operator++(int) { return Identifier(value_++); }

  // Returns a string representation of this identifier. This function allocates
  // memory.
  std::string ToString() const { return Traits::ToString(value_); }

 private:
  T value_;
};

// Opaque identifier type for Bluetooth peers.
class PeerId : public Identifier<uint64_t> {
 public:
  constexpr explicit PeerId(uint64_t value) : Identifier<uint64_t>(value) {}
  constexpr PeerId() : PeerId(0u) {}

  bool IsValid() const { return value() != 0u; }
};

inline constexpr PeerId kInvalidPeerId(0u);

// Generates a valid random peer identifier. This function can never return
// kInvalidPeerId.
PeerId RandomPeerId();

}  // namespace bt

// Specialization of std::hash for std::unordered_set, std::unordered_map, etc.
namespace std {

template <typename T>
struct hash<bt::Identifier<T>> {
  size_t operator()(const bt::Identifier<T>& id) const {
    return std::hash<T>()(id.value());
  }
};

template <>
struct hash<bt::PeerId> {
  size_t operator()(const bt::PeerId& id) const {
    return std::hash<decltype(id.value())>()(id.value());
  }
};

}  // namespace std
