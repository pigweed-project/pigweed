// Copyright 2026 The Pigweed Authors
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
#include "pw_async2/coro_task.h"

#include "pw_assert/check.h"

namespace pw::async2::internal {

void CrashDueToCoroutineAllocationFailure() {
  PW_CRASH(
      "Failed to allocate coroutine. Use a pw::Allocator with a larger "
      "capacity, or use pw::async2::FallibleCoroTask to gracefully handle "
      "allocation failure.");
}

}  // namespace pw::async2::internal
