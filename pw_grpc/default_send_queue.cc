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

DefaultSendQueue::~DefaultSendQueue() {
  send_dispatcher_.RequestStop();
  send_dispatcher_.Cancel(send_task_);
}

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

void DefaultSendQueue::NotifyOnSpaceAvailable() {
  std::lock_guard lock(send_mutex_);
  if (on_space_available_) {
    on_space_available_();
  }
}

void DefaultSendQueue::set_on_error(ErrorHandler&& error_handler) {
  std::lock_guard lock(send_mutex_);
  on_error_ = std::move(error_handler);
}

void DefaultSendQueue::set_on_space_available(
    SpaceAvailableCallback&& on_space_available) {
  std::lock_guard lock(send_mutex_);
  on_space_available_ = std::move(on_space_available);
}

void DefaultSendQueue::SetFrameSource(FrameSource* source) {
  std::lock_guard source_lock(frame_source_mutex_);
  bool wait_for_pull = false;
  {
    std::lock_guard lock(send_mutex_);
    if (frame_source_ == source) {
      if (source != nullptr) {
        data_ready_ = true;
        send_dispatcher_.Cancel(send_task_);
        send_dispatcher_.Post(send_task_);
      }
      return;
    }
    frame_source_ = nullptr;
    data_ready_ = false;
    wait_for_pull = pull_active_;
    waiting_for_pull_ = wait_for_pull;
    if (source == nullptr && queue_.empty()) {
      send_dispatcher_.Cancel(send_task_);
    }
  }
  if (wait_for_pull) {
    pull_done_.acquire();
    // Wait until the puller has released send_mutex_ after signaling
    // pull_done_.
    std::lock_guard lock(send_mutex_);
  }
  if (source != nullptr) {
    std::lock_guard lock(send_mutex_);
    frame_source_ = source;
    data_ready_ = true;
    send_dispatcher_.Cancel(send_task_);
    send_dispatcher_.Post(send_task_);
  }
}

void DefaultSendQueue::NotifyDataReady() {
  std::lock_guard lock(send_mutex_);
  if (frame_source_ == nullptr) {
    return;
  }
  data_ready_ = true;
  send_dispatcher_.Cancel(send_task_);
  send_dispatcher_.Post(send_task_);
}

void DefaultSendQueue::ProcessSendQueue(async::Context&, Status task_status) {
  if (!task_status.ok()) {
    return;
  }

  while (true) {
    UniquePtr<std::byte[]> buffer = PopNext();
    if (buffer != nullptr) {
      if (Status status = socket_.Write(pw::span(buffer.get(), buffer.size()));
          !status.ok()) {
        PW_LOG_ERROR("Failed to write to socket in DefaultSendQueue: %s",
                     status.str());
        NotifyOnError(status);
        return;
      }
      NotifyOnSpaceAvailable();
      continue;
    }

    FrameSource* source = nullptr;
    {
      std::lock_guard lock(send_mutex_);
      if (frame_source_ == nullptr) {
        return;
      }
      source = frame_source_;
      data_ready_ = false;
      pull_active_ = true;
    }

    const StatusWithSize pulled = source->FillPacket(pull_buffer_);

    bool retry = false;
    {
      std::lock_guard lock(send_mutex_);
      pull_active_ = false;
      if (waiting_for_pull_) {
        waiting_for_pull_ = false;
        pull_done_.release();
      }
      retry = data_ready_ && frame_source_ != nullptr;
    }

    if (!pulled.ok()) {
      PW_LOG_ERROR("FrameSource::FillPacket failed in DefaultSendQueue: %s",
                   pulled.status().str());
      NotifyOnError(pulled.status());
      return;
    }

    if (pulled.size() == 0) {
      if (retry) {
        continue;
      }
      return;
    }

    if (Status status =
            socket_.Write(pw::span(pull_buffer_.data(), pulled.size()));
        !status.ok()) {
      PW_LOG_ERROR("Failed to write pulled packet in DefaultSendQueue: %s",
                   status.str());
      NotifyOnError(status);
      return;
    }
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
