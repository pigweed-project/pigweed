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

#include "pw_async_fuchsia/task.h"

#include <zircon/assert.h>

#include "pw_async_fuchsia/util.h"

namespace pw::async::backend {

NativeTask::NativeTask(::pw::async::Task& task)
    : async_task_t{{ASYNC_STATE_INIT}, &NativeTask::Handler, {}}, task_(task) {}
NativeTask::NativeTask(::pw::async::Task& task, TaskFunction&& f)
    : async_task_t{{ASYNC_STATE_INIT}, &NativeTask::Handler, {}},
      func_(std::move(f)),
      task_(task) {}

void NativeTask::operator()(Context& ctx, Status status) { func_(ctx, status); }

void NativeTask::set_function(TaskFunction&& f) { func_ = std::move(f); }

pw::chrono::SystemClock::time_point NativeTask::due_time() const {
  return pw::async_fuchsia::ZxTimeToTimepoint(zx::time{deadline});
}

void NativeTask::set_due_time(chrono::SystemClock::time_point due_time) {
  deadline = pw::async_fuchsia::TimepointToZxTime(due_time).get();
}

void NativeTask::Handler(async_dispatcher_t* /*dispatcher*/,
                         async_task_t* task,
                         zx_status_t status) {
  auto self = static_cast<NativeTask*>(task);
  self->is_posted_.store(false);
  Context c{.dispatcher = self->dispatcher_, .task = &self->task_};
  (*self)(c, status == ZX_OK ? OkStatus() : Status::Cancelled());
}

}  // namespace pw::async::backend
