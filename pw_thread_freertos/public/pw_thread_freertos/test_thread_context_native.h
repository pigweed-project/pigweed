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

#include "pw_thread_freertos/config.h"
#include "pw_thread_freertos/context.h"
#include "pw_thread_freertos/options.h"

namespace pw::thread::test::backend {

// Native Test Thread Options backend class for FreeRTOS.
class TestThreadContextNative {
 public:
  static constexpr size_t kStackSizeWords =
      PW_THREAD_FREERTOS_CONFIG_TEST_THREAD_STACK_SIZE_WORDS;

  constexpr TestThreadContextNative() : context_(kConstexpr) {
    options_.set_static_context(context_);
  }

  TestThreadContextNative(const TestThreadContextNative&) = delete;
  TestThreadContextNative& operator=(const TestThreadContextNative&) = delete;

  ~TestThreadContextNative() = default;

  const Options& options() const { return options_; }

 private:
  freertos::Options options_;
  freertos::StaticContextWithStack<kStackSizeWords> context_;
};

}  // namespace pw::thread::test::backend
