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
#pragma once

#include "pw_allocator/allocator.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_async2/waker.h"
#include "pw_channel/packet_channel.h"

namespace pw::channel {
namespace internal {

class BasicPacketProxyTask;

class BasicProxy {
 public:
  constexpr Allocator& allocator() { return allocator_; }

  constexpr bool IsConnected() const { return state_ == State::kConnected; }

  /// Resets the proxy. Returns `Ready()` when the proxy's tasks have completed
  /// and the proxy is ready to start again.
  ///
  /// Only one task may call `Reset` at a time. `Reset` must return `Ready`
  /// before another task may call it.
  async2::Poll<> Reset(async2::Context& context);

 protected:
  constexpr explicit BasicProxy(Allocator& allocator) : allocator_(allocator) {}

  constexpr void Connect() { state_ = State::kConnected; }

 private:
  /// Tracks whether the proxy is connected in one or both directions.
  ///
  /// The proxy may only be `Run` when it is `kDisconnected`, which will start
  /// both host and controller proxy tasks and move the state to `kConnected`.
  /// When either tasks finish, or the proxy is `Reset`, it will move to
  /// `kDisconnecting`. Once both tasks have finished it will move back to
  /// `kDisconnected`.
  enum class State {
    kConnected,
    kResetPending,
    kDisconnecting,
    kDisconnected,
  };

  // PacketTasks call PendCompleted and DisconnectTask.
  friend class ::pw::channel::internal::BasicPacketProxyTask;

  // Checks if the proxy is stopped and registers a waker so tasks can be
  // cancelled without waiting for packets to arrive.
  async2::Poll<> PendCompleted(async2::Context& context) {
    if (!IsConnected()) {
      return async2::Ready();
    }
    if (!PW_ASYNC_TRY_STORE_WAKER(
            context, cancel_task_1_, "proxy reset signal")) {
      PW_ASYNC_STORE_WAKER(context, cancel_task_2_, "proxy reset signal");
    }
    return async2::Pending();
  }

  // Called by both tasks to indicate that they have finished running.
  void DisconnectTask();

  // Used to requeue a task that calls `Reset` when the proxy disconnects.
  async2::Waker reset_waker_;

  // Wakers to cancel the two tasks so they don't have to wait for a packet when
  // `Reset` is called.
  async2::Waker cancel_task_1_;
  async2::Waker cancel_task_2_;

  State state_ = State::kDisconnected;

  Allocator& allocator_;
};

}  // namespace internal
}  // namespace pw::channel
