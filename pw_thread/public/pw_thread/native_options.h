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

#include "pw_thread/attrs.h"
#include "pw_thread/context.h"
#include "pw_thread/options.h"
#include "pw_thread_backend/options.h"  // IWYU pragma: export

namespace pw {

/// Alias for the backend's native Options type.
using NativeThreadOptions = ::pw::thread::backend::NativeOptions;

/// Gets `NativeThreadOptions` for the given `ThreadContext` and `ThreadAttrs`.
/// Checks for compatibility between the `ThreadContext` and `ThreadAttrs` at
/// compile time.
template <const ThreadAttrs& kAttributes, size_t kContextStackSizeBytes>
constexpr NativeThreadOptions GetThreadOptions(
    ThreadContext<kContextStackSizeBytes>& context) {
  if constexpr (kContextStackSizeBytes == kExternallyAllocatedThreadStack) {
    static_assert(
        kAttributes.has_external_stack(),
        "No stack was provided! The ThreadContext<> does not include a stack, "
        "but the ThreadAttrs does not have one set (set_stack())");
  } else if constexpr (!kAttributes.has_external_stack()) {
    static_assert(
        kContextStackSizeBytes >= kAttributes.stack_size_bytes(),
        "ThreadContext stack is too small for the requested ThreadAttrs");
  }
  return thread::backend::GetNativeOptions(context.native(), kAttributes);
}

/// Gets `NativeThreadOptions` for the given `ThreadContext` and `ThreadAttrs`.
/// Checks for compatibility between the `ThreadContext` and `ThreadAttrs` with
/// a `PW_ASSERT`. If possible, use the overload with the `ThreadAttr` as a
/// template parameter to move these checks to `static_assert`.
template <size_t kContextStackSizeBytes>
constexpr NativeThreadOptions GetThreadOptions(
    ThreadContext<kContextStackSizeBytes>& context,
    const ThreadAttrs& attributes) {
  if constexpr (kContextStackSizeBytes == kExternallyAllocatedThreadStack) {
    PW_ASSERT(attributes.has_external_stack());  // Must set stack
  } else if (!attributes.has_external_stack()) {
    PW_ASSERT(kContextStackSizeBytes >= attributes.stack_size_bytes());
  }
  return thread::backend::GetNativeOptions(context.native(), attributes);
}

/// Gets `NativeThreadOptions` for a `ThreadContextFor`.
template <const ThreadAttrs& kAttributes>
constexpr NativeThreadOptions GetThreadOptions(
    ThreadContextFor<kAttributes>& context) {
  return thread::backend::GetNativeOptions(context.native(), kAttributes);
}

}  // namespace pw
