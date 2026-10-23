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

#include "pw_async2/epoll_dispatcher.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_status/status.h"

namespace pw::async2 {
namespace {

constexpr char kNotificationSignal = 'c';

}  // namespace

Status EpollDispatcher::NativeInit() {
  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ == -1) {
    PW_LOG_ERROR("Failed to open epoll: %s", std::strerror(errno));
    return Status::Internal();
  }

  int pipefd[2];
  if (pipe2(pipefd, O_DIRECT | O_NONBLOCK) == -1) {
    PW_LOG_ERROR("Failed to create pipe: %s", std::strerror(errno));
    return Status::Internal();
  }

  wait_fd_ = pipefd[0];
  notify_fd_ = pipefd[1];

  struct epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = wait_fd_;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wait_fd_, &event) == -1) {
    PW_LOG_ERROR("Failed to initialize epoll event for dispatcher");
    return Status::Internal();
  }

  return OkStatus();
}

Status EpollDispatcher::NativeWaitForWake() {
  std::array<epoll_event, kMaxEventsToProcessAtOnce> events;

  int num_events =
      epoll_wait(epoll_fd_, events.data(), events.size(), /*timeout=*/-1);
  if (num_events < 0) {
    if (errno == EINTR) {
      return OkStatus();
    }

    PW_LOG_ERROR("Dispatcher failed to wait for incoming events: %s",
                 std::strerror(errno));
    return Status::Internal();
  }

  for (int i = 0; i < num_events; ++i) {
    epoll_event& event = events[static_cast<size_t>(i)];
    if (event.data.fd == wait_fd_) {
      // Consume the wake notification.
      char unused;
      ssize_t bytes_read = read(wait_fd_, &unused, 1);
      PW_CHECK_INT_EQ(
          bytes_read, 1, "Dispatcher failed to read wake notification");
      PW_DCHECK_INT_EQ(unused, kNotificationSignal);
      continue;
    }

    // Debug log for missed events.
    if (PW_LOG_LEVEL >= PW_LOG_LEVEL_DEBUG &&
        wakers_[event.data.fd].read.IsEmpty() &&
        wakers_[event.data.fd].write.IsEmpty()) {
      PW_LOG_DEBUG(
          "Received an event for registered file descriptor %d, but there is "
          "no task to wake",
          event.data.fd);
    }

    if ((event.events & (EPOLLIN | EPOLLRDHUP)) != 0) {
      wakers_[event.data.fd].read.Wake();
    }
    if ((event.events & EPOLLOUT) != 0) {
      wakers_[event.data.fd].write.Wake();
    }
  }

  return OkStatus();
}

void EpollDispatcher::DoWaitForWake() { PW_CHECK_OK(NativeWaitForWake()); }

Status EpollDispatcher::NativeRegisterFileDescriptor(int fd,
                                                     FileDescriptorType type) {
  epoll_event event;
  event.events = EPOLLET;
  event.data.fd = fd;

  if ((type & FileDescriptorType::kReadable) != 0) {
    event.events |= EPOLLIN | EPOLLRDHUP;
  }
  if ((type & FileDescriptorType::kWritable) != 0) {
    event.events |= EPOLLOUT;
  }

  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
    PW_LOG_ERROR("Failed to register epoll event: %s", std::strerror(errno));
    return Status::Internal();
  }

  return OkStatus();
}

Status EpollDispatcher::NativeUnregisterFileDescriptor(int fd) {
  epoll_event event;
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &event) == -1) {
    PW_LOG_ERROR("Failed to unregister epoll event: %s", std::strerror(errno));
    return Status::Internal();
  }
  wakers_.erase(fd);
  return OkStatus();
}

void EpollDispatcher::DoWake() {
  // Perform a write to unblock the waiting dispatcher.
  //
  // We ignore the result of the write, since nonblocking writes can
  // fail due to there already being messages in the `notify_fd_` pipe.
  // This is fine, since it means that the dispatcher thread is already queued
  // to wake up.
  write(notify_fd_, &kNotificationSignal, 1);
}

}  // namespace pw::async2
