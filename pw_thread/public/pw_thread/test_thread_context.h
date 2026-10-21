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

#include "pw_thread/options.h"
#include "pw_thread_backend/test_thread_context_native.h"  // IWYU pragma: export

namespace pw::thread::test {

/// @module{pw_thread}

/// `TestThreadContext` is a facade class for creating threads for unit tests in
/// a platform independent way. To use it, set
/// `pw_thread_TEST_THREAD_CONTEXT_BACKEND` to a backend that implements the
/// `pw::thread::test::backend::TestThreadContextNative` class.
///
/// To create a thread for unit testing, instantiate a `TestThreadContext`, then
/// call `options()` to obtain a `pw::thread::Options`. Use that `Options` to
/// start a `Thread`. Users must ensure the context's lifespan outlives the
/// thread it creates. Recycling or destroying the context is only allowed if
/// `join()` is called on the thread first.
///
/// @code{.cpp}
///    pw::thread::test::TestThreadContext context;
///    pw::Thread test_thread(context.options(), ExampleThreadFunction);
/// @endcode
///
/// Threads created with `TestThreadContext` cannot be configured in any way.
/// Backends should create threads with sufficient resources to execute typical
/// unit tests. Tests for complex scenarios or interactions where e.g. priority
/// matters are not portable, and `TestThreadContext` may not work for them.
/// Non-portable tests may include backend-specific headers and instantiate
/// thread options for their platforms as required.
///
/// @note Developers should structure their logic so it can be tested without
/// spawning a thread. Unit tests should avoid spawning threads unless
/// absolutely necessary.
///
/// @warning Threads using the `TestThreadContext` may only be detached if the
/// context has a static lifetime, meaning the context is both never re-used and
/// not destroyed before the end of the lifetime of the application.
class TestThreadContext {
 public:
  TestThreadContext() = default;

  TestThreadContext(const TestThreadContext&) = delete;
  TestThreadContext& operator=(const TestThreadContext&) = delete;

  /// `pw::thread::test::TestThreadContext` returns a `pw::thread::Options`
  /// associated with the this object, which can be used to contruct a thread.
  ///
  /// @return The default options for testing thread.
  const Options& options() const { return context_.options(); }

 private:
  backend::TestThreadContextNative context_;
};

}  // namespace pw::thread::test
