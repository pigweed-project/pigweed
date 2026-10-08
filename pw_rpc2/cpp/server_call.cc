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

#include "pw_rpc2/internal/server_call.h"

#include <algorithm>
#include <new>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_async2/await.h"
#include "pw_async2/poll.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/server_connection_task.h"

namespace pw::rpc2::internal {

static_assert(
    alignof(ServerCall) == alignof(std::max_align_t),
    "ServerCall must be aligned to std::max_align_t so trailing future storage "
    "at offset sizeof(ServerCall) is aligned for any standard type.");

ServerCall::ServerCall(ServerConnectionTask& connection_task,
                       uint32_t call_id,
                       const Method& method,
                       Allocator* allocator)
    : Call(connection_task, call_id, allocator),
      async2::Task(PW_ASYNC_TASK_NAME("pw::rpc2::ServerCall")),
      connection_ref_(this),
      method_(&method) {
  BeginInvocation();
}

ServerCall::~ServerCall() { PW_DCHECK(connection_ref_ == nullptr); }

Result<ServerCall*> ServerCall::Allocate(ServerConnectionTask& connection_task,
                                         uint32_t call_id,
                                         const Method& method,
                                         Allocator& allocator) {
  const size_t total_size = TotalAllocationSize(method.future_storage_size());
  void* mem =
      allocator.Allocate(allocator::Layout(total_size, alignof(ServerCall)));
  if (mem == nullptr) {
    return Status::ResourceExhausted();
  }
  return ::new (mem) ServerCall(connection_task, call_id, method, &allocator);
}

ProtocolStatus ServerCall::CommitEmplacedFuture(BoxedMethodFuture&& future,
                                                bool is_coro) {
  if (!future.is_pendable() && !future.is_complete()) {
    PW_CHECK(is_coro,
             "RPC method returned an invalid future (neither pendable nor "
             "complete)");
    future.Reset();
    return ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning;
  }
  user_future_ = std::move(future);
  FinishInvocation();
  return ProtocolStatus::kOk;
}

async2::Poll<> ServerCall::DoPend(async2::Context& cx) {
  // A closed call never consults its future again. The connection retires it,
  // which is what destroys the future and lets the handles inside it run their
  // end-of-call behavior.
  if (is_closed()) {
    FinishAndRequestRetirement();
    return async2::Ready();
  }

  // A future that is not pendable has nothing left to run: either the method
  // returned one that was already complete, or it completed on an earlier
  // poll. `BoxedMethodFuture::Pend` asserts on a non-pendable future, so check
  // rather than awaiting unconditionally.
  if (user_future_.is_pendable()) {
    BeginInvocation();
    async2::Poll<> poll = user_future_.Pend(cx);
    if (poll.IsReady() && !user_future_.is_complete() && !is_write_closed()) {
      // A coroutine aborted while running due to a nested coroutine allocation
      // failure. Queue the resource allocation error and close the write side
      // before FinishInvocation() so any deferred writer-drop packet is
      // disarmed.
      QueueError(ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning);
      CloseWrite();
    }
    FinishInvocation();
    if (poll.IsPending()) {
      return async2::Pending();
    }
  }

  FinishAndRequestRetirement();
  return async2::Ready();
}

void ServerCall::FinishAndRequestRetirement() {
  finished_ = true;
  // Retirement destroys this call's future and may free the call itself,
  // neither of which is safe from inside this poll, so the connection does it
  // from its own. `Complete()` has already woken the connection for a call
  // that was closed underneath us, but waking an awake task is free.
  if (is_attached()) {
    connection_task().Wake();
  }
}

}  // namespace pw::rpc2::internal
