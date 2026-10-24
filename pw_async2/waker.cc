// Copyright 2025 The Pigweed Authors
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

#include "pw_async2/waker.h"

#include <mutex>

#include "pw_async2/internal/lock.h"
#include "pw_async2/task.h"
#include "pw_log/tokenized_args.h"

namespace pw::async2 {

Waker& Waker::operator=(Waker&& other) noexcept {
  std::lock_guard lock(internal::lock());
  RemoveTaskIfSet();
  if (other.task_ == nullptr) {
    return *this;
  }
  task_ = other.task_;
#if PW_ASYNC2_DEBUG_WAIT_REASON
  wait_reason_ = other.wait_reason_;
#endif  // PW_ASYNC2_DEBUG_WAIT_REASON
  other.RemoveTask();
  task_->AddWakerLocked(*this);
  return *this;
}

void Waker::Wake() {
  internal::lock().lock();
  if (task_ == nullptr) {
    internal::lock().unlock();
  } else {
    Task& task = *task_;
    RemoveTask();
    task.Wake();
  }
}

bool Waker::TrySetTask(Context& context, log::Token wait_reason) {
  Task* const new_task = static_cast<Task*>(&context);

  std::lock_guard lock(internal::lock());
  if (task_ != nullptr && task_ != new_task) {
    return false;
  }

  set_wait_reason(wait_reason);

  if (task_ != new_task) {
    if (task_ != nullptr) {
      task_->RemoveWakerLocked(*this);
    }
    task_ = new_task;
    task_->AddWakerLocked(*this);
  }
  return true;
}

bool Waker::CloneInto(Waker& out, log::Token wait_reason) {
  std::lock_guard lock(internal::lock());
  if (out.task_ != nullptr && out.task_ != task_) {
    return false;
  }
  // The `out` waker already points to this task, so no work is necessary.
  if (out.task_ == task_) {
    return true;
  }
  // Remove the output waker from its existing task's list.
  out.RemoveTaskIfSet();
  out.task_ = task_;

  out.set_wait_reason(wait_reason);

  if (task_ != nullptr) {
    task_->AddWakerLocked(out);
  }
  return true;
}

bool Waker::IsEmpty() const {
  std::lock_guard lock(internal::lock());
  return task_ == nullptr;
}

void Waker::Clear() {
  std::lock_guard lock(internal::lock());
  RemoveTaskIfSet();
}

void Waker::RemoveTask() {
  task_->RemoveWakerLocked(*this);
  task_ = nullptr;
  set_wait_reason(log::kDefaultToken);
}

}  // namespace pw::async2
