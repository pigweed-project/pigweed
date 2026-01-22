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

#include "pw_channel/internal/basic_proxy.h"

#include "pw_assert/check.h"

namespace pw::channel::internal {

async2::Poll<> BasicProxy::Reset(async2::Context& context) {
  if (state_ == State::kDisconnected) {
    return async2::Ready();
  }

  PW_CHECK(state_ == State::kConnected,
           "Only one task can call Reset at a time");
  state_ = State::kResetPending;
  PW_ASYNC_STORE_WAKER(context, reset_waker_, "waiting for tasks to complete");
  cancel_task_1_.Wake();  // Wake the tasks so they can disconnect.
  cancel_task_2_.Wake();
  return async2::Pending();
}

void BasicProxy::DisconnectTask() {
  switch (state_) {
    case State::kConnected:
    case State::kResetPending:
      state_ = State::kDisconnecting;
      cancel_task_1_.Wake();  // Wake the other task so it can disconnect.
      cancel_task_2_.Wake();
      break;
    case State::kDisconnecting:
      state_ = State::kDisconnected;
      reset_waker_.Wake();
      break;
    case State::kDisconnected:
      break;
  }
}

}  // namespace pw::channel::internal
