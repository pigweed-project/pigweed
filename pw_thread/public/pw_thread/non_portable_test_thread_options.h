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

// IMPORTANT: DO NOT USE THIS HEADER FOR UNIT TESTS!!!
//
// Instead, use pw::thread::test::TestThreadContext in
// pw_thread/test_thread_context.h. See
// https://pigweed.dev/pw_thread#unit-testing-with-threads.
//
// pw_thread/non_portable_test_thread_options.h is not a facade. Code written
// against it is not portable. It was created for testing of pw_thread itself,
// so threads with different configurations can be instantiated in tests.
#pragma once

#include "pw_thread/options.h"
#include "pw_thread/thread.h"

namespace pw::thread::test {

// Two test threads are used to verify the thread facade.
//
// These functions are NOT part of a facade! They are used to allocate thread
// options for testing pw_thread backends only. Multiple variations of these
// functions may be instantiated within a single toolchain for testing purposes.
// Do NOT use unless absolutely necessary. Instead, use the TestThreadContext
// facade for unit tests.
const Options& TestOptionsThread0();
const Options& TestOptionsThread1();

// The proper way to ensure a thread is finished and cleaned up is to call
// join(). However, the pw_thread facade tests must test detached thread
// functionality. This backend-specific cleanup API blocks until all the test
// threads above have finished executing.
//
// Threads may be backed by static contexts or dynamic context heap allocations.
// After this function returns, the threads' static contexts are ready for reuse
// and/or their dynamically allocated memory has been freed.
//
// Precondition: The threads must have started to execute before calling this
// if cleanup is expected.
void WaitUntilDetachedThreadsCleanedUp();

}  // namespace pw::thread::test
