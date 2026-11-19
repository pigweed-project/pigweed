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
#pragma once

#include <cstddef>
#include <cstdint>

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"

namespace pw::async2::size_report {

class MockTask : public Task {
 public:
  bool should_complete = false;
  int polled = 0;
  Waker last_waker;

 private:
  Poll<> DoPend(Context& cx) override {
    ++polled;
    PW_ASYNC_STORE_WAKER(cx, last_waker, "MockTask is waiting for last_waker");
    if (should_complete) {
      return Ready();
    }
    return Pending();
  }
};

class BaselineFutureProvider;

// Minimal FutureCore-backed future used by the pw_async2 core baseline so that
// core future runtime machinery (FutureCore, BaseFutureList, PW_AWAIT) is
// included without pulling in higher-level future types like ValueFuture.
class BaselineFuture {
 public:
  using value_type = void;

  constexpr BaselineFuture() = default;
  BaselineFuture(const BaselineFuture&) = delete;
  BaselineFuture& operator=(const BaselineFuture&) = delete;
  BaselineFuture(BaselineFuture&&) = default;
  BaselineFuture& operator=(BaselineFuture&&) = default;

  ~BaselineFuture() { core_.Unlist(); }

  void Cancel() { core_.Reset(); }

  Poll<> Pend(Context& cx) { return core_.DoPend(*this, cx); }

  [[nodiscard]] bool is_pendable() const { return core_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return core_.is_complete(); }

 private:
  friend class ::pw::async2::FutureCore;
  friend class BaselineFutureProvider;

  static constexpr const char kWaitReason[] = "BaselineFuture";

  explicit BaselineFuture(FutureState::Pending)
      : core_(FutureState::kPending) {}

  Poll<> DoPend(Context&) {
    if (core_.is_ready()) {
      return Ready();
    }
    return Pending();
  }

  FutureCore core_;
};

class BaselineFutureProvider {
 public:
  BaselineFuture Get() {
    BaselineFuture future(FutureState::kPending);
    list_.Push(future.core_);
    return future;
  }

  BaselineFuture GetSingle() {
    BaselineFuture future(FutureState::kPending);
    list_.PushRequireEmpty(future.core_);
    return future;
  }

  void ResolveOne() { list_.ResolveOneIfAvailable(); }

  void ResolveAll() { list_.ResolveAll(); }

  size_t size() const { return list_.size(); }

 private:
  BaseFutureList list_;
};

BasicDispatcher& GetDispatcher();

// Establishes a baseline system already using core pw_async2 primitives:
// BasicDispatcher, Task (including Deregister), FuncTask, Waker, FutureCore,
// BaseFutureList, and PW_AWAIT.
int SetAsync2Baseline(volatile uint32_t& mask);

}  // namespace pw::async2::size_report
