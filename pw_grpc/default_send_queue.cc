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

#include "pw_grpc/default_send_queue.h"

#include <mutex>

#include "pw_log/log.h"

namespace pw::grpc {

UniquePtr<std::byte[]> DefaultSendQueue::PopNext() {
  std::lock_guard lock(send_mutex_);
  if (queue_.empty()) {
    return nullptr;
  }
  auto buffer = std::move(queue_.front());
  queue_.pop_front();
  return buffer;
}

void DefaultSendQueue::NotifyOnError(Status status) {
  std::lock_guard lock(send_mutex_);
  if (on_error_) {
    on_error_(status);
  }
}

void DefaultSendQueue::set_on_error(ErrorHandler&& error_handler) {
  std::lock_guard lock(send_mutex_);
  on_error_ = std::move(error_handler);
}

void DefaultSendQueue::ProcessSendQueue(async::Context&, Status task_status) {
  if (!task_status.ok()) {
    return;
  }

  UniquePtr<std::byte[]> buffer = PopNext();
  while (buffer != nullptr) {
    if (Status status = socket_.Write(pw::span(buffer.get(), buffer.size()));
        !status.ok()) {
      PW_LOG_ERROR("Failed to write to socket in DefaultSendQueue: %s",
                   status.str());
      NotifyOnError(status);
      return;
    }
    buffer = PopNext();
  }
}

bool DefaultSendQueue::QueueSend(UniquePtr<std::byte[]>&& buffer) {
  std::lock_guard lock(send_mutex_);
  if (!queue_.try_push_back(std::move(buffer))) {
    return false;
  }
  send_dispatcher_.Cancel(send_task_);
  send_dispatcher_.Post(send_task_);
  return true;
}

}  // namespace pw::grpc
