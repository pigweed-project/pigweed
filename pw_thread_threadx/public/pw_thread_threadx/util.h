// Copyright 2021 The Pigweed Authors
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

#include "pw_function/function.h"
#include "pw_status/status.h"
#include "tx_api.h"

namespace pw::thread::threadx {

/// @module{pw_thread_threadx}

/// A callback that is executed for each thread when using `ForEachThread()`.
/// The callback should return `true` if thread iteration should continue. When
/// this callback returns `false`, `ForEachThread()` ceases iteration of threads
/// and returns an @ABORTED error code.
using ThreadCallback = pw::Function<bool(const TX_THREAD&)>;

/// Iterates through all threads that haven't been deleted, calling the provided
/// callback on each thread.
///
/// @pre This is only safe to use when the scheduler is disabled.
///
/// @param[in] cb Callback invoked for each non-deleted thread.
///
/// @returns
/// * @OK: Successfully iterated over all threads.
/// * @ABORTED: The callback requested early termination of thread iteration.
Status ForEachThread(const ThreadCallback& cb);

/// @endmodule

namespace internal {

// This function is exposed for testing. Prefer
// pw::thread::threadx::ForEachThread.
Status ForEachThread(const TX_THREAD& starting_thread,
                     const ThreadCallback& cb);

}  // namespace internal
}  // namespace pw::thread::threadx
