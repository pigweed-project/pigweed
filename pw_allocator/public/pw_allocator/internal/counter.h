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

#include <array>
#include <cstddef>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace pw::allocator::test {

/// A test utility class that tracks how often it has been created or
/// destroyed.
///
/// Calling `GetNumCtorCalls` or `GetNumDtorCalls` resets the associated
/// counter to zero.
class Counter {
 public:
  Counter() : Counter(num_ctor_calls_) {}
  explicit Counter(size_t value) : value_(value) { ++num_ctor_calls_; }
  Counter(const Counter&) = delete;
  Counter(Counter&&) {}

  ~Counter() { ++num_dtor_calls_; }

  static size_t TakeNumCtorCalls() { return std::exchange(num_ctor_calls_, 0); }
  static size_t TakeNumDtorCalls() { return std::exchange(num_dtor_calls_, 0); }
  static void Reset() {
    // Clear values from any previous test.
    Counter::TakeNumCtorCalls();
    Counter::TakeNumDtorCalls();
  }

  size_t value() const { return value_; }

  void set_value(size_t value) { value_ = value; }

 private:
  static size_t num_ctor_calls_;
  static size_t num_dtor_calls_;
  size_t value_;
};

/// A test utility class that can only be move-constructed.
///
/// This can be used to verify factory methods use `std::forward`.
class CounterSink {
 public:
  CounterSink() = delete;
  CounterSink(Counter&& counter) : value_(counter.value()) {}
  size_t value() const { return value_; }

 private:
  size_t value_;
};

/// A test utility class that is larger than its base class.
///
/// This can be used to ensure a smart pointer to a base class type destroys and
/// deallocates using the derived class type.
class CounterWithBuffer : public Counter {
 public:
  std::array<std::byte, 128> buffer_;
};

class TestWithCounters : public ::testing::Test {
 protected:
  TestWithCounters() { Counter::Reset(); }
};

}  // namespace pw::allocator::test
