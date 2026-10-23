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

#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
namespace pw::async2::size_report {

class MockTask : public Task {
 public:
  bool should_complete = false;
  int polled = 0;
  Waker last_waker;

 private:
  Poll<> DoPend(Context& cx) override {
    ++polled;
    PW_ASYNC_STORE_WAKER(cx, last_waker, "MockTask is waiting for last_waker");
    if (should_complete) {
      return Ready();
    }
    return Pending();
  }
};

template <typename T>
struct PendableValue {
  PendableValue(T value)
      : result(value), poll_count(0), allow_completion(false), waker() {}
  PendableValue(PendableValue&) = delete;
  PendableValue(PendableValue&&) = delete;
  PendableValue& operator=(const PendableValue&) = delete;
  PendableValue& operator=(PendableValue&&) = delete;
  ~PendableValue() = default;

  Poll<T> Get(Context& cx) {
    ++poll_count;
    if (allow_completion) {
      return result;
    }
    PW_ASYNC_STORE_WAKER(cx, waker, "PendableValue is unavailable");
    return Pending();
  }

  T result;
  int poll_count;
  bool allow_completion;
  Waker waker;
};

using PendableInt = PendableValue<int>;
using PendableUint = PendableValue<unsigned int>;
using PendableChar = PendableValue<char>;

Allocator& GetAllocator();

int SetBaseline(uint32_t mask);

}  // namespace pw::async2::size_report
