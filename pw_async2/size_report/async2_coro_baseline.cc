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

#include "pw_async2/size_report/async2_coro_baseline.h"

#include <cstdint>
#include <utility>

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_async2/future_task.h"
#include "pw_async2/size_report/async2_baseline.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"

namespace pw::async2::size_report {
namespace {

Coro<int> ImmediatelyReturnsFive(CoroContext) { co_return 5; }

Coro<Status> AwaitsCoroAndFuture(CoroContext coro_cx,
                                 BaselineFuture future,
                                 int& out) {
  out = co_await ImmediatelyReturnsFive(coro_cx);
  co_await std::move(future);
  co_return OkStatus();
}

}  // namespace

PW_NO_INLINE int SetAsync2CoroBaseline(volatile uint32_t& mask) {
  int result = SetAsync2Baseline(mask);

  BaselineFutureProvider provider;
  int output = 0;
  FutureTask coro_task(
      AwaitsCoroAndFuture(CoroContext(GetAllocator()), provider.Get(), output));
  GetDispatcher().Post(coro_task);
  PW_BLOAT_COND(GetDispatcher().RunUntilStalled(), mask);

  provider.ResolveAll();
  GetDispatcher().RunToCompletion();
  PW_BLOAT_COND(output != 0, mask);

  return result + output;
}

}  // namespace pw::async2::size_report
