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
#include <initializer_list>

namespace pw::internal_result::test {

struct Copyable {
  constexpr Copyable(int v = 0) : value(v) {}
  constexpr Copyable(const Copyable&) = default;
  constexpr Copyable(Copyable&&) = default;
  constexpr Copyable& operator=(const Copyable&) = default;
  constexpr Copyable& operator=(Copyable&&) = default;
  int value;
  constexpr bool operator==(const Copyable& other) const {
    return value == other.value;
  }
};

struct CopyCtorNotAssign {
  constexpr CopyCtorNotAssign(int v) : value(v) {}
  constexpr CopyCtorNotAssign(const CopyCtorNotAssign&) = default;
  constexpr CopyCtorNotAssign(CopyCtorNotAssign&&) = default;
  constexpr CopyCtorNotAssign& operator=(const CopyCtorNotAssign&) = delete;
  constexpr CopyCtorNotAssign& operator=(CopyCtorNotAssign&&) = delete;
  int value;
  constexpr bool operator==(const CopyCtorNotAssign& other) const {
    return value == other.value;
  }
};

struct MoveCtorNotAssign {
  constexpr MoveCtorNotAssign(int v) : value(v) {}
  constexpr MoveCtorNotAssign(const MoveCtorNotAssign&) = delete;
  constexpr MoveCtorNotAssign(MoveCtorNotAssign&&) = default;
  constexpr MoveCtorNotAssign& operator=(const MoveCtorNotAssign&) = delete;
  constexpr MoveCtorNotAssign& operator=(MoveCtorNotAssign&&) = delete;
  int value;
  constexpr bool operator==(const MoveCtorNotAssign& other) const {
    return value == other.value;
  }
};

struct CopyAssignNotCtor {
  constexpr CopyAssignNotCtor(int v) : value(v) {}
  constexpr CopyAssignNotCtor(const CopyAssignNotCtor&) = delete;
  constexpr CopyAssignNotCtor(CopyAssignNotCtor&&) = default;
  constexpr CopyAssignNotCtor& operator=(const CopyAssignNotCtor&) = default;
  constexpr CopyAssignNotCtor& operator=(CopyAssignNotCtor&&) = default;
  int value;
  constexpr bool operator==(const CopyAssignNotCtor& other) const {
    return value == other.value;
  }
};

struct MoveAssignNotCtor {
  constexpr MoveAssignNotCtor(int v) : value(v) {}
  constexpr MoveAssignNotCtor(const MoveAssignNotCtor&) = delete;
  constexpr MoveAssignNotCtor(MoveAssignNotCtor&&) = delete;
  constexpr MoveAssignNotCtor& operator=(const MoveAssignNotCtor&) = delete;
  constexpr MoveAssignNotCtor& operator=(MoveAssignNotCtor&&) = default;
  int value;
  constexpr bool operator==(const MoveAssignNotCtor& other) const {
    return value == other.value;
  }
};

struct ConvertibleFromInt {
  constexpr ConvertibleFromInt(int v) : value(v) {}
  int value;
  constexpr bool operator==(const ConvertibleFromInt& other) const {
    return value == other.value;
  }
};

struct ExplicitConstructibleFromInt {
  explicit constexpr ExplicitConstructibleFromInt(int v) : value(v) {}
  int value;
  constexpr bool operator==(const ExplicitConstructibleFromInt& other) const {
    return value == other.value;
  }
};

struct ConvertibleToBool {
  explicit constexpr ConvertibleToBool(bool y) : x(y) {}
  constexpr operator bool() const { return x; }  // NOLINT
  bool x = false;
};

struct InitListConstructible {
  constexpr InitListConstructible(std::initializer_list<int> ilist,
                                  int mult = 1)
      : sum(0), count(ilist.size()) {
    for (int v : ilist) {
      sum += v * mult;
    }
  }
  int sum;
  size_t count;
};

}  // namespace pw::internal_result::test
