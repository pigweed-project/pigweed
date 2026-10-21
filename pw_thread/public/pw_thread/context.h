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

#include <cstddef>
#include <limits>

#include "pw_thread/attrs.h"
#include "pw_thread/stack.h"
#include "pw_thread_backend/context.h"  // IWYU pragma: export

namespace pw {

/// @module{pw_thread}

inline constexpr size_t kExternallyAllocatedThreadStack =
    std::numeric_limits<size_t>::max();

/// Represents the resources required for one thread. May include OS data
/// structures, the thread stack, or be empty, depending on the platform.
///
/// `ThreadContext` may be reused or deleted if the associated thread is joined.
///
/// `ThreadContext` takes an optional stack size template parameter. If a stack
/// size is provided, the context allocates a stack internally, if supported by
/// the backend. If no stack is is provided (`ThreadContext<>`), the
/// `ThreadContext` must be paired with a `pw::ThreadStack`.
///
/// @warning Avoid creating `ThreadContext` objects on the stack. Instead,
/// create them in static/global or heap memory. Depending on backend and
/// configuration, `ThreadContext` may contain the thread's stack. Not all
/// platforms allow having stack within a stack, so avoid this for portability.
template <size_t kStackSizeBytes = kExternallyAllocatedThreadStack>
class ThreadContext {
 public:
  constexpr ThreadContext() = default;

  ThreadContext(const ThreadContext&) = delete;
  ThreadContext& operator=(const ThreadContext&) = delete;

  constexpr thread::backend::NativeContextWithStack<kStackSizeBytes>& native() {
    return native_context_;
  }

 private:
  thread::backend::NativeContextWithStack<kStackSizeBytes> native_context_;
};

// ThreadContext with externally allocated stack.
template <>
class ThreadContext<kExternallyAllocatedThreadStack> {
 public:
  constexpr ThreadContext() = default;

  ThreadContext(const ThreadContext&) = delete;
  ThreadContext& operator=(const ThreadContext&) = delete;

  constexpr thread::backend::NativeContext& native() { return native_context_; }

 private:
  thread::backend::NativeContext native_context_;
};

/// Declares a `ThreadContext` that is associated with a specific set of thread
/// attributes. The `ThreadContext` may be reused if the associated thread is
/// joined, but all threads use the same `ThreadAttrs`.
template <const ThreadAttrs& kAttributes>
class ThreadContextFor {
 public:
  constexpr ThreadContextFor() = default;

  ThreadContextFor(const ThreadContextFor&) = delete;
  ThreadContextFor& operator=(const ThreadContextFor&) = delete;

  constexpr auto& native() { return context_.native(); }

 private:
  ThreadContext<kAttributes.has_external_stack()
                    ? kExternallyAllocatedThreadStack
                    : kAttributes.stack_size_bytes()>
      context_;
};

/// Alias for ThreadContext with the backend's default stack size.
using DefaultThreadContext =
    ThreadContext<thread::backend::kDefaultStackSizeBytes>;

}  // namespace pw
