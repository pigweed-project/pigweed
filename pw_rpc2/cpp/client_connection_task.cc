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

#include "pw_rpc2/internal/client_connection_task.h"

#include <mutex>
#include <utility>

#include "pw_log/log.h"

namespace pw::rpc2::internal {

ClientConnectionTask::ClientConnectionTask(
    EstablishedConnection established_connection, Allocator& allocator)
    : ConnectionTask(
          std::move(established_connection), allocator, EndpointRole::kClient) {
}

ClientConnectionTask::~ClientConnectionTask() {
  Teardown();
  Finish();
}

uint32_t ClientConnectionTask::NewCallId() {
  if (next_call_id_ == kMaxCallId) {
    PW_LOG_ERROR("Client call IDs exhausted; closing the connection");
    CloseConnection(Status::ResourceExhausted());
    return kMaxCallId;
  }
  return next_call_id_++;
}

void ClientConnectionTask::RequestClose(ControlFuture* future) {
  bool wake = false;
  {
    std::lock_guard lock(ControlLock());
    switch (close_state_.load(std::memory_order_relaxed)) {
      case CloseState::kClosed:
        if (future != nullptr) {
          future->ResolveLocked(OkStatus());
        }
        return;
      case CloseState::kOpen:
        close_state_.store(CloseState::kClosing, std::memory_order_release);
        wake = true;
        break;
      case CloseState::kClosing:
        break;
    }
    if (future != nullptr) {
      close_futures_.Push(*future);
    }
  }
  // Wake outside of `ControlLock()`, which is shared by every connection. The
  // caller holds a reference to this task, so it outlives the wake.
  if (wake) {
    Wake();
  }
}

ControlFuture ClientConnectionTask::Close() {
  ControlFuture future(/*listener=*/nullptr);
  RequestClose(&future);
  return future;
}

void ClientConnectionTask::CloseBlocking() {
  RequestClose(/*future=*/nullptr);
  BlockingJoin();
}

void ClientConnectionTask::ReleaseUserHandle() {
  if (user_handles_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  // The last handle is gone, so nothing can reach the connection to close it.
  RequestClose(/*future=*/nullptr);
}

void ClientConnectionTask::Finish() {
  std::lock_guard lock(ControlLock());
  if (close_state_.exchange(CloseState::kClosed, std::memory_order_acq_rel) ==
      CloseState::kClosed) {
    return;
  }
  close_futures_.ResolveAllWith(
      [](ControlFuture& future)
          PW_NO_LOCK_SAFETY_ANALYSIS { future.ResolveLocked(OkStatus()); });
}

async2::Poll<> ClientConnectionTask::DoPend(async2::Context& cx) {
  // Checked every poll rather than latched, because the close may be requested
  // by any thread at any point.
  if (close_state_.load(std::memory_order_acquire) == CloseState::kClosing) {
    CloseConnection(Status::Cancelled());
  }

  if (PollConnection(cx)) {
    cx.ReEnqueue();
  }
  if (is_closed()) {
    Finish();
    return async2::Ready();
  }
  return async2::Pending();
}

bool ClientConnectionTask::PollConnection(async2::Context& cx) {
  if (is_closed()) {
    return false;
  }
  StoreWaker(cx);

  // As in `ServerConnectionTask::PollConnection()`, `progressed` is true after
  // the loop only when all `kMaxPacketsPerPoll` iterations succeeded and the
  // task must yield via `cx.ReEnqueue()`; an early break means `PendPacket()`
  // has already registered wakers on the pending transport futures.
  bool progressed = false;
  for (int i = 0; i < kMaxPacketsPerPoll; ++i) {
    progressed = PendPacket(cx);
    if (!progressed || is_closed()) {
      break;
    }
  }
  return !is_closed() && progressed;
}

}  // namespace pw::rpc2::internal
