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

#include <cstddef>  // IWYU pragma: export

#include "pw_thread_freertos/config.h"   // IWYU pragma: export
#include "pw_thread_freertos/context.h"  // IWYU pragma: export
#include "pw_toolchain/constexpr_tag.h"  // IWYU pragma: export

namespace pw::thread::backend {

#if PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED

using NativeContext = ::pw::thread::freertos::Context;

template <size_t kStackSizeBytes>
using NativeContextWithStack = ::pw::thread::freertos::Context;

#else

using NativeContext = ::pw::thread::freertos::StaticContext;

template <size_t kStackSizeBytes>
class NativeContextWithStack {
 public:
  constexpr NativeContextWithStack() : context_with_stack_(kConstexpr) {}

  auto& context() { return context_with_stack_; }
  const auto& context() const { return context_with_stack_; }

 private:
  freertos::StaticContextWithStack<freertos::StackSizeBytesToWords(
      kStackSizeBytes)>
      context_with_stack_;
};

#endif  // PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED

}  // namespace pw::thread::backend
