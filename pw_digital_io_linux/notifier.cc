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

#include "pw_digital_io_linux/notifier.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>

#include "log_errno.h"
#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_status/status.h"

namespace pw::digital_io {

namespace {

using pw::OkStatus;

constexpr void* kCancelToken = nullptr;

// The max number of that the notifier will process in a single iteration.
inline constexpr auto kMaxEventsPerWake = 16;

}  // namespace

pw::Result<std::shared_ptr<LinuxGpioNotifier>> LinuxGpioNotifier::Create() {
  // Create file descriptors
  int raw_epoll_fd = epoll_create1(0);
  if (raw_epoll_fd < 0) {
    LOG_ERROR_WITH_ERRNO("Failed to initialize epoll descriptor:", errno);
    return pw::Status::Internal();
  }
  auto epoll_fd = OwnedFd(raw_epoll_fd);

  int raw_event_fd = eventfd(0, 0);
  if (raw_event_fd < 0) {
    LOG_ERROR_WITH_ERRNO("Failed to initialize event descriptor:", errno);
    return pw::Status::Internal();
  }
  auto event_fd = OwnedFd(raw_event_fd);

  // Attempt to register event_fd with epoll_fd
  epoll_event event = {
      .events = EPOLLIN,
      .data = {.ptr = kCancelToken},
  };
  if (epoll_ctl(epoll_fd.fd(), EPOLL_CTL_ADD, event_fd.fd(), &event) != 0) {
    // There is no reason this should ever fail, except for a bug!
    LOG_ERROR_WITH_ERRNO("Failed to add cancel event to epoll descriptor:",
                         errno);
    return pw::Status::Internal();
  }

  // Initialization succeeded - create the object.
  return std::shared_ptr<LinuxGpioNotifier>(
      new LinuxGpioNotifier(std::move(epoll_fd), std::move(event_fd)));
}

LinuxGpioNotifier::~LinuxGpioNotifier() {
  PW_CHECK_INT_EQ(  // Crash OK: prevent use-after-free via registered lines.
      registered_line_count_,
      0,
      "Destroying notifier with registered lines");
  // fds closed automatically
}

pw::Status LinuxGpioNotifier::RegisterLine(
    int fd, LinuxGpioNotifier::Handler& handler) {
  // Register for event notifications. Note that it's not clear from the
  // documentation if EPOLLIN or EPOLLPRI is needed here, but EPOLLPRI shows up
  // in all the examples online, and EPOLLIN is useful for testing.
  epoll_event event = {
      .events = EPOLLIN | EPOLLPRI,
      .data = {.ptr = &handler},
  };
  if (epoll_ctl(epoll_fd_.fd(), EPOLL_CTL_ADD, fd, &event) != 0) {
    switch (errno) {
      case EBADF:
        PW_LOG_WARN("The fd [%d] is invalid", fd);
        return pw::Status::InvalidArgument();
      case EEXIST:
        PW_LOG_WARN(
            "The fd [%d] is already registered with epoll descriptor [%d]",
            fd,
            epoll_fd_.fd());
        return pw::Status::FailedPrecondition();
      case ENOSPC:
        PW_LOG_WARN("No space to add fd [%d] to epoll descriptor [%d]",
                    fd,
                    epoll_fd_.fd());
        return pw::Status::ResourceExhausted();
    }
    // Other errors are likely the result of bugs and should never happen.
    LOG_ERROR_WITH_ERRNO("Failed to add fd [%d] to epoll descriptor [%d]:",
                         errno,
                         fd,
                         epoll_fd_.fd());
    return pw::Status::Internal();
  }
  ++registered_line_count_;
  return OkStatus();
}

pw::Status LinuxGpioNotifier::UnregisterLine(int fd) {
  // Unregister from event notifications.
  epoll_event unused{};  // See BUGS under epoll_ctl(2).
  if (epoll_ctl(epoll_fd_.fd(), EPOLL_CTL_DEL, fd, &unused) != 0) {
    switch (errno) {
      case ENOENT:
        // The file descriptor was not registered.
        return pw::Status::NotFound();
      case EBADF:
        PW_LOG_WARN("The fd [%d] is invalid", fd);
        return pw::Status::InvalidArgument();
    }
    // Other errors are likely the result of bugs and should never happen.
    LOG_ERROR_WITH_ERRNO("Failed to remove fd [%d] from epoll descriptor [%d]:",
                         errno,
                         fd,
                         epoll_fd_.fd());
    return pw::Status::Internal();
  }
  --registered_line_count_;
  return OkStatus();
}

void LinuxGpioNotifier::CancelWait() {
  uint64_t value = 1;
  // Note this is used in tests only, and failure to cancel will hang the test
  // or leak a thread - depending on if the test tries to join the thread.
  ssize_t result = cancel_event_fd_.write(&value, sizeof(value));
  PW_DCHECK_INT_EQ(result,
                   sizeof(value),
                   "Failed to write cancel event: " ERRNO_FORMAT_STRING,
                   ERRNO_FORMAT_ARGS(errno));
}

pw::Result<unsigned int> LinuxGpioNotifier::WaitForEvents(int timeout_ms) {
  // Block until there is at least 1 file descriptor with an event.
  std::array<epoll_event, kMaxEventsPerWake> events{};
  int event_count;
  for (;;) {
    errno = 0;
    event_count =
        epoll_wait(epoll_fd_.fd(), events.data(), events.size(), timeout_ms);
    if (event_count > 0) {
      break;
    }
    if (event_count == 0) {
      return pw::Status::DeadlineExceeded();
    }
    if (errno == EINTR) {
      // Call was interrupted by a signal to the thread. Restart it.
      // NOTE: We don't attempt to update timeout_ms.
      continue;
    }
    LOG_CRITICAL_WITH_ERRNO("Failed to wait on epoll descriptor:", errno);
    return pw::Status::Internal();
  }

  // Process any lines that have events. Note that if event_count =
  // kMaxEvents and there are more events waiting, we will get them on the
  // next loop.
  for (size_t i = 0; i < static_cast<size_t>(event_count); i++) {
    if (events[i].data.ptr == kCancelToken) {
      return pw::Status::Cancelled();
    }
    static_cast<Handler*>(events[static_cast<size_t>(i)].data.ptr)
        ->HandleEvents();
  }

  // Must be positive due to (event_count > 0) check above.
  return static_cast<unsigned int>(event_count);
}

void LinuxGpioNotifier::Run() {
  for (;;) {
    auto status = WaitForEvents(-1);
    if (!status.ok()) {
      break;
    }
  }
}

}  // namespace pw::digital_io
