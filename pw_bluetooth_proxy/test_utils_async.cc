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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_status/try.h"

namespace pw::bluetooth::proxy {

////////////////////////////////////////////////////////////////////////////////
// ProxyHostTestDispatcher methods.

ProxyHostTestDispatcher::~ProxyHostTestDispatcher() {
  PW_CHECK(!thread_.has_value(),
           "The dispatcher thread must be joined before test completes.");
}

void ProxyHostTestDispatcher::StartOnCurrentThread(ProxyHost& proxy) {
  proxy_ = &proxy;
  {
    std::lock_guard lock(mutex_);
    PW_CHECK(!id_.has_value());
    id_ = this_thread::get_id();
  }
  PW_CHECK_OK(proxy_->SetDispatcher(dispatcher_));
}

bool ProxyHostTestDispatcher::IsRunningOnThisThread() const {
  std::lock_guard lock(mutex_);
  PW_CHECK(id_.has_value());
  return this_thread::get_id() == *id_;
}

void ProxyHostTestDispatcher::Run() {
  if (IsRunningOnThisThread()) {
    dispatcher_.RunUntilStalled();
  } else {
    // Clear any latch set by a previously completed run of the dispatcher.
    std::ignore = tx_.try_acquire();
    rx_.release();
    tx_.acquire();
  }
}

#if PW_THREAD_JOINING_ENABLED

void ProxyHostTestDispatcher::StartOnNewThread(ProxyHost& proxy) {
  proxy_ = &proxy;
  thread_.emplace(context_.options(), [this]() {
    {
      std::lock_guard lock(mutex_);
      PW_CHECK(!id_.has_value());
      id_ = this_thread::get_id();
    }
    PW_CHECK_OK(proxy_->SetDispatcher(dispatcher_));
    tx_.release();
    while (true) {
      rx_.acquire();
      bool stop = false;
      {
        std::lock_guard lock(mutex_);
        stop = !id_.has_value();
      }
      dispatcher_.RunUntilStalled();
      tx_.release();
      if (stop) {
        break;
      }
    }
  });

  // Make sure the dispatcher is set before returning.
  tx_.acquire();
}

void ProxyHostTestDispatcher::JoinThread() {
  if (!thread_.has_value()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    id_.reset();
  }
  rx_.release();
  thread_->join();
  thread_.reset();
}

#endif  // PW_THREAD_JOINING_ENABLED

////////////////////////////////////////////////////////////////////////////////
// ProxyHostTest methods.

void ProxyHostTest::StartDispatcherOnCurrentThread(ProxyHost& proxy) {
  dispatcher_.StartOnCurrentThread(proxy);
}

void ProxyHostTest::StartDispatcherOnNewThread(ProxyHost& proxy) {
  dispatcher_.StartOnNewThread(proxy);
}

void ProxyHostTest::RunDispatcher() { dispatcher_.Run(); }

void ProxyHostTest::JoinDispatcherThread() { dispatcher_.JoinThread(); }

}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
