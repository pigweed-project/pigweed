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

#include "pw_async2/size_report/async2_baseline.h"

#include <cstdint>
#include <utility>

#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/size_report/size_report.h"
#include "pw_async2/waker.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_preprocessor/compiler.h"

namespace pw::async2::size_report {
namespace {

BasicDispatcher dispatcher;

}  // namespace

BasicDispatcher& GetDispatcher() { return dispatcher; }

PW_NO_INLINE int SetAsync2Baseline(volatile uint32_t& mask) {
  SetBaseline(mask);

  // Exercise Task posting, polling, waker storage/move/wake, and completion.
  MockTask task;
  dispatcher.Post(task);

  // Exercise explicit Task::Deregister() on a pending task.
  MockTask deregister_task;
  dispatcher.Post(deregister_task);
  dispatcher.RunUntilStalled();
  deregister_task.Deregister();

  task.should_complete = true;
  Waker waker;
  PW_BLOAT_EXPR((waker = std::move(task.last_waker)), mask);
  waker.Wake();
  dispatcher.RunToCompletion();

  // Exercise core FutureCore + BaseFutureList + PW_AWAIT machinery (including
  // move-construction, move-assignment, cancellation/Reset, PushRequireEmpty,
  // Push, ResolveOne, and ResolveAll).
  BaselineFutureProvider provider;
  {
    BaselineFuture single = provider.GetSingle();
    PW_BLOAT_COND(provider.size() > 0, mask);
    BaselineFuture moved(std::move(single));
    moved.Cancel();
  }

  BaselineFuture future_1;
  BaselineFuture future_2;
  FuncTask future_task_1([&](Context& cx) -> Poll<> {
    if (!future_1.is_pendable()) {
      future_1 = provider.Get();
    }
    PW_AWAIT(future_1, cx);
    return Ready();
  });
  FuncTask future_task_2([&](Context& cx) -> Poll<> {
    if (!future_2.is_pendable()) {
      future_2 = provider.Get();
    }
    PW_AWAIT(future_2, cx);
    return Ready();
  });

  dispatcher.Post(future_task_1);
  dispatcher.Post(future_task_2);
  dispatcher.RunUntilStalled();

  provider.ResolveOne();
  dispatcher.RunUntilStalled();
  provider.ResolveAll();
  dispatcher.RunToCompletion();

  return task.polled;
}

}  // namespace pw::async2::size_report
