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

#include <cstdint>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_status/status.h"

namespace pw {

class JsonBuilder;
class NestedJsonObject;

namespace json_impl {

// Tracks how deeply nested an array or object is and the types of the
// structures it is nested in.
class Nesting {
 public:
  enum Type : uint16_t { kArray = 0, kObject = 1 };

  constexpr Nesting() : json_offset_(0), depth_(0), types_(0) {}

  constexpr Nesting(const Nesting&) = default;
  constexpr Nesting& operator=(const Nesting&) = default;

  constexpr Nesting Nest(size_t start, Type type) const {
    PW_ASSERT(depth_ < 16);  // Arrays or objects may be nested at most 17 times
    return Nesting(start,
                   static_cast<uint16_t>(depth_ + 1),
                   static_cast<uint16_t>((types_ << 1) | type));
  }

  // The start of this structure in the original buffer.
  constexpr size_t offset() const { return json_offset_; }

  // Number of layers this array or object is nested within.
  constexpr size_t depth() const { return depth_; }

  constexpr void CheckNesting(char* json) const {
    PW_ASSERT(json[depth_] == '\0');  // Enclosing JSON has changed.
    for (uint16_t i = 0; i < depth_; ++i) {
      PW_ASSERT(json[i] == close(i));  // Enclosing JSON has changed.
    }
  }

  // Writes closing ] or } and the final \0.
  constexpr void Terminate(char* buffer) const {
    for (uint16_t i = 0; i < depth_; ++i) {
      buffer[i] = close(i);
    }
    buffer[depth_] = '\0';
  }

 private:
  constexpr Nesting(size_t offset, uint16_t depth, uint16_t types)
      : json_offset_(offset), depth_(depth), types_(types) {}

  constexpr char close(uint16_t i) const {
    return (types_ & (1 << i)) == 0 ? ']' : '}';
  }

  size_t json_offset_;
  uint16_t depth_;  // Depth only counts nested structures; [] is 0, [{}] is 1
  uint16_t types_;
};

// Represents a nested array or object.
class NestedJson {
 public:
  constexpr NestedJson(const NestedJson&) = delete;
  constexpr NestedJson& operator=(const NestedJson&) = delete;

  constexpr NestedJson(NestedJson&& other) : builder_(), nesting_() {
    *this = std::move(other);
  }

  constexpr NestedJson& operator=(NestedJson&& other) {
    builder_ = other.builder_;
    nesting_ = other.nesting_;
    other.builder_ = nullptr;
    return *this;
  }

  constexpr JsonBuilder& builder() const { return *builder_; }
  constexpr const Nesting& nesting() const { return nesting_; }

 private:
  friend class pw::JsonBuilder;

  constexpr NestedJson(JsonBuilder& builder, const Nesting& nesting)
      : builder_(&builder), nesting_(nesting) {}

  JsonBuilder* builder_;
  Nesting nesting_;
};

}  // namespace json_impl
}  // namespace pw
