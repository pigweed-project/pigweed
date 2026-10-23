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
#pragma once

#include <cstddef>
#include <unordered_map>

#include "pw_assert/assert.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/runnable_dispatcher.h"
#include "pw_async2/waker.h"
#include "pw_status/status.h"

namespace pw::async2 {

/// @submodule{pw_async2,dispatchers}

class EpollDispatcher final : public RunnableDispatcher {
 public:
  EpollDispatcher() { PW_ASSERT_OK(NativeInit()); }
  ~EpollDispatcher() override { Terminate(); }

  Status NativeInit();

  enum FileDescriptorType {
    kReadable = 1 << 0,
    kWritable = 1 << 1,
    kReadWrite = kReadable | kWritable,
  };

  Status NativeRegisterFileDescriptor(int fd, FileDescriptorType type);
  Status NativeUnregisterFileDescriptor(int fd);

  Waker& NativeAddReadWakerForFileDescriptor(int fd) {
    return wakers_[fd].read;
  }

  Waker& NativeAddWriteWakerForFileDescriptor(int fd) {
    return wakers_[fd].write;
  }

 private:
  friend class ::pw::async2::Dispatcher;

  static constexpr size_t kMaxEventsToProcessAtOnce = 5;

  struct ReadWriteWaker {
    Waker read;
    Waker write;
  };

  void DoWake() override;

  void DoWaitForWake() override;

  Status NativeWaitForWake();
  void NativeFindAndWakeFileDescriptor(int fd, FileDescriptorType type);

  int epoll_fd_;
  int notify_fd_;
  int wait_fd_;

  std::unordered_map<int, ReadWriteWaker> wakers_;
};

/// @endsubmodule

}  // namespace pw::async2
