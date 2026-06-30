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

#include "pw_async_fuchsia/dispatcher.h"

#include <lib/async/cpp/time.h>

#include "pw_assert/check.h"
#include "pw_async_fuchsia/util.h"

namespace pw::async_fuchsia {

chrono::SystemClock::time_point FuchsiaDispatcher::now() {
  return ZxTimeToTimepoint(zx::time{async_now(dispatcher_)});
}

void FuchsiaDispatcher::Post(pw::async::Task& task) {
  pw::async::backend::NativeTask& native_task = task.native_type();
  if (native_task.is_posted()) {
    PW_CHECK(native_task.dispatcher_ == this,
             "Attempted to post a task that is already posted to a different "
             "Dispatcher.");
    if (native_task.due_time() <= now()) {
      // If the task is posted and due run, don't cancel and re-post, which
      // would push it to the back of the queue.
      return;
    }
  }
  PostAt(task, now());
}

void FuchsiaDispatcher::PostAt(pw::async::Task& task,
                               chrono::SystemClock::time_point time) {
  pw::async::backend::NativeTask& native_task = task.native_type();
  if (native_task.is_posted()) {
    PW_CHECK(native_task.dispatcher_ == this,
             "Attempted to post a task that is already posted to a different "
             "Dispatcher.");
    Cancel(task);
  }
  native_task.set_due_time(time);
  native_task.dispatcher_ = this;
  native_task.set_posted(true);
  zx_status_t status = async_post_task(dispatcher_, &native_task);
  ZX_ASSERT(status == ZX_OK);
}

bool FuchsiaDispatcher::Cancel(pw::async::Task& task) {
  if (async_cancel_task(dispatcher_, &task.native_type()) == ZX_OK) {
    task.native_type().set_posted(false);
    return true;
  }
  return false;
}

}  // namespace pw::async_fuchsia
